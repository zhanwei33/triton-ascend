/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */

#include "TritonToGraph/SymbolicExecution.h"
#include "mlir/IR/PatternMatch.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "symbolic-execution"

using namespace mlir;
using namespace mlir::triton;
using namespace mlir::triton::ascend;

//===----------------------------------------------------------------------===//
// SymValue 辅助实现
//===----------------------------------------------------------------------===//

void ScalarExprSV::print(llvm::raw_ostream& os) const {
  if (isSelect()) {
    os << "select(";
    condition->print(os);
    os << ", ";
    lhs->print(os);
    os << ", ";
    rhs->print(os);
    os << ")";
  } else {
    os << "(";
    lhs->print(os);
    os << " " << getOpStr(op) << " ";
    rhs->print(os);
    os << ")";
  }
}

const char* ScalarExprSV::getOpStr(OpKind k) {
  switch (k) {
    case OpKind::Add: return "+";
    case OpKind::Sub: return "-";
    case OpKind::Mul: return "*";
    case OpKind::Div: return "/";
    case OpKind::CmpEQ: return "==";
    case OpKind::CmpNE: return "!=";
    case OpKind::CmpLT: return "<";
    case OpKind::CmpLE: return "<=";
    case OpKind::CmpGT: return ">";
    case OpKind::CmpGE: return ">=";
    case OpKind::Select: return "select";
  }
  return "?";
}

bool ScalarExprSV::canFoldConstants() const {
  if (isSelect()) return false;
  return isConstant(lhs) && isConstant(rhs);
}

ScalarConstantSV* ScalarExprSV::foldConstants() {
  if (!canFoldConstants()) return nullptr;

  auto lhsInt = dyn_cast<ScalarConstantIntSV>(lhs);
  auto rhsInt = dyn_cast<ScalarConstantIntSV>(rhs);

  if (lhsInt && rhsInt) {
    int64_t l = lhsInt->getInt();
    int64_t r = rhsInt->getInt();
    int64_t result = 0;

    switch (op) {
      case OpKind::Add: result = l + r; break;
      case OpKind::Sub: result = l - r; break;
      case OpKind::Mul: result = l * r; break;
      case OpKind::Div: result = r != 0 ? l / r : 0; break;
      default: return nullptr;
    }
    return new ScalarConstantIntSV(result);
  }

  // TODO: 浮点常量处理
  return nullptr;
}

ScalarExprSV* ScalarExprSV::applyAssociative() {
  if (op != OpKind::Add && op != OpKind::Mul) return this;

  // 如果lhs是同类运算，重组: (A op B) op C -> A op (B op C)
  if (auto lhsExpr = dyn_cast<ScalarExprSV>(lhs)) {
    if (lhsExpr->op == op) {
      // 检查是否满足结合律优化条件
      auto A = lhsExpr->lhs;
      auto B = lhsExpr->rhs;
      auto C = rhs;

      auto newRHS = new ScalarExprSV(op, B, C);
      return new ScalarExprSV(op, A, newRHS);
    }
  }
  return this;
}

ScalarExprSV* ScalarExprSV::applyDistributive() {
  // a * (b + c) = a * b + a * c
  if (op == OpKind::Mul) {
    if (auto rhsExpr = dyn_cast<ScalarExprSV>(rhs)) {
      if (rhsExpr->op == OpKind::Add) {
        auto a = lhs;
        auto b = rhsExpr->lhs;
        auto c = rhsExpr->rhs;

        auto aMulB = new ScalarExprSV(OpKind::Mul, a, b);
        auto aMulC = new ScalarExprSV(OpKind::Mul, a, c);
        return new ScalarExprSV(OpKind::Add, aMulB, aMulC);
      }
    }
  }
  return this;
}

ScalarExprSV* ScalarExprSV::canonicalize() {
  // 常量放右边: x + 3 -> 3 + x (如果+满足交换律)
  if (op == OpKind::Add || op == OpKind::Mul) {
    if (isConstant(lhs) && !isConstant(rhs)) {
      // 交换左右操作数
      return new ScalarExprSV(op, rhs, lhs);
    }
  }
  return this;
}

ScalarExprSV* ScalarExprSV::combineLikeTerms() {
  // x + 2*x = 3*x
  if (op == OpKind::Add || op == OpKind::Sub) {
    // 简单情况: 检查两边是否有公共因子
    // 更复杂的合并需要完整的多项式表示
    // TODO: 实现更完善的同类项合并
  }
  return this;
}

SymValue* ScalarExprSV::simplify() {
  // 1. 常量折叠
  if (auto folded = foldConstants()) {
    return folded;
  }

  // 2. 规范化
  auto canonical = canonicalize();

  // 3. 尝试结合律优化
  auto associative = canonical->applyAssociative();

  // 4. 再次尝试常量折叠
  if (auto folded = associative->foldConstants()) {
    return folded;
  }

  return associative;
}

ScalarExprSV* TensorRangeSV::getElementExpr(int64_t index) const {
  auto startVal = new ScalarConstantIntSV(start);
  auto idxVal = new ScalarConstantIntSV(index);
  return new ScalarExprSV(ScalarExprSV::OpKind::Add, startVal, idxVal);
}

int64_t TensorSplatSV::getLinearIndex(ArrayRef<int64_t> indices) const {
  int64_t linear = 0;
  int64_t stride = 1;
  for (int i = shape.size() - 1; i >= 0; --i) {
    linear += indices[i] * stride;
    stride *= shape[i];
  }
  return linear;
}

SmallVector<int64_t> TensorSplatSV::getMultiDimIndex(int64_t linearIdx) const {
  SmallVector<int64_t> indices(shape.size());
  for (int i = shape.size() - 1; i >= 0; --i) {
    indices[i] = linearIdx % shape[i];
    linearIdx /= shape[i];
  }
  return indices;
}

void TensorExprSV::print(llvm::raw_ostream& os) const {
  os << "tensor." << ScalarExprSV::getOpStr(
    static_cast<ScalarExprSV::OpKind>(static_cast<int>(op)));
  os << "[shape=";
  for (size_t i = 0; i < shape.size(); ++i) {
    if (i > 0) os << "x";
    os << shape[i];
  }
  os << "]";
}

SymValue* TensorExprSV::getElement(ArrayRef<int64_t> indices) const {
  // 根据操作类型返回元素的SymValue
  switch (op) {
    case OpKind::Add:
    case OpKind::Sub:
    case OpKind::Mul:
    case OpKind::Div: {
      auto lhsElem = dyn_cast<TensorExprSV>(operands[0])
                        ? dyn_cast<TensorExprSV>(operands[0])->getElement(indices)
                        : operands[0];
      auto rhsElem = dyn_cast<TensorExprSV>(operands[1])
                        ? dyn_cast<TensorExprSV>(operands[1])->getElement(indices)
                        : operands[1];
      return new ScalarExprSV(static_cast<ScalarExprSV::OpKind>(
                                static_cast<int>(op)), lhsElem, rhsElem);
    }
    case OpKind::Select: {
      auto condElem = dyn_cast<TensorExprSV>(operands[0])
                         ? dyn_cast<TensorExprSV>(operands[0])->getElement(indices)
                         : operands[0];
      auto trueElem = dyn_cast<TensorExprSV>(operands[1])
                         ? dyn_cast<TensorExprSV>(operands[1])->getElement(indices)
                         : operands[1];
      auto falseElem = dyn_cast<TensorExprSV>(operands[2])
                          ? dyn_cast<TensorExprSV>(operands[2])->getElement(indices)
                          : operands[2];
      return new ScalarExprSV(condElem, trueElem, falseElem);
    }
    case OpKind::ExpandDims: {
      // 需要根据expand的axis调整indices
      // 这里简化处理，假设indices已经正确
      auto inputTensor = dyn_cast<TensorExprSV>(operands[0]);
      if (inputTensor) {
        SmallVector<int64_t> inputIndices;
        for (size_t i = 0, j = 0; i < shape.size(); ++i) {
          if (i == static_cast<size_t>(expandAxis)) {
            // expand的维度，index应该是0
            if (indices[i] != 0) {
              // 超出范围，但暂时不报错
            }
          } else {
            inputIndices.push_back(indices[i]);
            ++j;
          }
        }
        return inputTensor->getElement(inputIndices);
      }
      return operands[0];
    }
    case OpKind::Broadcast: {
      // 广播操作需要根据broadcastDims转换indices
      auto inputTensor = dyn_cast<TensorExprSV>(operands[0]);
      if (inputTensor) {
        SmallVector<int64_t> inputIndices;
        for (size_t i = 0; i < shape.size(); ++i) {
          // 检查i是否在broadcastDims中
          bool isBroadcastDim = false;
          for (auto bd : broadcastDims) {
            if (static_cast<int64_t>(i) == bd) {
              isBroadcastDim = true;
              break;
            }
          }
          if (!isBroadcastDim) {
            inputIndices.push_back(indices[i]);
          }
        }
        return inputTensor->getElement(inputIndices);
      }
      return operands[0];
    }
    default:
      return nullptr;
  }
}

bool TensorExprSV::isLengthCheckPattern(SymValue*& range, int64_t& bound) const {
  // 识别 select(cmp_lt(idx, bound), idx, 0) 模式
  if (op != OpKind::Select) return false;

  auto cond = operands[0];
  auto trueVal = operands[1];
  auto falseVal = operands[2];

  // 条件应为比较操作
  auto condExpr = dyn_cast<ScalarExprSV>(cond);
  if (!condExpr) {
    // 也可能是tensor级别的比较
    auto condTensor = dyn_cast<TensorExprSV>(cond);
    if (condTensor && condTensor->getOp() == OpKind::CmpLT) {
      // 获取元素级别的比较
      // 简化：假设可以获取第一个元素的模式
    }
    return false;
  }

  // 检查是否为 idx < bound
  if (condExpr->getOp() == ScalarExprSV::OpKind::CmpLT) {
    // trueVal应该是idx，falseVal应该是0或padding值
    if (condExpr->getLHS() == trueVal) {
      // 检查bound是否为常量
      if (auto boundConst = dyn_cast<ScalarConstantIntSV>(condExpr->getRHS())) {
        range = trueVal;
        bound = boundConst->getInt();
        return true;
      }
    }
  }

  return false;
}

bool TensorExprSV::isMinPattern() const {
  // min(a, b) = select(a < b, a, b)
  if (op != OpKind::Select) return false;

  auto cond = operands[0];
  auto trueVal = operands[1];
  auto falseVal = operands[2];

  if (auto condExpr = dyn_cast<ScalarExprSV>(cond)) {
    if (condExpr->getOp() == ScalarExprSV::OpKind::CmpLT) {
      return condExpr->getLHS() == trueVal && condExpr->getRHS() == falseVal;
    }
  }
  return false;
}

bool TensorExprSV::isMaxPattern() const {
  // max(a, b) = select(a > b, a, b)
  if (op != OpKind::Select) return false;

  auto cond = operands[0];
  auto trueVal = operands[1];
  auto falseVal = operands[2];

  if (auto condExpr = dyn_cast<ScalarExprSV>(cond)) {
    if (condExpr->getOp() == ScalarExprSV::OpKind::CmpGT) {
      return condExpr->getLHS() == trueVal && condExpr->getRHS() == falseVal;
    }
  }
  return false;
}

int64_t PtrTensorSV::getLinearIndex(ArrayRef<int64_t> indices) const {
  int64_t linear = 0;
  int64_t stride = 1;
  for (int i = shape.size() - 1; i >= 0; --i) {
    linear += indices[i] * stride;
    stride *= shape[i];
  }
  return linear;
}

void PtrTensorSV::setElementOffset(ArrayRef<int64_t> indices, SymValue* offset) {
  int64_t linear = getLinearIndex(indices);
  elementOffsets[linear] = offset;
}

SymValue* PtrTensorSV::getElementOffset(ArrayRef<int64_t> indices) const {
  int64_t linear = getLinearIndex(indices);
  auto it = elementOffsets.find(linear);
  if (it != elementOffsets.end()) {
    return it->second;
  }
  return nullptr;
}

SymValue* PtrTensorSV::getElementOffset(int64_t linearIdx) const {
  auto it = elementOffsets.find(linearIdx);
  if (it != elementOffsets.end()) {
    return it->second;
  }
  return nullptr;
}

SmallVector<int64_t> PtrTensorSV::inferStrides() const {
  // 基于offsets推导stride信息
  // 假设是row-major布局
  SmallVector<int64_t> strides(shape.size(), 1);

  // 从shape末尾开始计算
  for (int i = shape.size() - 2; i >= 0; --i) {
    strides[i] = strides[i + 1] * shape[i + 1];
  }

  // TODO: 基于实际offsets验证/调整strides
  return strides;
}

//===----------------------------------------------------------------------===//
// 辅助函数实现
//===----------------------------------------------------------------------===//

ScalarExprSV* mlir::triton::ascend::createBinaryExpr(
    ScalarExprSV::OpKind op, SymValue* lhs, SymValue* rhs) {
  auto expr = new ScalarExprSV(op, lhs, rhs);

  // 尝试简化
  if (auto simplified = expr->simplify()) {
    if (simplified != expr) {
      // 简化成功，返回简化后的值
      // 注意：这里可能会有内存泄漏，实际使用时需要更好的内存管理
      return dyn_cast<ScalarExprSV>(simplified) ?
             dyn_cast<ScalarExprSV>(simplified) : expr;
    }
  }
  return expr;
}

llvm::Optional<int64_t> mlir::triton::ascend::getConstantInt(SymValue* sv) {
  if (auto intConst = dyn_cast<ScalarConstantIntSV>(sv)) {
    return intConst->getInt();
  }
  return llvm::None;
}

llvm::Optional<double> mlir::triton::ascend::getConstantFloat(SymValue* sv) {
  if (auto floatConst = dyn_cast<ScalarConstantFloatSV>(sv)) {
    return floatConst->getFloat();
  }
  return llvm::None;
}

//===----------------------------------------------------------------------===//
// SymbolicExecutionState 实现
//===----------------------------------------------------------------------===//

SymValue* SymbolicExecutionState::getSymValue(Value v) const {
  auto it = valueMap.find(v);
  if (it != valueMap.end()) {
    return it->second;
  }
  return nullptr;
}

void SymbolicExecutionState::setSymValue(Value v, SymValue* sv) {
  valueMap[v] = sv;
}

bool SymbolicExecutionState::hasSymValue(Value v) const {
  return valueMap.count(v) > 0;
}

void SymbolicExecutionState::enterLoop(scf::ForOp loop, SymValue* iv,
                                       SymValue* lb, SymValue* ub,
                                       SymValue* step) {
  loopStack.emplace_back(loop, iv, lb, ub, step);
}

void SymbolicExecutionState::exitLoop() {
  if (!loopStack.empty()) {
    loopStack.pop_back();
  }
}

const LoopContext& SymbolicExecutionState::getCurrentLoop() const {
  assert(!loopStack.empty() && "Not in a loop context");
  return loopStack.back();
}

void SymbolicExecutionState::enterCondition(Value cond, bool isTrueBranch) {
  conditionStack.emplace_back(cond, isTrueBranch);
}

void SymbolicExecutionState::exitCondition() {
  if (!conditionStack.empty()) {
    conditionStack.pop_back();
  }
}

void SymbolicExecutionState::mergeStates(
    SymbolicExecutionState& result,
    const SymbolicExecutionState& trueState,
    const SymbolicExecutionState& falseState,
    Value condition) {
  // 获取condition的SymValue
  SymValue* condSym = result.getSymValue(condition);
  if (!condSym) {
    // 如果condition没有符号值，无法合并
    return;
  }

  // 合并两个状态的values
  for (const auto& pair : trueState.valueMap) {
    Value v = pair.first;
    SymValue* trueSym = pair.second;

    auto falseIt = falseState.valueMap.find(v);
    if (falseIt != falseState.valueMap.end()) {
      SymValue* falseSym = falseIt->second;

      // 如果两个分支的值相同，不需要Select
      if (trueSym == falseSym) {
        result.setSymValue(v, trueSym);
      } else {
        // 创建Select表达式
        auto select = new ScalarExprSV(condSym, trueSym, falseSym);
        result.setSymValue(v, select);
      }
    }
  }
}

void SymbolicExecutionState::print(llvm::raw_ostream& os) const {
  os << "=== Symbolic Execution State ===\n";
  os << "Value mappings:\n";
  for (const auto& pair : valueMap) {
    os << "  " << pair.first << " -> ";
    if (pair.second) {
      pair.second->print(os);
    } else {
      os << "null";
    }
    os << "\n";
  }
  os << "Loop contexts: " << loopStack.size() << "\n";
  os << "Condition contexts: " << conditionStack.size() << "\n";
}

//===----------------------------------------------------------------------===//
// SymbolicExecutionEngine 实现
//===----------------------------------------------------------------------===//

void SymbolicExecutionEngine::executeBlock(Block* block,
                                           SymbolicExecutionState& state) {
  for (auto& op : block->getOperations()) {
    executeOperation(&op, state);
  }
}

void SymbolicExecutionEngine::executeOperations(
    ArrayRef<Operation*> ops, SymbolicExecutionState& state) {
  for (auto* op : ops) {
    executeOperation(op, state);
  }
}

void SymbolicExecutionEngine::executeOperation(Operation* op,
                                               SymbolicExecutionState& state) {
  // 根据操作类型分发
  if (auto constOp = dyn_cast<arith::ConstantOp>(op)) {
    executeArithConstant(constOp, state);
  } else if (auto binOp = dyn_cast<ArithmeticOpInterface>(op)) {
    executeArithBinary(binOp, state);
  } else if (auto selectOp = dyn_cast<arith::SelectOp>(op)) {
    executeArithSelect(selectOp, state);
  } else if (auto cmpIOp = dyn_cast<arith::CmpIOp>(op)) {
    executeArithCmpI(cmpIOp, state);
  } else if (auto cmpFOp = dyn_cast<arith::CmpFOp>(op)) {
    executeArithCmpF(cmpFOp, state);
  } else if (isa<arith::IndexCastOp, arith::SIToFPOp,
                 arith::FPToSIOp, arith::ExtSIOp, arith::TruncIOp>(op)) {
    executeArithCast(op, state);
  } else if (auto pidOp = dyn_cast<tt::GetProgramIdOp>(op)) {
    executeGetProgramID(pidOp, state);
  } else if (auto rangeOp = dyn_cast<tt::MakeRangeOp>(op)) {
    executeMakeRange(rangeOp, state);
  } else if (auto splatOp = dyn_cast<tt::SplatOp>(op)) {
    executeSplat(splatOp, state);
  } else if (auto addPtrOp = dyn_cast<tt::AddPtrOp>(op)) {
    executeAddPtr(addPtrOp, state);
  } else if (auto expandDimsOp = dyn_cast<tt::ExpandDimsOp>(op)) {
    executeExpandDims(expandDimsOp, state);
  } else if (auto broadcastOp = dyn_cast<tt::BroadcastOp>(op)) {
    executeBroadcast(broadcastOp, state);
  } else if (auto makePtrOp = dyn_cast<tt::MakeTensorPtrOp>(op)) {
    executeMakeTensorPtr(makePtrOp, state);
  } else if (auto advanceOp = dyn_cast<tt::AdvanceOp>(op)) {
    executeAdvance(advanceOp, state);
  } else if (auto forOp = dyn_cast<scf::ForOp>(op)) {
    executeForLoop(forOp, state);
  } else if (auto ifOp = dyn_cast<scf::IfOp>(op)) {
    executeIfOp(ifOp, state);
  } else if (auto yieldOp = dyn_cast<scf::YieldOp>(op)) {
    executeYield(yieldOp, state);
  } else {
    // 对于未处理的操作，尝试简单处理其操作数
    // 如果操作有结果但未被处理，创建一个Unknown值
    LLVM_DEBUG(llvm::dbgs() << "Unhandled op: " << op->getName() << "\n");
  }
}

//===----------------------------------------------------------------------===//
// Arith 指令执行
//===----------------------------------------------------------------------===//

void SymbolicExecutionEngine::executeArithConstant(
    arith::ConstantOp op, SymbolicExecutionState& state) {
  Type type = op.getType();
  Attribute attr = op.getValue();

  if (auto intType = dyn_cast<IntegerType>(type)) {
    if (auto intAttr = dyn_cast<IntegerAttr>(attr)) {
      auto sv = new ScalarConstantIntSV(intAttr.getInt());
      sv->setElementType(type);
      state.setSymValue(op.getResult(), sv);
    }
  } else if (isa<FloatType>(type)) {
    if (auto floatAttr = dyn_cast<FloatAttr>(attr)) {
      auto sv = new ScalarConstantFloatSV(floatAttr.getValue().convertToDouble());
      sv->setElementType(type);
      state.setSymValue(op.getResult(), sv);
    }
  }
}

void SymbolicExecutionEngine::executeArithBinary(
    ArithmeticOpInterface op, SymbolicExecutionState& state) {
  Value lhs = op->getOperand(0);
  Value rhs = op->getOperand(1);
  Value result = op->getResult(0);

  SymValue* lhsSym = state.getSymValue(lhs);
  SymValue* rhsSym = state.getSymValue(rhs);

  if (!lhsSym || !rhsSym) {
    // 操作数没有符号值，无法继续
    return;
  }

  auto opKindOpt = getArithOpKind(op);
  if (!opKindOpt) {
    return;
  }

  auto sv = createBinaryExpr(*opKindOpt, lhsSym, rhsSym);
  sv->setElementType(result.getType());
  state.setSymValue(result, sv);
}

void SymbolicExecutionEngine::executeArithSelect(
    arith::SelectOp op, SymbolicExecutionState& state) {
  Value cond = op.getCondition();
  Value trueVal = op.getTrueValue();
  Value falseVal = op.getFalseValue();
  Value result = op.getResult();

  SymValue* condSym = state.getSymValue(cond);
  SymValue* trueSym = state.getSymValue(trueVal);
  SymValue* falseSym = state.getSymValue(falseVal);

  if (!condSym || !trueSym || !falseSym) {
    return;
  }

  auto sv = new ScalarExprSV(condSym, trueSym, falseSym);
  sv->setElementType(result.getType());
  state.setSymValue(result, sv);
}

void SymbolicExecutionEngine::executeArithCmpI(
    arith::CmpIOp op, SymbolicExecutionState& state) {
  Value lhs = op.getLhs();
  Value rhs = op.getRhs();
  Value result = op.getResult();

  SymValue* lhsSym = state.getSymValue(lhs);
  SymValue* rhsSym = state.getSymValue(rhs);

  if (!lhsSym || !rhsSym) {
    return;
  }

  auto opKind = getCmpIOpKind(op.getPredicate());
  auto sv = new ScalarExprSV(opKind, lhsSym, rhsSym);
  sv->setElementType(result.getType());
  state.setSymValue(result, sv);
}

void SymbolicExecutionEngine::executeArithCmpF(
    arith::CmpFOp op, SymbolicExecutionState& state) {
  Value lhs = op.getLhs();
  Value rhs = op.getRhs();
  Value result = op.getResult();

  SymValue* lhsSym = state.getSymValue(lhs);
  SymValue* rhsSym = state.getSymValue(rhs);

  if (!lhsSym || !rhsSym) {
    return;
  }

  auto opKind = getCmpFOpKind(op.getPredicate());
  auto sv = new ScalarExprSV(opKind, lhsSym, rhsSym);
  sv->setElementType(result.getType());
  state.setSymValue(result, sv);
}

void SymbolicExecutionEngine::executeArithCast(
    Operation* op, SymbolicExecutionState& state) {
  // 对于cast操作，我们简单地传递源值的SymValue
  // 因为在符号执行层面，类型转换不影响值的结构
  if (op->getNumOperands() == 1 && op->getNumResults() == 1) {
    Value input = op->getOperand(0);
    Value result = op->getResult(0);

    if (SymValue* inputSym = state.getSymValue(input)) {
      // 简单传递，可能需要创建一个新的SymValue保持类型信息
      state.setSymValue(result, inputSym);
    }
  }
}

//===----------------------------------------------------------------------===//
// Triton 指令执行
//===----------------------------------------------------------------------===//

void SymbolicExecutionEngine::executeGetProgramID(
    tt::GetProgramIdOp op, SymbolicExecutionState& state) {
  int axis = static_cast<int>(op.getAxis());
  auto sv = new ProgramIDSV(axis);
  sv->setElementType(op.getResult().getType());
  state.setSymValue(op.getResult(), sv);
}

void SymbolicExecutionEngine::executeMakeRange(
    tt::MakeRangeOp op, SymbolicExecutionState& state) {
  int64_t start = op.getStart();
  int64_t end = op.getEnd();

  auto sv = new TensorRangeSV(start, end);
  sv->setElementType(op.getResult().getType().getElementType());
  state.setSymValue(op.getResult(), sv);
}

void SymbolicExecutionEngine::executeSplat(
    tt::SplatOp op, SymbolicExecutionState& state) {
  Value input = op.getSrc();
  Value result = op.getResult();

  SymValue* inputSym = state.getSymValue(input);
  if (!inputSym) {
    return;
  }

  // 获取输出tensor的形状
  auto tensorType = dyn_cast<RankedTensorType>(result.getType());
  if (!tensorType) {
    return;
  }

  SmallVector<int64_t> shape(tensorType.getShape());
  auto sv = new TensorSplatSV(shape, inputSym);
  sv->setElementType(tensorType.getElementType());
  state.setSymValue(result, sv);
}

void SymbolicExecutionEngine::executeAddPtr(
    tt::AddPtrOp op, SymbolicExecutionState& state) {
  Value ptr = op.getPtr();
  Value offset = op.getOffset();
  Value result = op.getResult();

  SymValue* ptrSym = state.getSymValue(ptr);
  SymValue* offsetSym = state.getSymValue(offset);

  if (!ptrSym || !offsetSym) {
    return;
  }

  // 判断是标量指针还是tensor指针
  Type ptrType = ptr.getType();
  Type resultType = result.getType();

  if (isa<tt::PointerType>(ptrType)) {
    // 标量指针
    auto sv = new PtrBaseSV(result, offsetSym,
                            cast<tt::PointerType>(ptrType).getPointeeType());
    sv->setElementType(resultType);
    state.setSymValue(result, sv);
  } else if (auto tensorType = dyn_cast<RankedTensorType>(ptrType)) {
    // Tensor指针
    SmallVector<int64_t> shape(tensorType.getShape());

    // 获取pointee类型
    Type pointeeType;
    if (auto ptrTensorType = dyn_cast<tt::PointerType>(tensorType.getElementType())) {
      pointeeType = ptrTensorType.getPointeeType();
    }

    auto sv = new PtrTensorSV(result, shape, pointeeType);

    // 计算每个元素的offset
    // 如果offset是tensor，需要遍历计算
    if (auto offsetTensor = dyn_cast<TensorExprSV>(offsetSym)) {
      // 元素级操作
      int64_t numElements = 1;
      for (auto s : shape) numElements *= s;

      for (int64_t i = 0; i < numElements; ++i) {
        SmallVector<int64_t> indices(shape.size());
        int64_t tmp = i;
        for (int j = shape.size() - 1; j >= 0; --j) {
          indices[j] = tmp % shape[j];
          tmp /= shape[j];
        }

        SymValue* elemOffset = offsetTensor->getElement(indices);
        sv->setElementOffset(indices, elemOffset);
      }
    } else if (auto offsetSplat = dyn_cast<TensorSplatSV>(offsetSym)) {
      // splat offset，所有元素相同
      int64_t numElements = 1;
      for (auto s : shape) numElements *= s;

      for (int64_t i = 0; i < numElements; ++i) {
        SmallVector<int64_t> indices(shape.size());
        int64_t tmp = i;
        for (int j = shape.size() - 1; j >= 0; --j) {
          indices[j] = tmp % shape[j];
          tmp /= shape[j];
        }
        sv->setElementOffset(indices, offsetSplat->getElementValue());
      }
    }

    sv->setElementType(tensorType.getElementType());
    state.setSymValue(result, sv);
  }
}

void SymbolicExecutionEngine::executeExpandDims(
    tt::ExpandDimsOp op, SymbolicExecutionState& state) {
  Value input = op.getSrc();
  Value result = op.getResult();
  int64_t axis = op.getAxis();

  SymValue* inputSym = state.getSymValue(input);
  if (!inputSym) {
    return;
  }

  auto tensorType = dyn_cast<RankedTensorType>(result.getType());
  if (!tensorType) {
    return;
  }

  SmallVector<int64_t> shape(tensorType.getShape());

  // 如果输入也是TensorExpr，构建新的TensorExpr
  if (auto inputTensor = dyn_cast<TensorExprSV>(inputSym)) {
    auto sv = new TensorExprSV(inputTensor, axis, shape);
    sv->setElementType(tensorType.getElementType());
    state.setSymValue(result, sv);
  } else if (auto inputSplat = dyn_cast<TensorSplatSV>(inputSym)) {
    // splat经过expand_dims仍然是splat
    auto sv = new TensorSplatSV(shape, inputSplat->getElementValue());
    sv->setElementType(tensorType.getElementType());
    state.setSymValue(result, sv);
  } else if (auto inputRange = dyn_cast<TensorRangeSV>(inputSym)) {
    // range经过expand_dims变成TensorExpr
    auto sv = new TensorExprSV(inputRange, axis, shape);
    sv->setElementType(tensorType.getElementType());
    state.setSymValue(result, sv);
  }
}

void SymbolicExecutionEngine::executeBroadcast(
    tt::BroadcastOp op, SymbolicExecutionState& state) {
  Value input = op.getSrc();
  Value result = op.getResult();

  SymValue* inputSym = state.getSymValue(input);
  if (!inputSym) {
    return;
  }

  auto tensorType = dyn_cast<RankedTensorType>(result.getType());
  if (!tensorType) {
    return;
  }

  SmallVector<int64_t> resultShape(tensorType.getShape());

  // 计算广播维度
  auto inputType = dyn_cast<RankedTensorType>(input.getType());
  if (!inputType) {
    return;
  }

  SmallVector<int64_t> inputShape(inputType.getShape());
  SmallVector<int64_t> broadcastDims;

  // 找出哪些维度被广播了
  // 假设输入shape长度 <= 输出shape长度
  int inputIdx = inputShape.size() - 1;
  for (int i = resultShape.size() - 1; i >= 0; --i) {
    if (inputIdx >= 0) {
      if (inputShape[inputIdx] == 1 && resultShape[i] > 1) {
        broadcastDims.push_back(i);
      }
      inputIdx--;
    } else {
      // 新增的维度
      broadcastDims.push_back(i);
    }
  }

  // 如果输入是TensorExpr，构建新的TensorExpr
  if (auto inputTensor = dyn_cast<TensorExprSV>(inputSym)) {
    auto sv = new TensorExprSV(inputTensor, resultShape, broadcastDims);
    sv->setElementType(tensorType.getElementType());
    state.setSymValue(result, sv);
  } else if (auto inputSplat = dyn_cast<TensorSplatSV>(inputSym)) {
    auto sv = new TensorSplatSV(resultShape, inputSplat->getElementValue());
    sv->setElementType(tensorType.getElementType());
    state.setSymValue(result, sv);
  } else if (auto inputRange = dyn_cast<TensorRangeSV>(inputSym)) {
    auto sv = new TensorExprSV(inputRange, 0, resultShape);  // axis=0简化处理
    sv->setElementType(tensorType.getElementType());
    state.setSymValue(result, sv);
  }
}

void SymbolicExecutionEngine::executeMakeTensorPtr(
    tt::MakeTensorPtrOp op, SymbolicExecutionState& state) {
  // MakeTensorPtr创建一个结构化的tensor指针
  // 在符号执行中，我们创建一个PtrTensorSV

  Value base = op.getBase();
  Value result = op.getResult();

  // 获取形状信息
  auto resultType = dyn_cast<tt::PointerType>(result.getType());
  if (!resultType) {
    return;
  }

  auto tensorType = dyn_cast<RankedTensorType>(resultType.getPointeeType());
  if (!tensorType) {
    return;
  }

  SmallVector<int64_t> shape(tensorType.getShape());

  // 获取offsets（这些是MLIR Value，需要查找对应的SymValue）
  auto offsets = op.getOffsets();

  // 获取strides
  auto strides = op.getStrides();

  // 创建PtrTensorSV
  auto sv = new PtrTensorSV(result, shape, tensorType.getElementType());

  // 根据offsets设置初始元素偏移
  // 这里简化处理，假设是连续的row-major布局
  int64_t numElements = 1;
  for (auto s : shape) numElements *= s;

  for (int64_t i = 0; i < numElements; ++i) {
    SmallVector<int64_t> indices(shape.size());
    int64_t tmp = i;
    SmallVector<int64_t> inferredStrides = sv->inferStrides();

    for (int j = shape.size() - 1; j >= 0; --j) {
      indices[j] = tmp % shape[j];
      tmp /= shape[j];
    }

    // 计算线性偏移: sum(index[i] * stride[i])
    // 从offsets获取基偏移
    // 这里简化处理，假设offsets是标量或可以直接使用
    // 实际的偏移计算应该考虑所有维度

    // 简化：对于第一个维度，使用offsets[0]的SymValue
    if (offsets.size() > 0) {
      SymValue* baseOffset = state.getSymValue(offsets[0]);
      if (!baseOffset) {
        baseOffset = new ScalarConstantIntSV(0);
      }

      // 计算这个元素相对于baseOffset的额外偏移
      int64_t extraOffset = 0;
      for (size_t j = 1; j < shape.size(); ++j) {
        extraOffset += indices[j] * inferredStrides[j];
      }

      if (extraOffset == 0) {
        sv->setElementOffset(indices, baseOffset);
      } else {
        auto extraConst = new ScalarConstantIntSV(extraOffset);
        auto totalOffset = new ScalarExprSV(
            ScalarExprSV::OpKind::Add, baseOffset, extraConst);
        sv->setElementOffset(indices, totalOffset);
      }
    }
  }

  sv->setElementType(resultType);
  state.setSymValue(result, sv);
}

void SymbolicExecutionEngine::executeAdvance(
    tt::AdvanceOp op, SymbolicExecutionState& state) {
  // Advance更新tensor指针的offsets
  Value ptr = op.getPtr();
  Value result = op.getResult();

  SymValue* ptrSym = state.getSymValue(ptr);
  if (!ptrSym) {
    return;
  }

  auto offsets = op.getOffsets();

  // 获取PtrTensorSV并更新offsets
  if (auto ptrTensor = dyn_cast<PtrTensorSV>(ptrSym)) {
    // 创建新的PtrTensorSV（advance产生新指针）
    auto sv = new PtrTensorSV(result, ptrTensor->getShape(),
                               ptrTensor->getPointeeType());

    // 复制原有的offsets并加上advance的量
    auto oldOffsets = ptrTensor->getAllOffsets();
    for (const auto& pair : oldOffsets) {
      uint64_t linearIdx = pair.first;
      SymValue* oldOffset = pair.second;

      // 如果offsets非空，添加advance
      if (!offsets.empty()) {
        // 简化处理：假设第一个offset是主要的advance量
        SymValue* advanceVal = state.getSymValue(offsets[0]);
        if (!advanceVal) {
          advanceVal = new ScalarConstantIntSV(0);
        }

        // 计算新的offset
        auto newOffset = new ScalarExprSV(
            ScalarExprSV::OpKind::Add, oldOffset, advanceVal);
        sv->setElementOffset(linearIdx, newOffset);
      } else {
        sv->setElementOffset(linearIdx, oldOffset);
      }
    }

    sv->setElementType(ptrTensor->getElementType());
    state.setSymValue(result, sv);
  }
}

//===----------------------------------------------------------------------===//
// 控制流指令执行
//===----------------------------------------------------------------------===//

void SymbolicExecutionEngine::executeForLoop(
    scf::ForOp loop, SymbolicExecutionState& state) {
  // 获取循环边界
  Value lb = loop.getLowerBound();
  Value ub = loop.getUpperBound();
  Value step = loop.getStep();

  SymValue* lbSym = state.getSymValue(lb);
  SymValue* ubSym = state.getSymValue(ub);
  SymValue* stepSym = state.getSymValue(step);

  // 如果没有符号值，尝试从constant创建
  if (!lbSym) {
    if (auto constOp = lb.getDefiningOp<arith::ConstantOp>()) {
      executeArithConstant(constOp, state);
      lbSym = state.getSymValue(lb);
    }
  }

  if (!ubSym) {
    if (auto constOp = ub.getDefiningOp<arith::ConstantOp>()) {
      executeArithConstant(constOp, state);
      ubSym = state.getSymValue(ub);
    }
  }

  if (!stepSym) {
    if (auto constOp = step.getDefiningOp<arith::ConstantOp>()) {
      executeArithConstant(constOp, state);
      stepSym = state.getSymValue(step);
    }
  }

  // 创建迭代变量的符号表示
  // 使用LoopIterSV表示一个范围值
  auto ivSym = new TensorRangeSV(
      lbSym ? getConstantInt(lbSym).getValueOr(0) : 0,
      ubSym ? getConstantInt(ubSym).getValueOr(1) : 1);
  ivSym->setElementType(loop.getInductionVar().getType());

  // 进入循环上下文
  state.enterLoop(loop, ivSym, lbSym, ubSym, stepSym);

  // 设置迭代变量的符号值
  state.setSymValue(loop.getInductionVar(), ivSym);

  // 处理iter_args
  handleIterArgs(loop, state);

  // 执行循环体一次（符号化执行，不展开所有迭代）
  executeBlock(loop.getBody(), state);

  // 处理循环yield
  if (auto yieldOp = dyn_cast<scf::YieldOp>(loop.getBody()->getTerminator())) {
    handleLoopYield(loop, yieldOp, state);
  }

  // 退出循环上下文
  state.exitLoop();
}

void SymbolicExecutionEngine::handleIterArgs(
    scf::ForOp loop, SymbolicExecutionState& state) {
  auto iterArgs = loop.getIterOperands();
  auto regionIterArgs = loop.getRegionIterArgs();

  for (size_t i = 0; i < iterArgs.size(); ++i) {
    Value initVal = iterArgs[i];
    Value regionArg = regionIterArgs[i];

    if (SymValue* initSym = state.getSymValue(initVal)) {
      state.setSymValue(regionArg, initSym);
    }
  }
}

void SymbolicExecutionEngine::handleLoopYield(
    scf::ForOp loop, scf::YieldOp yield,
    SymbolicExecutionState& state) {
  // 循环yield的结果将成为iter_args的新值
  auto results = yield.getOperands();
  auto iterArgs = loop.getRegionIterArgs();

  for (size_t i = 0; i < results.size() && i < iterArgs.size(); ++i) {
    Value result = results[i];
    Value iterArg = iterArgs[i];

    if (SymValue* resultSym = state.getSymValue(result)) {
      // 在循环上下文中，iter_arg在yield后更新
      // 这里我们简单设置iter_arg的新值
      // 实际分析可能需要考虑归纳关系
      state.setSymValue(iterArg, resultSym);
    }
  }
}

void SymbolicExecutionEngine::executeIfOp(
    scf::IfOp op, SymbolicExecutionState& state) {
  Value condition = op.getCondition();

  // 保存当前状态
  SymbolicExecutionState trueState = state;
  SymbolicExecutionState falseState = state;

  // 进入true分支
  trueState.enterCondition(condition, true);
  if (op.thenBlock()) {
    executeBlock(op.thenBlock(), trueState);
  }

  // 进入false分支
  falseState.enterCondition(condition, false);
  if (op.elseBlock()) {
    executeBlock(op.elseBlock(), falseState);
  }

  // 合并两个分支的状态
  SymbolicExecutionState::mergeStates(state, trueState, falseState, condition);

  // 退出条件上下文
  state.exitCondition();
}

void SymbolicExecutionEngine::executeYield(
    scf::YieldOp op, SymbolicExecutionState& state) {
  // Yield本身不创建新的符号值
  // 值已经在操作数中定义
  // 这个函数主要用于循环yield的特殊处理（在handleLoopYield中）
}

//===----------------------------------------------------------------------===//
// 辅助方法
//===----------------------------------------------------------------------===//

llvm::Optional<ScalarExprSV::OpKind>
SymbolicExecutionEngine::getArithOpKind(ArithmeticOpInterface op) {
  StringRef opName = op->getName().getStringRef();

  if (opName == "arith.addi" || opName == "arith.addf") {
    return ScalarExprSV::OpKind::Add;
  } else if (opName == "arith.subi" || opName == "arith.subf") {
    return ScalarExprSV::OpKind::Sub;
  } else if (opName == "arith.muli" || opName == "arith.mulf") {
    return ScalarExprSV::OpKind::Mul;
  } else if (opName == "arith.divsi" || opName == "arith.divui" ||
             opName == "arith.divf") {
    return ScalarExprSV::OpKind::Div;
  }

  return llvm::None;
}

ScalarExprSV::OpKind SymbolicExecutionEngine::getCmpIOpKind(
    arith::CmpIPredicate pred) {
  switch (pred) {
    case arith::CmpIPredicate::eq: return ScalarExprSV::OpKind::CmpEQ;
    case arith::CmpIPredicate::ne: return ScalarExprSV::OpKind::CmpNE;
    case arith::CmpIPredicate::slt:
    case arith::CmpIPredicate::ult: return ScalarExprSV::OpKind::CmpLT;
    case arith::CmpIPredicate::sle:
    case arith::CmpIPredicate::ule: return ScalarExprSV::OpKind::CmpLE;
    case arith::CmpIPredicate::sgt:
    case arith::CmpIPredicate::ugt: return ScalarExprSV::OpKind::CmpGT;
    case arith::CmpIPredicate::sge:
    case arith::CmpIPredicate::uge: return ScalarExprSV::OpKind::CmpGE;
  }
  return ScalarExprSV::OpKind::CmpEQ;  // default
}

ScalarExprSV::OpKind SymbolicExecutionEngine::getCmpFOpKind(
    arith::CmpFPredicate pred) {
  switch (pred) {
    case arith::CmpFPredicate::OEQ:
    case arith::CmpFPredicate::UEQ: return ScalarExprSV::OpKind::CmpEQ;
    case arith::CmpFPredicate::ONE:
    case arith::CmpFPredicate::UNE: return ScalarExprSV::OpKind::CmpNE;
    case arith::CmpFPredicate::OLT:
    case arith::CmpFPredicate::ULT: return ScalarExprSV::OpKind::CmpLT;
    case arith::CmpFPredicate::OLE:
    case arith::CmpFPredicate::ULE: return ScalarExprSV::OpKind::CmpLE;
    case arith::CmpFPredicate::OGT:
    case arith::CmpFPredicate::UGT: return ScalarExprSV::OpKind::CmpGT;
    case arith::CmpFPredicate::OGE:
    case arith::CmpFPredicate::UGE: return ScalarExprSV::OpKind::CmpGE;
    default: return ScalarExprSV::OpKind::CmpEQ;
  }
}
