//===-- FlangRepl.cpp - Interactive Fortran compiler ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "flang/Frontend/CompilerInstance.h"
#include "flang/Interpreter/Interpreter.h"
#include "flang/Interpreter/MLIRIncrementalExecutor.h"
#include "clang/Options/OptionUtils.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"
#include <cstdint>
#include <iostream>
#include <string>

namespace {

extern "C" void flang_repl_print_i32(std::int32_t value) {
  llvm::outs() << value << '\n';
}

llvm::cl::opt<std::string> targetTriple{"target",
    llvm::cl::desc("Target triple used to compile cells"),
    llvm::cl::value_desc("triple")};

llvm::cl::opt<bool> compileOnly{"compile-only",
    llvm::cl::desc("Compile cells without loading or executing them"),
    llvm::cl::init(false)};

llvm::cl::opt<std::string> resourceDirectory{"resource-dir",
    llvm::cl::desc("Flang resource directory containing runtime and modules"),
    llvm::cl::value_desc("directory")};

llvm::cl::opt<std::string> runtimeLibrary{"runtime-library",
    llvm::cl::desc("Flang runtime dynamic library to preload"),
    llvm::cl::value_desc("path")};

llvm::cl::opt<bool> noRuntime{"no-runtime",
    llvm::cl::desc("Do not preload the Flang runtime"), llvm::cl::init(false)};

llvm::cl::opt<bool> trace{"trace",
    llvm::cl::desc("Trace compiler and executor session operations"),
    llvm::cl::init(false)};

void traceMessage(const llvm::Twine &message) {
  if (trace)
    llvm::errs() << "[flang-repl] " << message << '\n';
}

std::string getRuntimeLibraryPath(
    llvm::StringRef resourceDir, const llvm::Triple &triple) {
  llvm::SmallString<256> path{resourceDir};
  llvm::sys::path::append(path, "lib");
  if (triple.isOSDarwin()) {
    llvm::sys::path::append(path, "darwin", "libflang_rt.runtime.dylib");
  } else {
    llvm::sys::path::append(path, triple.str(), "libflang_rt.runtime.so");
  }
  return path.str().str();
}

std::string getIntrinsicModulePath(
    llvm::StringRef resourceDir, const llvm::Triple &triple) {
  llvm::SmallString<256> path{resourceDir};
  llvm::sys::path::append(path, "finclude", "flang", triple.str());
  return path.str().str();
}

void printHelp() {
  llvm::outs() << "Enter a Fortran cell and finish it with %end.\n"
               << "Commands:\n"
               << "  %dump  print the LLVM-dialect MLIR for the newest cell\n"
               << "  %load <path>  load a native dynamic library\n"
               << "  %undo  remove the newest cell from the session\n"
               << "  %help  show this help\n"
               << "  %quit  leave the interpreter\n";
}

void printError(llvm::Error error) {
  llvm::logAllUnhandledErrors(std::move(error), llvm::errs(), "error: ");
}

} // namespace

int main(int argc, char **argv) {
  llvm::InitLLVM initLLVM{argc, argv};
  llvm::cl::ParseCommandLineOptions(argc, argv, "Flang incremental REPL\n");

  llvm::InitializeAllTargetInfos();
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmParsers();
  llvm::InitializeAllAsmPrinters();

  llvm::Triple triple{targetTriple.empty()
          ? llvm::sys::getProcessTriple()
          : llvm::Triple::normalize(targetTriple)};
  std::string resourceDir = resourceDirectory.empty()
      ? clang::GetResourcesPath(argv[0])
      : resourceDirectory;
  std::string intrinsicModulePath = getIntrinsicModulePath(resourceDir, triple);

  traceMessage(llvm::Twine{"target triple: "} + triple.str());
  traceMessage(llvm::Twine{"resource directory: "} + resourceDir);
  traceMessage(
      llvm::Twine{"intrinsic module directory: "} + intrinsicModulePath);

  Fortran::interpreter::IncrementalCompilerBuilder builder;
  builder.setExecutablePath(argv[0]);
  builder.setTargetTriple(triple.str());
  builder.addIntrinsicModuleDirectory(intrinsicModulePath);

  auto compiler = builder.create();
  if (!compiler) {
    printError(compiler.takeError());
    return 1;
  }

  std::unique_ptr<Fortran::interpreter::IncrementalExecutor> executor;
  if (!compileOnly)
    executor =
        std::make_unique<Fortran::interpreter::MLIRIncrementalExecutor>();
  auto interpreter = Fortran::interpreter::Interpreter::create(
      std::move(*compiler), std::move(executor));
  if (!interpreter) {
    printError(interpreter.takeError());
    return 1;
  }
  if (!compileOnly)
    if (llvm::Error error = (*interpreter)
            ->registerSymbol("flang_repl_print_i32",
                llvm::orc::ExecutorAddr::fromPtr(&flang_repl_print_i32))) {
      printError(std::move(error));
      return 1;
    }

  if (!compileOnly && !noRuntime) {
    std::string path = runtimeLibrary.empty()
        ? getRuntimeLibraryPath(resourceDir, triple)
        : runtimeLibrary;
    traceMessage(llvm::Twine{"loading runtime: "} + path);
    if (!llvm::sys::fs::exists(path)) {
      llvm::errs() << "error: Flang runtime library was not found at '" << path
                   << "'\n"
                   << "use --runtime-library=<path> or --no-runtime\n";
      return 1;
    }
    if (llvm::Error error = (*interpreter)->loadDynamicLibrary(path.c_str())) {
      printError(std::move(error));
      return 1;
    }
    traceMessage("runtime loaded");
  }

  llvm::outs() << "flang-repl experimental session\n";
  if (compileOnly)
    llvm::outs() << "Execution is disabled by --compile-only.\n";
  printHelp();

  std::string line;
  while (true) {
    llvm::outs() << "flang-repl> ";
    llvm::outs().flush();
    if (!std::getline(std::cin, line))
      break;

    llvm::StringRef command{line};
    command = command.trim();
    if (command.empty())
      continue;
    if (command == "%quit")
      break;
    if (command == "%help") {
      printHelp();
      continue;
    }
    if (command == "%dump") {
      const auto *cell = (*interpreter)->getLastCell();
      if (!cell)
        llvm::errs() << "error: the session contains no cells\n";
      else
        cell->getModule()->print(llvm::outs());
      llvm::outs() << '\n';
      continue;
    }
    if (command == "%undo") {
      if (llvm::Error error = (*interpreter)->undo())
        printError(std::move(error));
      else
        llvm::outs() << "removed the newest cell\n";
      continue;
    }
    if (command.consume_front("%load")) {
      command = command.trim();
      if (command.empty()) {
        llvm::errs() << "error: %load requires a library path\n";
      } else if (llvm::Error error = (*interpreter)
                     ->loadDynamicLibrary(command.str().c_str())) {
        printError(std::move(error));
      } else {
        llvm::outs() << "loaded '" << command << "'\n";
      }
      continue;
    }
    if (command.starts_with("%")) {
      llvm::errs() << "error: unknown command '" << command << "'\n";
      continue;
    }

    std::string code = line;
    code.push_back('\n');
    while (true) {
      llvm::outs() << "       ...> ";
      llvm::outs().flush();
      if (!std::getline(std::cin, line))
        break;
      if (llvm::StringRef(line).trim() == "%end")
        break;
      code.append(line);
      code.push_back('\n');
    }

    auto cell = (*interpreter)->compile(code);
    if (!cell) {
      printError(cell.takeError());
      continue;
    }
    traceMessage(llvm::Twine{"compiled cell "} + llvm::Twine{cell->getId()} +
        " (entry: " +
        (cell->getEntryPoint().empty() ? llvm::StringRef{"<none>"}
                                       : cell->getEntryPoint()) +
        ")");
    if (trace && cell->getCompiledSource() != code)
      llvm::errs() << "[flang-repl] generated source:\n"
                   << cell->getCompiledSource();
    if (!compileOnly) {
      traceMessage(llvm::Twine{"loading and executing cell "} +
          llvm::Twine{cell->getId()});
      if (llvm::Error error = (*interpreter)->execute(*cell)) {
        printError(std::move(error));
        continue;
      }
      traceMessage(llvm::Twine{"executed cell "} + llvm::Twine{cell->getId()});
    }
    llvm::outs() << "compiled";
    if (!compileOnly)
      llvm::outs() << " and executed";
    llvm::outs() << " cell " << cell->getId();
    if (!cell->getEntryPoint().empty())
      llvm::outs() << " (entry: " << cell->getEntryPoint() << ')';
    llvm::outs() << '\n';
  }

  return 0;
}
