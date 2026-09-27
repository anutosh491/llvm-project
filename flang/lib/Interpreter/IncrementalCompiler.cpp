//===-- IncrementalCompiler.cpp - Compile one Fortran cell ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "IncrementalCompiler.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "flang/Frontend/CompilerInstance.h"
#include "flang/Frontend/FrontendActions.h"
#include "flang/Frontend/FrontendOptions.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"
#include <vector>

namespace Fortran::interpreter {
namespace {

class CaptureLLVMMLIRAction : public frontend::CodeGenAction {
public:
  CaptureLLVMMLIRAction()
      : CodeGenAction(frontend::BackendActionTy::Backend_EmitLL) {}

  std::unique_ptr<mlir::MLIRContext> takeContext() {
    return std::move(mlirCtx);
  }
  mlir::OwningOpRef<mlir::ModuleOp> takeModule() {
    return std::move(mlirModule);
  }

private:
  void executeAction() override { generateLLVMIR(); }
};

struct PreparedCell {
  std::string source;
  std::string stateModule;
  std::string entryPoint;
  llvm::SmallVector<std::string> declaredNames;
};

bool startsWithKeyword(llvm::StringRef line, llvm::StringRef keyword) {
  if (!line.starts_with(keyword))
    return false;
  return line.size() == keyword.size() || !llvm::isAlnum(line[keyword.size()]);
}

bool isCompleteProgramUnit(llvm::StringRef source) {
  llvm::SmallVector<llvm::StringRef> lines;
  source.split(lines, '\n');
  for (llvm::StringRef line : lines) {
    std::string loweredStorage = line.trim().lower();
    llvm::StringRef lowered{loweredStorage};
    if (lowered.empty() || lowered.starts_with("!"))
      continue;
    if (startsWithKeyword(lowered, "program") ||
        startsWithKeyword(lowered, "module") ||
        startsWithKeyword(lowered, "subroutine") ||
        startsWithKeyword(lowered, "function") ||
        lowered.starts_with("block data") || lowered.contains(" function "))
      return true;
    return false;
  }
  return false;
}

bool isStandaloneProcedure(llvm::StringRef source) {
  llvm::SmallVector<llvm::StringRef> lines;
  source.split(lines, '\n');
  for (llvm::StringRef line : lines) {
    std::string loweredStorage = line.trim().lower();
    llvm::StringRef lowered{loweredStorage};
    if (lowered.empty() || lowered.starts_with("!"))
      continue;
    return startsWithKeyword(lowered, "subroutine") ||
        startsWithKeyword(lowered, "function") ||
        lowered.contains(" function ");
  }
  return false;
}

bool unwrapMainProgram(llvm::StringRef source, std::string &body) {
  llvm::SmallVector<llvm::StringRef> lines;
  source.split(lines, '\n');

  std::size_t first = 0;
  while (first < lines.size() &&
      (lines[first].trim().empty() || lines[first].trim().starts_with("!")))
    ++first;
  if (first == lines.size())
    return false;

  std::string firstLoweredStorage = lines[first].trim().lower();
  if (!startsWithKeyword(firstLoweredStorage, "program"))
    return false;

  std::size_t last = lines.size();
  while (last > first + 1 &&
      (lines[last - 1].trim().empty() ||
          lines[last - 1].trim().starts_with("!")))
    --last;
  if (last == first + 1)
    return false;

  std::string lastLoweredStorage = lines[last - 1].trim().lower();
  llvm::StringRef lastLowered{lastLoweredStorage};
  if (lastLowered != "end" && !lastLowered.starts_with("end program"))
    return false;

  llvm::raw_string_ostream output{body};
  for (std::size_t index = first + 1; index + 1 < last; ++index)
    output << lines[index] << '\n';
  return true;
}

bool isSpecificationLine(llvm::StringRef line) {
  std::string loweredStorage = line.trim().lower();
  llvm::StringRef lowered{loweredStorage};
  if (lowered.empty() || lowered.starts_with("!"))
    return true;
  static constexpr llvm::StringLiteral prefixes[] = {"integer", "real",
      "complex", "logical", "character", "double precision", "type(",
      "type ::", "class(", "use ", "implicit ", "parameter ", "dimension ",
      "save "};
  return llvm::any_of(prefixes,
      [&](llvm::StringRef prefix) { return lowered.starts_with(prefix); });
}

bool isExpressionCell(llvm::ArrayRef<llvm::StringRef> lines) {
  llvm::StringRef expression;
  for (llvm::StringRef line : lines) {
    line = line.trim();
    if (line.empty() || line.starts_with("!"))
      continue;
    if (!expression.empty())
      return false;
    expression = line;
  }
  if (expression.empty() || expression.contains('='))
    return false;
  std::string loweredStorage = expression.lower();
  llvm::StringRef lowered{loweredStorage};
  static constexpr llvm::StringLiteral statementPrefixes[] = {"allocate",
      "associate", "block", "call", "close", "critical", "cycle", "deallocate",
      "do", "error stop", "exit", "flush", "forall", "if", "open", "print",
      "read", "return", "select", "stop", "sync", "where", "write"};
  return !llvm::any_of(statementPrefixes, [&](llvm::StringRef prefix) {
    return startsWithKeyword(lowered, prefix);
  });
}

void appendDeclaredNames(
    llvm::StringRef line, llvm::SmallVectorImpl<std::string> &names) {
  line = line.take_front(line.find('!'));
  std::size_t separator = line.find("::");
  if (separator == llvm::StringRef::npos)
    return;

  llvm::StringRef entities = line.drop_front(separator + 2);
  std::size_t begin = 0;
  unsigned parentheses = 0;
  for (std::size_t index = 0; index <= entities.size(); ++index) {
    char character = index == entities.size() ? ',' : entities[index];
    if (character == '(') {
      ++parentheses;
    } else if (character == ')' && parentheses) {
      --parentheses;
    } else if (character == ',' && !parentheses) {
      llvm::StringRef entity = entities.slice(begin, index).trim();
      std::size_t nameLength = 0;
      while (nameLength < entity.size() &&
          (llvm::isAlnum(entity[nameLength]) || entity[nameLength] == '_'))
        ++nameLength;
      if (nameLength)
        names.push_back(entity.take_front(nameLength).lower());
      begin = index + 1;
    }
  }
}

std::string getProcedureName(llvm::ArrayRef<llvm::StringRef> lines) {
  for (llvm::StringRef line : lines) {
    std::string loweredStorage = line.trim().lower();
    llvm::StringRef lowered{loweredStorage};
    if (lowered.empty() || lowered.starts_with("!"))
      continue;

    for (llvm::StringRef keyword :
        {llvm::StringRef{"subroutine"}, llvm::StringRef{"function"}}) {
      std::size_t position = lowered.find(keyword);
      if (position == llvm::StringRef::npos ||
          (position && llvm::isAlnum(lowered[position - 1])))
        continue;
      llvm::StringRef after = lowered.drop_front(position + keyword.size());
      after = after.ltrim();
      std::size_t nameLength = 0;
      while (nameLength < after.size() &&
          (llvm::isAlnum(after[nameLength]) || after[nameLength] == '_'))
        ++nameLength;
      return after.take_front(nameLength).str();
    }
    break;
  }
  return {};
}

void emitStateUse(llvm::raw_ostream &output, llvm::StringRef module,
    llvm::ArrayRef<std::string> shadowedNames, std::uint64_t cellId) {
  if (module.empty())
    return;
  output << "  use " << module;
  for (auto [index, name] : llvm::enumerate(shadowedNames))
    output << ", flang_repl_old_" << cellId << '_' << index << " => " << name;
  output << '\n';
}

llvm::SmallVector<std::string> getShadowedNames(
    llvm::ArrayRef<std::string> declaredNames,
    llvm::ArrayRef<std::string> visibleNames) {
  llvm::SmallVector<std::string> shadowedNames;
  for (const std::string &name : declaredNames)
    if (llvm::is_contained(visibleNames, name))
      shadowedNames.push_back(name);
  return shadowedNames;
}

PreparedCell prepareCellSource(llvm::StringRef code, std::uint64_t cellId,
    llvm::StringRef previousStateModule,
    llvm::ArrayRef<std::string> visibleNames) {
  std::string mainProgramBody;
  if (unwrapMainProgram(code, mainProgramBody))
    return prepareCellSource(
        mainProgramBody, cellId, previousStateModule, visibleNames);

  if (isStandaloneProcedure(code)) {
    llvm::SmallVector<llvm::StringRef> lines;
    code.split(lines, '\n');
    std::string procedureName = getProcedureName(lines);
    llvm::SmallVector<std::string> declaredNames;
    if (!procedureName.empty())
      declaredNames.push_back(procedureName);
    llvm::SmallVector<std::string> shadowedNames =
        getShadowedNames(declaredNames, visibleNames);

    std::string moduleName =
        llvm::formatv("flang_repl_state_{0}", cellId).str();
    std::string generated;
    llvm::raw_string_ostream output{generated};
    output << "module " << moduleName << '\n';
    emitStateUse(output, previousStateModule, shadowedNames, cellId);
    output << "contains\n";
    std::size_t procedureEnd = lines.size();
    for (std::size_t index = 0; index < lines.size(); ++index) {
      std::string loweredStorage = lines[index].trim().lower();
      llvm::StringRef lowered{loweredStorage};
      if (lowered.starts_with("end function") ||
          lowered.starts_with("endfunction") ||
          lowered.starts_with("end subroutine") ||
          lowered.starts_with("endsubroutine")) {
        procedureEnd = index + 1;
        break;
      }
    }
    for (std::size_t index = 0; index < procedureEnd; ++index)
      output << "  " << lines[index] << '\n';
    llvm::ArrayRef<llvm::StringRef> trailingLines =
        llvm::ArrayRef<llvm::StringRef>{lines}.drop_front(procedureEnd);
    std::string entryPoint;
    if (llvm::any_of(trailingLines, [](llvm::StringRef line) {
          return !line.trim().empty() && !line.trim().starts_with("!");
        })) {
      entryPoint = llvm::formatv("flang_repl_cell_{0}", cellId).str();
      output << "  subroutine " << entryPoint << "() bind(c, name=\""
             << entryPoint << "\")\n";
      if (isExpressionCell(trailingLines)) {
        for (llvm::StringRef line : trailingLines)
          if (!line.trim().empty() && !line.trim().starts_with("!"))
            output << "    print *, " << line.trim() << '\n';
      } else {
        for (llvm::StringRef line : trailingLines)
          output << "    " << line << '\n';
      }
      output << "  end subroutine\n";
    }
    output << "end module " << moduleName << '\n';
    return {std::move(generated), std::move(moduleName), std::move(entryPoint),
        std::move(declaredNames)};
  }
  if (isCompleteProgramUnit(code))
    return {code.str(), {}, {}, {}};

  llvm::SmallVector<llvm::StringRef> lines;
  code.split(lines, '\n');
  std::size_t firstExecutable = lines.size();
  bool hasSpecification = false;
  for (std::size_t index = 0; index < lines.size(); ++index) {
    if (isSpecificationLine(lines[index])) {
      if (!lines[index].trim().empty() && !lines[index].trim().starts_with("!"))
        hasSpecification = true;
      continue;
    }
    firstExecutable = index;
    break;
  }

  std::string procedureName =
      llvm::formatv("flang_repl_cell_{0}", cellId).str();
  std::string generated;
  llvm::raw_string_ostream output{generated};
  if (hasSpecification) {
    llvm::SmallVector<std::string> declaredNames;
    for (std::size_t index = 0; index < firstExecutable; ++index)
      appendDeclaredNames(lines[index], declaredNames);
    llvm::SmallVector<std::string> shadowedNames =
        getShadowedNames(declaredNames, visibleNames);

    std::string moduleName =
        llvm::formatv("flang_repl_state_{0}", cellId).str();
    output << "module " << moduleName << '\n';
    emitStateUse(output, previousStateModule, shadowedNames, cellId);
    for (std::size_t index = 0; index < firstExecutable; ++index)
      output << "  " << lines[index] << '\n';
    if (firstExecutable != lines.size()) {
      output << "contains\n"
             << "  subroutine " << procedureName << "() bind(c, name=\""
             << procedureName << "\")\n";
      for (std::size_t index = firstExecutable; index < lines.size(); ++index)
        output << "    " << lines[index] << '\n';
      output << "  end subroutine\n";
    }
    output << "end module " << moduleName << '\n';
    return {std::move(generated), std::move(moduleName),
        firstExecutable == lines.size() ? "" : procedureName,
        std::move(declaredNames)};
  }

  output << "subroutine " << procedureName << "() bind(c, name=\""
         << procedureName << "\")\n";
  if (!previousStateModule.empty())
    output << "  use " << previousStateModule << '\n';
  if (isExpressionCell(lines)) {
    for (llvm::StringRef line : lines)
      if (!line.trim().empty() && !line.trim().starts_with("!"))
        output << "  print *, " << line.trim() << '\n';
  } else {
    for (llvm::StringRef line : lines)
      output << "  " << line << '\n';
  }
  output << "end subroutine\n";
  return {std::move(generated), {}, std::move(procedureName), {}};
}

} // namespace

IncrementalCompiler::IncrementalCompiler(
    std::unique_ptr<frontend::CompilerInstance> compiler)
    : invocation{std::make_shared<frontend::CompilerInvocation>(
          compiler->getInvocation())} {
  if (invocation->getModuleDir() == ".") {
    llvm::SmallString<128> directory;
    initializationError =
        llvm::sys::fs::createUniqueDirectory("flang-repl", directory);
    if (!initializationError) {
      sessionModuleDirectory = directory.str().str();
      invocation->getModuleDir() = sessionModuleDirectory;
      invocation->getFortranOpts().searchDirectories.push_back(
          sessionModuleDirectory);
      ownsModuleDirectory = true;
    }
  }
}

IncrementalCompiler::~IncrementalCompiler() {
  if (ownsModuleDirectory)
    llvm::sys::fs::remove_directories(sessionModuleDirectory);
}

llvm::Expected<CellArtifact> IncrementalCompiler::compile(
    llvm::StringRef code, std::uint64_t cellId) {
  if (initializationError)
    return llvm::errorCodeToError(initializationError);
  std::string inputName =
      llvm::formatv("flang-repl-cell-{0}.f90", cellId).str();
  PreparedCell prepared = prepareCellSource(
      code, cellId, latestStateModule, visibleNamesHistory.back());
  std::unique_ptr<llvm::MemoryBuffer> input =
      llvm::MemoryBuffer::getMemBufferCopy(prepared.source, inputName);

  auto cellInvocation =
      std::make_shared<frontend::CompilerInvocation>(*invocation);
  auto &inputs = cellInvocation->getFrontendOpts().inputs;
  inputs.clear();
  inputs.emplace_back(input.get(), frontend::Language::Fortran);

  frontend::CompilerInstance compiler{std::move(cellInvocation)};
  compiler.createDiagnostics();

  CaptureLLVMMLIRAction action;
  if (!compiler.executeAction(action))
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
        "compilation of cell %llu failed",
        static_cast<unsigned long long>(cellId));

  // Context must be moved before Module is constructed in CellArtifact. The
  // artifact declares Context before Module, so Module is destroyed first.
  std::unique_ptr<mlir::MLIRContext> context = action.takeContext();
  mlir::OwningOpRef<mlir::ModuleOp> module = action.takeModule();
  if (!context || !module)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
        "compilation of cell %llu produced no MLIR module",
        static_cast<unsigned long long>(cellId));

  module->setSymName(llvm::formatv("flang_repl_module_{0}", cellId).str());

  CellArtifact artifact{cellId, std::move(inputName),
      std::move(prepared.source), std::move(context), std::move(module)};

  artifact.setEntryPoint(prepared.entryPoint);
  if (!prepared.stateModule.empty()) {
    artifact.setStateTransition(latestStateModule, prepared.stateModule);
    latestStateModule = prepared.stateModule;
    std::vector<std::string> visibleNames = visibleNamesHistory.back();
    for (const std::string &name : prepared.declaredNames)
      if (!llvm::is_contained(visibleNames, name))
        visibleNames.push_back(name);
    visibleNamesHistory.push_back(std::move(visibleNames));
  }
  return std::move(artifact);
}

void IncrementalCompiler::undo(const CellArtifact &cell) {
  if (!cell.getStateModule().empty()) {
    latestStateModule = cell.getPreviousStateModule().str();
    assert(visibleNamesHistory.size() > 1);
    visibleNamesHistory.pop_back();
  }
}

} // namespace Fortran::interpreter
