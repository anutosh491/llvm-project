//===-- IncrementalExecutor.h - Flang incremental execution ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef FORTRAN_INTERPRETER_INCREMENTALEXECUTOR_H
#define FORTRAN_INTERPRETER_INCREMENTALEXECUTOR_H

#include "llvm/ADT/StringRef.h"
#include "llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h"
#include "llvm/Support/Error.h"

namespace Fortran::interpreter {

class CellArtifact;

/// Backend contract for incrementally adding and removing compiled cells.
class IncrementalExecutor {
public:
  virtual ~IncrementalExecutor() = default;

  virtual llvm::Error addModule(CellArtifact &) = 0;
  virtual llvm::Error execute(CellArtifact &) = 0;
  virtual llvm::Error removeModule(CellArtifact &) = 0;
  virtual llvm::Expected<llvm::orc::ExecutorAddr> getSymbolAddress(
      llvm::StringRef name) const = 0;
  virtual llvm::Error registerSymbol(
      llvm::StringRef name, llvm::orc::ExecutorAddr address) = 0;
  virtual llvm::Error loadDynamicLibrary(const char *path) = 0;
};

} // namespace Fortran::interpreter

#endif // FORTRAN_INTERPRETER_INCREMENTALEXECUTOR_H
