//===-- Interpreter.h - Incremental Fortran compilation --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef FORTRAN_INTERPRETER_INTERPRETER_H
#define FORTRAN_INTERPRETER_INTERPRETER_H

#include "flang/Interpreter/CellArtifact.h"
#include "flang/Interpreter/IncrementalExecutor.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <vector>

namespace Fortran::frontend {
class CompilerInstance;
}

namespace Fortran::interpreter {

class IncrementalCompiler;

/// Creates a CompilerInstance configured for an interpreter session.
class IncrementalCompilerBuilder {
public:
  void setTargetTriple(llvm::StringRef triple) { targetTriple = triple.str(); }
  void setExecutablePath(llvm::StringRef path) { executablePath = path.str(); }
  void setModuleDirectory(llvm::StringRef directory) {
    moduleDirectory = directory.str();
  }
  void addIntrinsicModuleDirectory(llvm::StringRef directory) {
    intrinsicModuleDirectories.push_back(directory.str());
  }

  llvm::Expected<std::unique_ptr<frontend::CompilerInstance>> create() const;

private:
  std::string targetTriple;
  std::string executablePath{"flang-repl"};
  std::string moduleDirectory{"."};
  std::vector<std::string> intrinsicModuleDirectories;
};

/// Coordinates incremental compilation, execution, and rollback.
class Interpreter {
public:
  static llvm::Expected<std::unique_ptr<Interpreter>> create(
      std::unique_ptr<frontend::CompilerInstance> compiler,
      std::unique_ptr<IncrementalExecutor> executor = nullptr);

  ~Interpreter();

  llvm::Expected<CellArtifact &> compile(llvm::StringRef code);
  llvm::Error execute(CellArtifact &cell);
  llvm::Error compileAndExecute(llvm::StringRef code);
  llvm::Error undo(unsigned count = 1);

  llvm::Expected<llvm::orc::ExecutorAddr> getSymbolAddress(
      llvm::StringRef name) const;
  llvm::Error registerSymbol(
      llvm::StringRef name, llvm::orc::ExecutorAddr address);
  llvm::Error loadDynamicLibrary(const char *path);

  std::size_t cellCount() const { return cells.size(); }
  const CellArtifact *getLastCell() const {
    return cells.empty() ? nullptr : &cells.back();
  }

private:
  Interpreter(std::unique_ptr<IncrementalCompiler> compiler,
      std::unique_ptr<IncrementalExecutor> executor);

  std::unique_ptr<IncrementalCompiler> compiler;
  std::unique_ptr<IncrementalExecutor> executor;
  std::list<CellArtifact> cells;
};

} // namespace Fortran::interpreter

#endif // FORTRAN_INTERPRETER_INTERPRETER_H
