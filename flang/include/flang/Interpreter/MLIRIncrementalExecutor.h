//===-- MLIRIncrementalExecutor.h - MLIR-backed execution ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef FORTRAN_INTERPRETER_MLIRINCREMENTALEXECUTOR_H
#define FORTRAN_INTERPRETER_MLIRINCREMENTALEXECUTOR_H

#include "flang/Interpreter/IncrementalExecutor.h"
#include <memory>

namespace Fortran::interpreter {

/// Executes each cell with MLIR's ORC-based ExecutionEngine.
class MLIRIncrementalExecutor : public IncrementalExecutor {
public:
  MLIRIncrementalExecutor();
  ~MLIRIncrementalExecutor() override;

  llvm::Error addModule(CellArtifact &) override;
  llvm::Error execute(CellArtifact &) override;
  llvm::Error removeModule(CellArtifact &) override;
  llvm::Expected<llvm::orc::ExecutorAddr> getSymbolAddress(
      llvm::StringRef name) const override;
  llvm::Error registerSymbol(
      llvm::StringRef name, llvm::orc::ExecutorAddr address) override;
  llvm::Error loadDynamicLibrary(const char *path) override;

private:
  class Impl;
  std::unique_ptr<Impl> impl;
};

} // namespace Fortran::interpreter

#endif // FORTRAN_INTERPRETER_MLIRINCREMENTALEXECUTOR_H
