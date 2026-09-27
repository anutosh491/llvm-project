//===-- Interpreter.cpp - Incremental Fortran compilation -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "flang/Interpreter/Interpreter.h"
#include "IncrementalCompiler.h"
#include "flang/Frontend/CompilerInstance.h"
#include "flang/Frontend/CompilerInvocation.h"
#include "llvm/TargetParser/Host.h"

namespace Fortran::interpreter {

llvm::Expected<std::unique_ptr<frontend::CompilerInstance>>
IncrementalCompilerBuilder::create() const {
  auto invocation = std::make_shared<frontend::CompilerInvocation>();
  invocation->setArgv0(executablePath.c_str());
  invocation->setDefaultFortranOpts();
  invocation->getTargetOpts().triple = targetTriple.empty()
      ? llvm::Triple::normalize(llvm::sys::getProcessTriple())
      : llvm::Triple::normalize(targetTriple);
  invocation->getModuleDir() = moduleDirectory;
  for (const std::string &directory : intrinsicModuleDirectories)
    invocation->getPreprocessorOpts()
        .searchDirectoriesFromIntrModPath.push_back(directory);
  invocation->setFortranOpts();
  invocation->setDefaultPredefinitions();
  invocation->setLoweringOptions();

  auto compiler =
      std::make_unique<frontend::CompilerInstance>(std::move(invocation));
  compiler->createDiagnostics();
  return std::move(compiler);
}

Interpreter::Interpreter(std::unique_ptr<IncrementalCompiler> compiler,
    std::unique_ptr<IncrementalExecutor> executor)
    : compiler{std::move(compiler)}, executor{std::move(executor)} {}

Interpreter::~Interpreter() = default;

llvm::Expected<std::unique_ptr<Interpreter>> Interpreter::create(
    std::unique_ptr<frontend::CompilerInstance> compiler,
    std::unique_ptr<IncrementalExecutor> executor) {
  if (!compiler)
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(), "missing compiler instance");
  return std::unique_ptr<Interpreter>{new Interpreter{
      std::make_unique<IncrementalCompiler>(std::move(compiler)),
      std::move(executor)}};
}

llvm::Expected<CellArtifact &> Interpreter::compile(llvm::StringRef code) {
  std::uint64_t id = cells.empty() ? 1 : cells.back().getId() + 1;
  llvm::Expected<CellArtifact> cell = compiler->compile(code, id);
  if (!cell)
    return cell.takeError();
  cells.emplace_back(std::move(*cell));
  return cells.back();
}

llvm::Error Interpreter::execute(CellArtifact &cell) {
  if (!executor)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
        "no incremental executor is configured");
  if (cell.wasExecuted())
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
        "cell %llu has already been executed",
        static_cast<unsigned long long>(cell.getId()));
  if (llvm::Error error = executor->addModule(cell))
    return error;
  if (llvm::Error error = executor->execute(cell)) {
    if (llvm::Error removeError = executor->removeModule(cell))
      return llvm::joinErrors(std::move(error), std::move(removeError));
    return error;
  }
  cell.markExecuted();
  return llvm::Error::success();
}

llvm::Error Interpreter::compileAndExecute(llvm::StringRef code) {
  llvm::Expected<CellArtifact &> cell = compile(code);
  if (!cell)
    return cell.takeError();
  return execute(*cell);
}

llvm::Error Interpreter::undo(unsigned count) {
  if (count > cells.size())
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
        "cannot undo %u cells; the session contains %zu", count, cells.size());

  while (count--) {
    CellArtifact &cell = cells.back();
    if (cell.wasExecuted() && executor)
      if (llvm::Error error = executor->removeModule(cell))
        return error;
    compiler->undo(cell);
    cells.pop_back();
  }
  return llvm::Error::success();
}

llvm::Expected<llvm::orc::ExecutorAddr> Interpreter::getSymbolAddress(
    llvm::StringRef name) const {
  if (!executor)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
        "no incremental executor is configured");
  return executor->getSymbolAddress(name);
}

llvm::Error Interpreter::registerSymbol(
    llvm::StringRef name, llvm::orc::ExecutorAddr address) {
  if (!executor)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
        "no incremental executor is configured");
  return executor->registerSymbol(name, address);
}

llvm::Error Interpreter::loadDynamicLibrary(const char *path) {
  if (!executor)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
        "no incremental executor is configured");
  return executor->loadDynamicLibrary(path);
}

} // namespace Fortran::interpreter
