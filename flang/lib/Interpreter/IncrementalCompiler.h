//===-- IncrementalCompiler.h - Compile one Fortran cell -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef FORTRAN_LIB_INTERPRETER_INCREMENTALCOMPILER_H
#define FORTRAN_LIB_INTERPRETER_INCREMENTALCOMPILER_H

#include "flang/Frontend/CompilerInvocation.h"
#include "flang/Interpreter/CellArtifact.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include <cstdint>
#include <memory>
#include <system_error>

namespace Fortran::frontend {
class CompilerInstance;
}

namespace Fortran::interpreter {

class IncrementalCompiler {
public:
  explicit IncrementalCompiler(
      std::unique_ptr<frontend::CompilerInstance> compiler);
  ~IncrementalCompiler();

  llvm::Expected<CellArtifact> compile(
      llvm::StringRef code, std::uint64_t cellId);
  void undo(const CellArtifact &cell);

private:
  std::shared_ptr<frontend::CompilerInvocation> invocation;
  std::string sessionModuleDirectory;
  std::error_code initializationError;
  bool ownsModuleDirectory{false};
  std::string latestStateModule;
};

} // namespace Fortran::interpreter

#endif // FORTRAN_LIB_INTERPRETER_INCREMENTALCOMPILER_H
