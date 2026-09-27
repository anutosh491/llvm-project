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

PreparedCell prepareCellSource(llvm::StringRef code, std::uint64_t cellId,
    llvm::StringRef previousStateModule) {
  if (isStandaloneProcedure(code)) {
    std::string moduleName =
        llvm::formatv("flang_repl_state_{0}", cellId).str();
    std::string generated;
    llvm::raw_string_ostream output{generated};
    output << "module " << moduleName << '\n';
    if (!previousStateModule.empty())
      output << "  use " << previousStateModule << '\n';
    output << "contains\n";
    llvm::SmallVector<llvm::StringRef> lines;
    code.split(lines, '\n');
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
    return {std::move(generated), std::move(moduleName), std::move(entryPoint)};
  }
  if (isCompleteProgramUnit(code)) {
    llvm::SmallVector<llvm::StringRef> lines;
    code.split(lines, '\n');
    for (llvm::StringRef line : lines) {
      std::string loweredStorage = line.trim().lower();
      llvm::StringRef lowered{loweredStorage};
      if (lowered.empty() || lowered.starts_with("!"))
        continue;
      return {code.str(), {},
          startsWithKeyword(lowered, "program") ? "_QQmain" : ""};
    }
  }

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
    std::string moduleName =
        llvm::formatv("flang_repl_state_{0}", cellId).str();
    output << "module " << moduleName << '\n';
    if (!previousStateModule.empty())
      output << "  use " << previousStateModule << '\n';
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
        firstExecutable == lines.size() ? "" : procedureName};
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
  return {std::move(generated), {}, std::move(procedureName)};
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
  PreparedCell prepared = prepareCellSource(code, cellId, latestStateModule);
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
  }
  return std::move(artifact);
}

void IncrementalCompiler::undo(const CellArtifact &cell) {
  if (!cell.getStateModule().empty())
    latestStateModule = cell.getPreviousStateModule().str();
}

} // namespace Fortran::interpreter
