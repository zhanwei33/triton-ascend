add_triton_library(TritonToGraph
  AliasAnalysis.cpp
  ControlFlowGraph.cpp
  ControlFlowGraphBuilder.cpp
  DataflowGraph.cpp
  MemorySsaBuilder.cpp
  GraphAnalysis.cpp
  GraphOptimizationContext.cpp
  GraphOptimizationPass.cpp
  PermutationAnalysis.cpp
  Rules/LoadStoreTransposeRule.cpp
  Rules/TransposePointwiseReorderRule.cpp
  Rules/UBPreloadRule.cpp
  TensorAnalyzer.cpp
  SymValue.cpp
  SymbolicExecution.cpp
  SValPatternAnalyzer.cpp
  Passes.cpp

  DEPENDS
  TritonToGraphPassIncGen

  LINK_LIBS PUBLIC
  MLIRArithDialect
  MLIRControlFlowDialect
  MLIRDialectUtils
  MLIRIR
  MLIRMathDialect
  MLIRFuncDialect
  MLIRPass
  MLIRSCFDialect
  MLIRSupport
  MLIRTensorDialect
  MLIRTransforms
  TritonIR
  TritonTransforms
)
