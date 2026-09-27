//===-- MLIRIncrementalExecutor.cpp - MLIR-backed execution --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "flang/Interpreter/MLIRIncrementalExecutor.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/ExecutionEngine/ExecutionEngine.h"
#include "flang/Interpreter/CellArtifact.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/Support/DynamicLibrary.h"
#include <vector>

namespace Fortran::interpreter {

namespace {

struct LoadedCell {
  std::unique_ptr<mlir::ExecutionEngine> engine;
  std::vector<std::string> exportedSymbols;
};

} // namespace

class MLIRIncrementalExecutor::Impl {
public:
  llvm::DenseMap<std::uint64_t, LoadedCell> cells;
  llvm::StringMap<void *> symbols;
};

MLIRIncrementalExecutor::MLIRIncrementalExecutor()
    : impl{std::make_unique<Impl>()} {}

MLIRIncrementalExecutor::~MLIRIncrementalExecutor() = default;

llvm::Error MLIRIncrementalExecutor::addModule(CellArtifact &cell) {
  if (impl->cells.contains(cell.getId()))
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
        "cell %llu is already loaded",
        static_cast<unsigned long long>(cell.getId()));

  std::vector<std::string> definitions;
  for (mlir::LLVM::LLVMFuncOp function :
      cell.getModule().getOps<mlir::LLVM::LLVMFuncOp>())
    if (!function.isExternal())
      definitions.push_back(function.getSymName().str());
  for (mlir::LLVM::GlobalOp global :
      cell.getModule().getOps<mlir::LLVM::GlobalOp>())
    if (global.getValueOrNull() || !global.getInitializerRegion().empty())
      definitions.push_back(global.getSymName().str());

  auto engine = mlir::ExecutionEngine::create(cell.getModule());
  if (!engine)
    return engine.takeError();

  (*engine)->registerSymbols([&](llvm::orc::MangleAndInterner interner) {
    llvm::orc::SymbolMap map;
    for (const auto &symbol : impl->symbols)
      map[interner(symbol.getKey())] = {
          llvm::orc::ExecutorAddr::fromPtr(symbol.getValue()),
          llvm::JITSymbolFlags::Exported};
    return map;
  });
  (*engine)->initialize();

  LoadedCell loaded{std::move(*engine), {}};
  for (const std::string &name : definitions) {
    llvm::Expected<void *> address = loaded.engine->lookup(name);
    if (!address)
      return address.takeError();
    impl->symbols[name] = *address;
    loaded.exportedSymbols.push_back(name);
  }

  impl->cells.try_emplace(cell.getId(), std::move(loaded));
  return llvm::Error::success();
}

llvm::Error MLIRIncrementalExecutor::execute(CellArtifact &cell) {
  auto found = impl->cells.find(cell.getId());
  if (found == impl->cells.end())
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
        "cell %llu is not loaded",
        static_cast<unsigned long long>(cell.getId()));
  if (cell.getEntryPoint().empty())
    return llvm::Error::success();
  return found->second.engine->invokePacked(cell.getEntryPoint());
}

llvm::Error MLIRIncrementalExecutor::removeModule(CellArtifact &cell) {
  auto found = impl->cells.find(cell.getId());
  if (found == impl->cells.end())
    return llvm::Error::success();
  for (const std::string &name : found->second.exportedSymbols)
    impl->symbols.erase(name);
  impl->cells.erase(found);
  return llvm::Error::success();
}

llvm::Expected<llvm::orc::ExecutorAddr>
MLIRIncrementalExecutor::getSymbolAddress(llvm::StringRef name) const {
  auto found = impl->symbols.find(name);
  if (found == impl->symbols.end())
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
        "symbol '%s' was not found", name.str().c_str());
  return llvm::orc::ExecutorAddr::fromPtr(found->second);
}

llvm::Error MLIRIncrementalExecutor::registerSymbol(
    llvm::StringRef name, llvm::orc::ExecutorAddr address) {
  impl->symbols[name] = address.toPtr<void *>();
  return llvm::Error::success();
}

llvm::Error MLIRIncrementalExecutor::loadDynamicLibrary(const char *path) {
  std::string error;
  if (llvm::sys::DynamicLibrary::LoadLibraryPermanently(path, &error))
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(), "%s", error.c_str());
  return llvm::Error::success();
}

} // namespace Fortran::interpreter
