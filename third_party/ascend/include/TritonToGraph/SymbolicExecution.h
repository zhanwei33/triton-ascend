/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */

#ifndef TRITON_TO_GRAPH_SYMBOLIC_EXECUTION_H
#define TRITON_TO_GRAPH_SYMBOLIC_EXECUTION_H

#include "TritonToGraph/SymValue.h"
#include "mlir/IR/Value.h"
#include "mlir/IR/Operation.h"
#include "mlir/IR/Block.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "triton/Dialect/Triton/IR/Dialect.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

namespace mlir {
namespace triton {
namespace ascend {

//===----------------------------------------------------------------------===//
// 循环上下文
//===----------------------------------------------------------------------===//

struct LoopContext {
  scf::ForOp loopOp;
  SymValue* inductionVar;  // 迭代变量的符号表示
  SymValue* lowerBound;
  SymValue* upperBound;
  SymValue* step;

  LoopContext(scf::ForOp loop, SymValue* iv, SymValue* lb,
              SymValue* ub, SymValue* s)
      : loopOp(loop), inductionVar(iv), lowerBound(lb),
        upperBound(ub), step(s) {}
};

//===----------------------------------------------------------------------===//
// 条件分支上下文
//===----------------------------------------------------------------------===//

struct ConditionContext {
  Value condition;      // 条件值
  bool isTrueBranch;    // 是否为true分支
  SmallVector<std::pair<Value, SymValue*>> branchValues;  // 分支内定义的value

  ConditionContext(Value cond, bool isTrue)
      : condition(cond), isTrueBranch(isTrue) {}
};

//===----------------------------------------------------------------------===//
// 符号执行状态
//===----------------------------------------------------------------------===//

class SymbolicExecutionState {
public:
  // Value -> SymValue 映射
  DenseMap<Value, SymValue*> valueMap;

  // 循环上下文栈（支持嵌套循环）
  SmallVector<LoopContext> loopStack;

  // 条件分支上下文栈
  SmallVector<ConditionContext> conditionStack;

public:
  SymbolicExecutionState() = default;

  // 获取/设置 SymValue
  SymValue* getSymValue(Value v) const;
  void setSymValue(Value v, SymValue* sv);
  bool hasSymValue(Value v) const;

  // 循环上下文管理
  void enterLoop(scf::ForOp loop, SymValue* iv, SymValue* lb,
                 SymValue* ub, SymValue* step);
  void exitLoop();
  bool inLoop() const { return !loopStack.empty(); }
  const LoopContext& getCurrentLoop() const;

  // 条件上下文管理
  void enterCondition(Value cond, bool isTrueBranch);
  void exitCondition();
  bool inCondition() const { return !conditionStack.empty(); }

  // 合并两个状态（用于if-else分支合并）
  // 使用Select操作符合并两个分支的values
  static void mergeStates(SymbolicExecutionState& result,
                          const SymbolicExecutionState& trueState,
                          const SymbolicExecutionState& falseState,
                          Value condition);

  // 打印当前状态（调试用）
  void print(llvm::raw_ostream& os) const;
};

//===----------------------------------------------------------------------===//
// 符号执行引擎
//===----------------------------------------------------------------------===//

class SymbolicExecutionEngine {
public:
  SymbolicExecutionEngine() = default;

  //===----------------------------------------------------------------------===
  // 主要执行接口
  //===----------------------------------------------------------------------===

  // 执行基本块中的所有指令
  void executeBlock(Block* block, SymbolicExecutionState& state);

  // 单步执行单个操作
  void executeOperation(Operation* op, SymbolicExecutionState& state);

  // 执行多个操作（按顺序）
  void executeOperations(ArrayRef<Operation*> ops,
                         SymbolicExecutionState& state);

  //===----------------------------------------------------------------------===
  // Arith 指令执行器
  //===----------------------------------------------------------------------===

  // arith.constant
  void executeArithConstant(arith::ConstantOp op,
                            SymbolicExecutionState& state);

  // arith.binary arithmetic (addi, subi, muli, divi, etc.)
  void executeArithBinary(ArithmeticOpInterface op,
                          SymbolicExecutionState& state);

  // arith.select
  void executeArithSelect(arith::SelectOp op,
                          SymbolicExecutionState& state);

  // arith.cmpi
  void executeArithCmpI(arith::CmpIOp op,
                        SymbolicExecutionState& state);

  // arith.cmpf
  void executeArithCmpF(arith::CmpFOp op,
                        SymbolicExecutionState& state);

  // arith.index_cast / arith.sitofp / etc.
  void executeArithCast(Operation* op, SymbolicExecutionState& state);

  //===----------------------------------------------------------------------===
  // Triton 指令执行器
  //===----------------------------------------------------------------------===

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

  // tt.advance
  void executeAdvance(tt::AdvanceOp op,
                      SymbolicExecutionState& state);

  //===----------------------------------------------------------------------===
  // 控制流指令执行器
  //===----------------------------------------------------------------------===

  // scf.for
  void executeForLoop(scf::ForOp loop,
                      SymbolicExecutionState& state);

  // scf.if
  void executeIfOp(scf::IfOp op,
                   SymbolicExecutionState& state);

  // scf.yield
  void executeYield(scf::YieldOp op,
                    SymbolicExecutionState& state);

private:
  // 辅助方法：根据arith操作符创建对应的ScalarExprSV::OpKind
  llvm::Optional<ScalarExprSV::OpKind> getArithOpKind(
      ArithmeticOpInterface op);

  // 辅助方法：根据arith.cmpi谓词创建对应的OpKind
  ScalarExprSV::OpKind getCmpIOpKind(arith::CmpIPredicate pred);

  // 辅助方法：根据arith.cmpf谓词创建对应的OpKind
  ScalarExprSV::OpKind getCmpFOpKind(arith::CmpFPredicate pred);

  // 辅助方法：处理循环迭代参数（iter_args）
  void handleIterArgs(scf::ForOp loop, SymbolicExecutionState& state);

  // 辅助方法：处理循环yield
  void handleLoopYield(scf::ForOp loop, scf::YieldOp yield,
                       SymbolicExecutionState& state);
};

} // namespace ascend
} // namespace triton
} // namespace mlir

#endif // TRITON_TO_GRAPH_SYMBOLIC_EXECUTION_H
