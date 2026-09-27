//===-- CellArtifact.h - Flang interpreter cell state ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef FORTRAN_INTERPRETER_CELLARTIFACT_H
#define FORTRAN_INTERPRETER_CELLARTIFACT_H

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/OwningOpRef.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <memory>
#include <string>

namespace Fortran::interpreter {

/// Owns the compiler output and session metadata associated with one input.
///
/// Context must be declared before Module so that Module is destroyed first.
class CellArtifact {
public:
  CellArtifact(std::uint64_t id, std::string inputName,
      std::string compiledSource, std::unique_ptr<mlir::MLIRContext> context,
      mlir::OwningOpRef<mlir::ModuleOp> module)
      : id{id}, inputName{std::move(inputName)},
        compiledSource{std::move(compiledSource)}, context{std::move(context)},
        module{std::move(module)} {}

  CellArtifact(CellArtifact &&) = default;
  CellArtifact &operator=(CellArtifact &&) = default;
  CellArtifact(const CellArtifact &) = delete;
  CellArtifact &operator=(const CellArtifact &) = delete;

  std::uint64_t getId() const { return id; }
  llvm::StringRef getInputName() const { return inputName; }
  llvm::StringRef getCompiledSource() const { return compiledSource; }
  mlir::ModuleOp getModule() const { return *module; }

  llvm::StringRef getEntryPoint() const { return entryPoint; }
  void setEntryPoint(llvm::StringRef name) { entryPoint = name.str(); }

  bool wasExecuted() const { return executed; }
  void markExecuted(bool value = true) { executed = value; }

  llvm::StringRef getStateModule() const { return stateModule; }
  llvm::StringRef getPreviousStateModule() const { return previousStateModule; }
  void setStateTransition(
      llvm::StringRef previousModule, llvm::StringRef newModule) {
    previousStateModule = previousModule.str();
    stateModule = newModule.str();
  }

private:
  std::uint64_t id;
  std::string inputName;
  std::string compiledSource;
  std::string entryPoint;
  std::string stateModule;
  std::string previousStateModule;

  std::unique_ptr<mlir::MLIRContext> context;
  mlir::OwningOpRef<mlir::ModuleOp> module;
  bool executed{false};
};

} // namespace Fortran::interpreter

#endif // FORTRAN_INTERPRETER_CELLARTIFACT_H
