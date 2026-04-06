/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */

#ifndef TRITON_TO_GRAPH_SYMBOLIC_EXECUTION_H
#define TRITON_TO_GRAPH_SYMBOLIC_EXECUTION_H

#include "TritonToGraph/SymValue.h"
#include "TritonToGraph/ControlFlowGraph.h"
#include "TritonToGraph/GraphAnalysis.h"
#include "mlir/IR/Value.h"
#include "mlir/IR/Operation.h"
#include "mlir/IR/Block.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Interfaces/InferTypeOpInterface.h"
#include "triton/Dialect/Triton/IR/Dialect.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

namespace cfg = mlir::triton::cfg;
namespace tt = mlir::triton;

namespace mlir {
namespace triton {
namespace ascend {

using namespace cfg;

//===----------------------------------------------------------------------===//
// 符号执行状态
//===----------------------------------------------------------------------===//

class SymbolicExecutionState {
public:
  // Value -> SymValue 映射
  DenseMap<Value, std::shared_ptr<SymValue>> valueMap;

public:
  SymbolicExecutionState() = default;

  // 获取 SymValue (返回 shared_ptr，避免 shared_from_this 的潜在风险)
  std::shared_ptr<SymValue> getSymValue(Value v) const;
  std::shared_ptr<ScalarSV> getScalarValue(Value v) const;
  std::shared_ptr<TensorSV> getTensorValue(Value v) const;

  // 便捷方法：直接获取 Tensor 的 elementExpr
  std::shared_ptr<ScalarSV> getTensorElementExpr(Value v) const;

  // 设置 SymValue
  void setSymValue(Value v, std::shared_ptr<SymValue> sv);

  // 检查是否存在
  bool hasSymValue(Value v) const;

  // 打印当前状态（调试用）
  void print(llvm::raw_ostream& os) const;
};

//===----------------------------------------------------------------------===//
// 符号执行引擎
//===----------------------------------------------------------------------===//

class SymbolicExecutionEngine {
public:
  SymbolicExecutionEngine() = default;

  // CFG 和 ProgramSlice 设置
  void setCFG(cfg::ControlFlowGraph* cfg) { cfg_ = cfg; }
  void setProgramSlice(ProgramSlice* slice) { slice_ = slice; }

  //===----------------------------------------------------------------------===//
  // 主要执行接口
  //===----------------------------------------------------------------------===//

  // 执行基本块中的所有指令
  void executeBlock(Block* block, SymbolicExecutionState& state);

  // 单步执行单个操作
  void executeOperation(Operation* op, SymbolicExecutionState& state);

  // 执行多个操作（按顺序）
  void executeOperations(ArrayRef<Operation*> ops,
                         SymbolicExecutionState& state);

  //===----------------------------------------------------------------------===//
  // Arith 指令执行器
  //===----------------------------------------------------------------------===//

  // arith.constant
  void executeArithConstant(arith::ConstantOp op,
                            SymbolicExecutionState& state);

  // arith.binary arithmetic (addi, subi, muli, divi, remi, etc.)
  void executeArithBinary(Operation* op,
                          SymbolicExecutionState& state);

  // arith.select
  void executeArithSelect(arith::SelectOp op,
                          SymbolicExecutionState& state);

  // arith.cmpi
  void executeArithCmpI(arith::CmpIOp op,
                        SymbolicExecutionState& state);

  //===----------------------------------------------------------------------===//
  // Triton 指令执行器
  //===----------------------------------------------------------------------===//

  // tt.get_program_id
  void executeGetProgramID(tt::GetProgramIdOp op,
                           SymbolicExecutionState& state);

  // tt.make_range
  void executeMakeRange(tt::MakeRangeOp op,
                        SymbolicExecutionState& state);

  // tt.splat
  void executeSplat(tt::SplatOp op,
                    SymbolicExecutionState& state);

  // tt.addptr
  void executeAddPtr(tt::AddPtrOp op,
                     SymbolicExecutionState& state);

  // tt.expand_dims
  void executeExpandDims(tt::ExpandDimsOp op,
                         SymbolicExecutionState& state);

  // tt.broadcast
  void executeBroadcast(tt::BroadcastOp op,
                        SymbolicExecutionState& state);

  // tt.make_tensor_ptr
  void executeMakeTensorPtr(tt::MakeTensorPtrOp op,
                            SymbolicExecutionState& state);

  // tt.load
  void executeLoad(tt::LoadOp op,
                   SymbolicExecutionState& state);

  //===----------------------------------------------------------------------===//
  // 控制流指令执行器（简化版）
  //===----------------------------------------------------------------------===//

  // scf.for - 简化为创建 InductionSV 和 IterArgSV
  void executeForLoop(scf::ForOp loop,
                      SymbolicExecutionState& state);

  // scf.if - 简化为创建 UnknownSV for results
  void executeIfOp(scf::IfOp op,
                   SymbolicExecutionState& state);

  // scf.yield - 简化处理
  void executeYield(scf::YieldOp op,
                    SymbolicExecutionState& state);

  //===----------------------------------------------------------------------===//
  // 辅助方法
  //===----------------------------------------------------------------------===//

  // 为入参创建 SymValue（指针类型创建 GmPtrSV，其他创建 UnknownSV）
  void createSymValueForArgument(Value arg, SymbolicExecutionState& state);

  // 创建 UnknownSV（根据类型区分 Scalar 或 Tensor）
  std::shared_ptr<SymValue> createUnknownSV(Type type);

private:
  // CFG 和 ProgramSlice（用于获取 ForOp/IfOp 的上下文信息）
  cfg::ControlFlowGraph* cfg_ = nullptr;
  ProgramSlice* slice_ = nullptr;

  // 辅助方法：根据arith操作符创建对应的表达式
  std::shared_ptr<ScalarSV> createArithExpr(
      StringRef opName, std::shared_ptr<ScalarSV> lhs,
      std::shared_ptr<ScalarSV> rhs, Type resultType);

  // 辅助方法：根据arith.cmpi谓词创建对应的CmpExprSV
  CmpExprSV::Pred getCmpIPred(arith::CmpIPredicate pred);
};

} // namespace ascend
} // namespace triton
} // namespace mlir

#endif // TRITON_TO_GRAPH_SYMBOLIC_EXECUTION_H
