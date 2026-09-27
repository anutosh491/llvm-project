add_flang_library(flangInterpreter
  IncrementalCompiler.cpp
  Interpreter.cpp
  MLIRIncrementalExecutor.cpp

  LINK_LIBS
  flangFrontend

  LINK_COMPONENTS
  Core
  ExecutionEngine
  OrcJIT
  Support
  TargetParser

  MLIR_LIBS
  MLIRIR
  MLIRExecutionEngine
  MLIRLLVMToLLVMIRTranslation
)
