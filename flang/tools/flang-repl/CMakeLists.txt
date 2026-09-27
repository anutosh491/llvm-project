set(LLVM_LINK_COMPONENTS
  Core
  ExecutionEngine
  Option
  OrcJIT
  Support
  TargetParser
  ${LLVM_TARGETS_TO_BUILD}
)

add_flang_tool(flang-repl
  FlangRepl.cpp
)

target_link_libraries(flang-repl PRIVATE
  flangInterpreter
)

mlir_target_link_libraries(flang-repl PRIVATE
  MLIRExecutionEngine
  MLIRIR
)

llvm_update_compile_flags(flang-repl)
