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
// SymbolicExecutionState 实现
//===----------------------------------------------------------------------===//

std::shared_ptr<SymValue> SymbolicExecutionState::getSymValue(Value v) const {
  auto it = valueMap.find(v);
  if (it != valueMap.end()) {
    return it->second;
  }
  return nullptr;
}

std::shared_ptr<ScalarSV> SymbolicExecutionState::getScalarValue(Value v) const {
  auto sv = getSymValue(v);
  if (!sv) return nullptr;
  return (sv && ScalarSV::classof(sv.get()) ? std::static_pointer_cast<ScalarSV>(sv) : nullptr);
}

std::shared_ptr<TensorSV> SymbolicExecutionState::getTensorValue(Value v) const {
  auto sv = getSymValue(v);
  if (!sv) return nullptr;
  return (sv && TensorSV::classof(sv.get()) ? std::static_pointer_cast<TensorSV>(sv) : nullptr);
}

std::shared_ptr<ScalarSV> SymbolicExecutionState::getTensorElementExpr(Value v) const {
  auto tensor = getTensorValue(v);
  if (!tensor) return nullptr;
  return tensor->elementExpr;
}

void SymbolicExecutionState::setSymValue(Value v, std::shared_ptr<SymValue> sv) {
  valueMap[v] = std::move(sv);
}

bool SymbolicExecutionState::hasSymValue(Value v) const {
  return valueMap.count(v) > 0;
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
  } else if (isa<mlir::arith::AddIOp, mlir::arith::SubIOp, mlir::arith::MulIOp, mlir::arith::DivSIOp, mlir::arith::DivUIOp, mlir::arith::AddFOp, mlir::arith::SubFOp, mlir::arith::MulFOp, mlir::arith::DivFOp>(op)) {
    executeArithBinary(op, state);
  } else if (auto selectOp = dyn_cast<arith::SelectOp>(op)) {
    executeArithSelect(selectOp, state);
  } else if (auto cmpIOp = dyn_cast<arith::CmpIOp>(op)) {
    executeArithCmpI(cmpIOp, state);
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
  } else if (auto loadOp = dyn_cast<tt::LoadOp>(op)) {
    executeLoad(loadOp, state);
  } else if (auto forOp = dyn_cast<scf::ForOp>(op)) {
    executeForLoop(forOp, state);
  } else if (auto ifOp = dyn_cast<scf::IfOp>(op)) {
    executeIfOp(ifOp, state);
  } else if (auto yieldOp = dyn_cast<scf::YieldOp>(op)) {
    executeYield(yieldOp, state);
  } else {
    // 对于未处理的指令，如果有结果，创建 UnknownSV
    for (Value result : op->getResults()) {
      if (!state.hasSymValue(result)) {
        auto sv = createUnknownSV(result.getType());
        if (sv) {
          state.setSymValue(result, std::move(sv));
        }
      }
    }
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
      auto sv = std::make_shared<ScalarConstantIntSV>(intAttr.getInt(), type);
      state.setSymValue(op.getResult(), std::move(sv));
    }
  } else if (isa<FloatType>(type)) {
    if (auto floatAttr = dyn_cast<FloatAttr>(attr)) {
      auto sv = std::make_shared<ScalarConstantFloatSV>(
          floatAttr.getValue().convertToDouble(), type);
      state.setSymValue(op.getResult(), std::move(sv));
    }
  } else if (auto tensorType = dyn_cast<RankedTensorType>(type)) {
    // 常量 Tensor（如 dense<0>）
    auto elemType = tensorType.getElementType();
    std::shared_ptr<ScalarSV> elemVal;
    if (auto denseAttr = dyn_cast<DenseElementsAttr>(attr)) {
      if (denseAttr.isSplat()) {
        if (isa<IntegerType>(elemType)) {
          elemVal = std::make_shared<ScalarConstantIntSV>(
              denseAttr.getSplatValue<APInt>().getSExtValue(), elemType);
        } else if (isa<FloatType>(elemType)) {
          elemVal = std::make_shared<ScalarConstantFloatSV>(
              denseAttr.getSplatValue<APFloat>().convertToDouble(), elemType);
        }
      }
    }
    if (!elemVal) {
      // 默认创建 0
      if (isa<IntegerType>(elemType)) {
        elemVal = std::make_shared<ScalarConstantIntSV>(0, elemType);
      } else {
        elemVal = std::make_shared<ScalarConstantFloatSV>(0.0, elemType);
      }
    }
    SmallVector<int64_t> shape(tensorType.getShape());
    auto sv = TensorSV::createSplat(std::move(elemVal), shape, elemType);
    state.setSymValue(op.getResult(), std::move(sv));
  }
}

void SymbolicExecutionEngine::executeArithBinary(
    Operation* op, SymbolicExecutionState& state) {
  Value lhs = op->getOperand(0);
  Value rhs = op->getOperand(1);
  Value result = op->getResult(0);

  Type resultType = result.getType();

  // 检查结果类型是 Scalar 还是 Tensor
  if (isa<RankedTensorType>(resultType)) {
    // Tensor 运算
    auto lhsSym = state.getTensorValue(lhs);
    auto rhsSym = state.getTensorValue(rhs);

    if (!lhsSym || !rhsSym) {
      // 创建 Unknown Tensor
      auto sv = createUnknownSV(resultType);
      if (sv) state.setSymValue(result, std::move(sv));
      return;
    }

    auto tensorType = cast<RankedTensorType>(resultType);
    SmallVector<int64_t> shape(tensorType.getShape());

    // 确定 SourceKind
    StringRef opName = op->getName().getStringRef();
    TensorSV::SourceKind sourceKind = TensorSV::SourceKind::Computed;
    if (opName == "arith.addi" || opName == "arith.addf") {
      sourceKind = TensorSV::SourceKind::Add;
    } else if (opName == "arith.subi" || opName == "arith.subf") {
      sourceKind = TensorSV::SourceKind::Sub;
    } else if (opName == "arith.muli" || opName == "arith.mulf") {
      sourceKind = TensorSV::SourceKind::Mul;
    } else if (opName == "arith.divsi" || opName == "arith.divui" ||
               opName == "arith.divf") {
      sourceKind = TensorSV::SourceKind::Div;
    } else if (opName == "arith.remsi" || opName == "arith.remui") {
      sourceKind = TensorSV::SourceKind::Rem;
    }

    auto sv = TensorSV::createComputed(sourceKind, lhsSym.get(), rhsSym.get());
    state.setSymValue(result, std::move(sv));
  } else {
    // Scalar 运算
    auto lhsSym = state.getScalarValue(lhs);
    auto rhsSym = state.getScalarValue(rhs);

    if (!lhsSym || !rhsSym) {
      // 创建 Unknown Scalar
      auto sv = createUnknownSV(resultType);
      if (sv) state.setSymValue(result, std::move(sv));
      return;
    }

    auto sv = createArithExpr(op->getName().getStringRef(),
                              lhsSym,
                              rhsSym,
                              resultType);
    if (sv) {
      state.setSymValue(result, std::move(sv));
    }
  }
}

void SymbolicExecutionEngine::executeArithSelect(
    arith::SelectOp op, SymbolicExecutionState& state) {
  Value cond = op.getCondition();
  Value trueVal = op.getTrueValue();
  Value falseVal = op.getFalseValue();
  Value result = op.getResult();

  Type resultType = result.getType();

  // 检查结果类型是 Scalar 还是 Tensor
  if (isa<RankedTensorType>(resultType)) {
    // Tensor select
    auto trueSym = state.getTensorValue(trueVal);
    auto falseSym = state.getTensorValue(falseVal);

    if (!trueSym || !falseSym) {
      auto sv = createUnknownSV(resultType);
      if (sv) state.setSymValue(result, std::move(sv));
      return;
    }

    // 获取 condition（可能是比较表达式）
    auto condScalar = state.getScalarValue(cond);
    auto condSym = (condScalar && CmpExprSV::classof(condScalar.get()) ? std::static_pointer_cast<CmpExprSV>(condScalar) : nullptr);

    // 使用专门的 createSelect 方法
    auto sv = TensorSV::createSelect(trueSym.get(), falseSym.get(), condSym);
    state.setSymValue(result, std::move(sv));
  } else {
    // Scalar select
    auto condScalar = state.getScalarValue(cond);
    auto trueSym = state.getScalarValue(trueVal);
    auto falseSym = state.getScalarValue(falseVal);

    if (!trueSym || !falseSym) {
      auto sv = createUnknownSV(resultType);
      if (sv) state.setSymValue(result, std::move(sv));
      return;
    }

    auto condSym = (condScalar && CmpExprSV::classof(condScalar.get()) ? std::static_pointer_cast<CmpExprSV>(condScalar) : nullptr);
    auto sv = std::make_shared<SelectExprSV>(
        condSym,
        trueSym,
        falseSym,
        resultType);

    state.setSymValue(result, std::move(sv));
  }
}

void SymbolicExecutionEngine::executeArithCmpI(
    arith::CmpIOp op, SymbolicExecutionState& state) {
  Value lhs = op.getLhs();
  Value rhs = op.getRhs();
  Value result = op.getResult();

  Type resultType = result.getType();

  // 检查结果类型是 Scalar 还是 Tensor
  if (isa<RankedTensorType>(resultType)) {
    // Tensor cmp
    auto lhsSym = state.getTensorValue(lhs);
    auto rhsSym = state.getTensorValue(rhs);

    if (!lhsSym || !rhsSym) {
      auto sv = createUnknownSV(resultType);
      if (sv) state.setSymValue(result, std::move(sv));
      return;
    }

    // 确定比较类型
    TensorSV::SourceKind cmpKind;
    switch (op.getPredicate()) {
      case arith::CmpIPredicate::eq: cmpKind = TensorSV::SourceKind::CmpEQ; break;
      case arith::CmpIPredicate::ne: cmpKind = TensorSV::SourceKind::CmpNE; break;
      case arith::CmpIPredicate::slt:
      case arith::CmpIPredicate::ult: cmpKind = TensorSV::SourceKind::CmpLT; break;
      case arith::CmpIPredicate::sle:
      case arith::CmpIPredicate::ule: cmpKind = TensorSV::SourceKind::CmpLE; break;
      case arith::CmpIPredicate::sgt:
      case arith::CmpIPredicate::ugt: cmpKind = TensorSV::SourceKind::CmpGT; break;
      case arith::CmpIPredicate::sge:
      case arith::CmpIPredicate::uge: cmpKind = TensorSV::SourceKind::CmpGE; break;
    }

    auto sv = TensorSV::createComputed(cmpKind, lhsSym.get(), rhsSym.get());
    state.setSymValue(result, std::move(sv));
  } else {
    // Scalar cmp
    auto lhsSym = state.getScalarValue(lhs);
    auto rhsSym = state.getScalarValue(rhs);

    if (!lhsSym || !rhsSym) {
      auto sv = createUnknownSV(resultType);
      if (sv) state.setSymValue(result, std::move(sv));
      return;
    }

    auto sv = std::make_shared<CmpExprSV>(
        getCmpIPred(op.getPredicate()),
        lhsSym,
        rhsSym,
        resultType);

    state.setSymValue(result, std::move(sv));
  }
}

//===----------------------------------------------------------------------===//
// Triton 指令执行
//===----------------------------------------------------------------------===//

void SymbolicExecutionEngine::executeGetProgramID(
    tt::GetProgramIdOp op, SymbolicExecutionState& state) {
  int axis = static_cast<int>(op.getAxis());
  auto sv = std::make_shared<ProgramIDSV>(axis, op.getResult().getType());
  state.setSymValue(op.getResult(), std::move(sv));
}

void SymbolicExecutionEngine::executeMakeRange(
    tt::MakeRangeOp op, SymbolicExecutionState& state) {
  int64_t start = op.getStart();
  int64_t end = op.getEnd();

  auto sv = TensorSV::createMakeRange(
      start, end,
      mlir::cast<RankedTensorType>(op.getResult().getType()).getElementType());
  state.setSymValue(op.getResult(), std::move(sv));
}

void SymbolicExecutionEngine::executeSplat(
    tt::SplatOp op, SymbolicExecutionState& state) {
  Value input = op.getSrc();
  Value result = op.getResult();

  auto inputSym = state.getScalarValue(input);
  if (!inputSym) {
    auto sv = createUnknownSV(result.getType());
    if (sv) state.setSymValue(result, std::move(sv));
    return;
  }

  auto tensorType = dyn_cast<RankedTensorType>(result.getType());
  if (!tensorType) {
    auto sv = createUnknownSV(result.getType());
    if (sv) state.setSymValue(result, std::move(sv));
    return;
  }

  SmallVector<int64_t> shape(tensorType.getShape());
  auto sv = TensorSV::createSplat(inputSym, shape,
                                  tensorType.getElementType());
  state.setSymValue(result, std::move(sv));
}

void SymbolicExecutionEngine::executeAddPtr(
    tt::AddPtrOp op, SymbolicExecutionState& state) {
  Value ptr = op.getPtr();
  Value offset = op.getOffset();
  Value result = op.getResult();

  Type ptrType = ptr.getType();
  Type resultType = result.getType();

  // 检查结果是 Scalar 还是 Tensor
  if (isa<RankedTensorType>(resultType)) {
    // Tensor addptr
    auto ptrSym = state.getTensorValue(ptr);
    auto offsetSym = state.getTensorValue(offset);

    if (!ptrSym || !offsetSym) {
      auto sv = createUnknownSV(resultType);
      if (sv) state.setSymValue(result, std::move(sv));
      return;
    }

    // 对于 Tensor addptr，创建一个 PtrExpr 作为 element
    auto resultTensorType = cast<RankedTensorType>(resultType);
    /*
    Type pointeeType;
    if (auto ptrType = dyn_cast<tt::PointerType>(resultTensorType.getElementType())) {
      pointeeType = ptrType.getPointeeType();
    } else {
      pointeeType = resultTensorType.getElementType();
    }
    */

    // 创建 element PtrExprSV (直接使用 tensor->elementExpr，避免 shared_from_this)
    auto elemPtrExpr = std::make_shared<PtrExprSV>(
        ptrSym->elementExpr,
        offsetSym->elementExpr,
        resultTensorType.getElementType());

    SmallVector<int64_t> shape(resultTensorType.getShape());
    Type pointeeType = resultTensorType.getElementType();
    auto sv = std::make_shared<TensorSV>(TensorSV::SourceKind::Computed,
                                         shape, pointeeType);
    sv->elementExpr = std::move(elemPtrExpr);
    state.setSymValue(result, std::move(sv));
  } else {
    // Scalar addptr
    auto ptrSym = state.getScalarValue(ptr);
    auto offsetSym = state.getScalarValue(offset);

    if (!ptrSym || !offsetSym) {
      auto sv = createUnknownSV(resultType);
      if (sv) state.setSymValue(result, std::move(sv));
      return;
    }

    /*
    Type pointeeType;
    if (auto pt = dyn_cast<tt::PointerType>(ptrType)) {
      pointeeType = pt.getPointeeType();
    } else {
      pointeeType = resultType;
    }
    */
    
    auto sv = std::make_shared<PtrExprSV>(
        ptrSym,
        offsetSym,
        resultType);
    state.setSymValue(result, std::move(sv));
  }
}

void SymbolicExecutionEngine::executeExpandDims(
    tt::ExpandDimsOp op, SymbolicExecutionState& state) {
  Value input = op.getSrc();
  Value result = op.getResult();
  int64_t axis = op.getAxis();

  auto inputSym = state.getTensorValue(input);
  if (!inputSym) {
    auto sv = createUnknownSV(result.getType());
    if (sv) state.setSymValue(result, std::move(sv));
    return;
  }

  auto sv = TensorSV::createExpandDims(inputSym.get(), axis);
  state.setSymValue(result, std::move(sv));
}

void SymbolicExecutionEngine::executeBroadcast(
    tt::BroadcastOp op, SymbolicExecutionState& state) {
  Value input = op.getSrc();
  Value result = op.getResult();

  auto inputSym = state.getTensorValue(input);
  if (!inputSym) {
    auto sv = createUnknownSV(result.getType());
    if (sv) state.setSymValue(result, std::move(sv));
    return;
  }

  auto tensorType = dyn_cast<RankedTensorType>(result.getType());
  if (!tensorType) {
    auto sv = createUnknownSV(result.getType());
    if (sv) state.setSymValue(result, std::move(sv));
    return;
  }

  SmallVector<int64_t> resultShape(tensorType.getShape());
  auto sv = TensorSV::createBroadcast(inputSym.get(), resultShape);
  state.setSymValue(result, std::move(sv));
}

void SymbolicExecutionEngine::executeMakeTensorPtr(
    tt::MakeTensorPtrOp op, SymbolicExecutionState& state) {
  Value base = op.getBase();

  auto resultType = dyn_cast<tt::PointerType>(op.getResult().getType());
  if (!resultType) {
    auto sv = createUnknownSV(op.getResult().getType());
    if (sv) state.setSymValue(op.getResult(), std::move(sv));
    return;
  }

  auto tensorType = dyn_cast<RankedTensorType>(resultType.getPointeeType());
  if (!tensorType) {
    auto sv = createUnknownSV(op.getResult().getType());
    if (sv) state.setSymValue(op.getResult(), std::move(sv));
    return;
  }

  // 获取 base 的符号值
  auto baseSym = state.getScalarValue(base);
  if (!baseSym) {
    // 尝试为 base 创建 GmPtrSV（如果是入参）
    if (isa<BlockArgument>(base)) {
      auto pt = dyn_cast<tt::PointerType>(base.getType());
      if (pt) {
        baseSym = std::make_shared<GmPtrSV>(base, pt.getPointeeType());
        state.setSymValue(base, baseSym);
      }
    }
  }

  // 收集 shape、strides、offsets（从 valueMap 或降级到常量）
  SmallVector<std::shared_ptr<ScalarSV>> shape;
  SmallVector<std::shared_ptr<ScalarSV>> strides;
  SmallVector<std::shared_ptr<ScalarSV>> offsets;
  SmallVector<int64_t> blockShape;

  // 辅助函数：获取 ScalarSV（优先从 valueMap，降级到常量，兜底 Unknown）
  auto getScalarForMakePtr = [&](Value val) -> std::shared_ptr<ScalarSV> {
    // 1. 优先从 valueMap 获取
    if (auto symVal = state.getScalarValue(val)) {
      return symVal;
    }

    // 2. 降级：尝试提取常量
    if (auto constOp = val.getDefiningOp<arith::ConstantOp>()) {
      if (auto intAttr = dyn_cast<IntegerAttr>(constOp.getValue())) {
        return std::make_shared<ScalarConstantIntSV>(
            intAttr.getInt(), val.getType());
      }
    }

    // 3. 兜底：创建 UnknownSV
    return std::make_shared<UnknownSV>(val.getType());
  };

  // 收集 shape（只收集 shape，不涉及 blockShape）
  for (auto s : op.getShape()) {
    shape.push_back(getScalarForMakePtr(s));
  }

  // 收集 strides
  for (auto s : op.getStrides()) {
    strides.push_back(getScalarForMakePtr(s));
  }

  // 收集 offsets
  for (auto o : op.getOffsets()) {
    offsets.push_back(getScalarForMakePtr(o));
  }

  // blockShape 从 result type 的 block type 获取
  auto tensorPtrType = mlir::cast<tt::PointerType>(op.getResult().getType());
  auto blockType = mlir::cast<RankedTensorType>(tensorPtrType.getPointeeType());
  for (auto bs : blockType.getShape()) {
    blockShape.push_back(bs);
  }

  // 创建 TensorPtrSV
  auto sv = std::make_shared<TensorPtrSV>(
      shape, strides, offsets, blockShape, tensorType.getElementType());

  state.setSymValue(op.getResult(), std::move(sv));
}

void SymbolicExecutionEngine::executeLoad(
    tt::LoadOp op, SymbolicExecutionState& state) {
  Value result = op.getResult();
  Type resultType = result.getType();

  // Load 返回 UnknownSV（区分 Scalar 还是 Tensor）
  auto sv = createUnknownSV(resultType);
  if (sv) {
    state.setSymValue(result, std::move(sv));
  }
}

//===----------------------------------------------------------------------===//
// 控制流指令执行（简化版）
//===----------------------------------------------------------------------===//

void SymbolicExecutionEngine::executeForLoop(
    scf::ForOp loop, SymbolicExecutionState& state) {
  // 1. 获取切片关注的 value
  Value* sliceValue = slice_ ? slice_->getDefinedValue(loop) : nullptr;
  Instruction* inst = cfg_->getInstruction(loop);

  // 2. 获取循环边界
  Value lb = loop.getLowerBound();
  Value ub = loop.getUpperBound();
  Value step = loop.getStep();

  // 3. 判断 sliceValue 类型并处理
  if (sliceValue) {
    Value sv = *sliceValue;

    // 3.1 如果是 InductionVar
    if (sv == loop.getInductionVar()) {
      auto inductionSv = std::make_shared<InductionSV>(
          inst,
          state.getScalarValue(lb),
          state.getScalarValue(ub),
          state.getScalarValue(step),
          sv.getType());
      state.setSymValue(sv, std::move(inductionSv));
      return;  // 切片只关注 iter_var，其他不处理
    }

    // 3.2 如果是 iter_args 中的某个
    auto regionIterArgs = loop.getRegionIterArgs();
    for (size_t i = 0; i < regionIterArgs.size(); ++i) {
      if (sv == regionIterArgs[i]) {
        // 使用 cfg->getIterArgPair 获取精确信息
        if (cfg_) {
          auto pairOpt = cfg_->getIterArgPair(inst->getParentBlock(), sv);
          if (pairOpt) {
            auto [initInst, yieldInst] = *pairOpt;
            auto iterArgSv = std::make_shared<IterArgSV>(
                inst, initInst, yieldInst, sv.getType());
            state.setSymValue(sv, std::move(iterArgSv));
            return;
          }
        }
      }
    }

    // 3.3 如果是 result → UnknownSV
    for (Value result : loop.getResults()) {
      if (sv == result) {
        auto svUnknown = createUnknownSV(sv.getType());
        if (svUnknown) state.setSymValue(sv, std::move(svUnknown));
        return;
      }
    }
  }
}

void SymbolicExecutionEngine::executeIfOp(
    scf::IfOp op, SymbolicExecutionState& state) {
  // 1. 获取切片关注的 value
  Value* sliceValue = slice_ ? slice_->getDefinedValue(op) : nullptr;

  // 2. 判断 sliceValue 是否是 result
  if (sliceValue) {
    Value sv = *sliceValue;
    for (Value result : op.getResults()) {
      if (sv == result) {
        // result → UnknownSV
        auto svUnknown = createUnknownSV(sv.getType());
        if (svUnknown) state.setSymValue(sv, std::move(svUnknown));
        return;  // 切片只关注此 result
      }
    }
  }

  // 3. 默认处理：所有 results 创建 UnknownSV
  for (Value result : op.getResults()) {
    auto sv = createUnknownSV(result.getType());
    if (sv) {
      state.setSymValue(result, std::move(sv));
    }
  }

  LLVM_DEBUG(llvm::dbgs() << "If op simplified: results created as Unknown\n");
}

void SymbolicExecutionEngine::executeYield(
    scf::YieldOp op, SymbolicExecutionState& state) {
  // Yield 本身不创建新的符号值
  // 值已经在操作数中定义
}

//===----------------------------------------------------------------------===//
// 辅助方法
//===----------------------------------------------------------------------===//

void SymbolicExecutionEngine::createSymValueForArgument(
    Value arg, SymbolicExecutionState& state) {
  if (state.hasSymValue(arg)) {
    return;
  }

  Type type = arg.getType();

  // 如果是指针类型，创建 GmPtrSV
  if (auto ptrType = dyn_cast<tt::PointerType>(type)) {
    auto sv = std::make_shared<GmPtrSV>(arg, ptrType.getPointeeType());
    state.setSymValue(arg, std::move(sv));
    return;
  }

  // 其他类型创建 UnknownSV
  auto sv = createUnknownSV(type);
  if (sv) {
    state.setSymValue(arg, std::move(sv));
  }
}

std::shared_ptr<SymValue> SymbolicExecutionEngine::createUnknownSV(Type type) {
  if (!type) {
    return nullptr;
  }

  // 根据类型创建 Scalar UnknownSV 或 Tensor UnknownSV
  if (isa<RankedTensorType>(type)) {
    auto tensorType = cast<RankedTensorType>(type);
    SmallVector<int64_t> shape(tensorType.getShape());
    return TensorSV::createLoad(shape, tensorType.getElementType());
  } else {
    return std::make_shared<UnknownSV>(type);
  }
}

std::shared_ptr<ScalarSV> SymbolicExecutionEngine::createArithExpr(
    StringRef opName, std::shared_ptr<ScalarSV> lhs,
    std::shared_ptr<ScalarSV> rhs, Type resultType) {
  if (opName == "arith.addi" || opName == "arith.addf") {
    return std::make_shared<AddExprSV>(lhs, rhs, resultType);
  } else if (opName == "arith.subi" || opName == "arith.subf") {
    return std::make_shared<SubExprSV>(lhs, rhs, resultType);
  } else if (opName == "arith.muli" || opName == "arith.mulf") {
    return std::make_shared<MulExprSV>(lhs, rhs, resultType);
  } else if (opName == "arith.divsi" || opName == "arith.divui" ||
             opName == "arith.divf") {
    return std::make_shared<DivExprSV>(lhs, rhs, resultType);
  } else if (opName == "arith.remsi" || opName == "arith.remui") {
    return std::make_shared<RemExprSV>(lhs, rhs, resultType);
  }
  // 默认返回 UnknownSV
  return std::make_shared<UnknownSV>(resultType);
}

CmpExprSV::Pred SymbolicExecutionEngine::getCmpIPred(arith::CmpIPredicate pred) {
  switch (pred) {
    case arith::CmpIPredicate::eq: return CmpExprSV::Pred::EQ;
    case arith::CmpIPredicate::ne: return CmpExprSV::Pred::NE;
    case arith::CmpIPredicate::slt:
    case arith::CmpIPredicate::ult: return CmpExprSV::Pred::LT;
    case arith::CmpIPredicate::sle:
    case arith::CmpIPredicate::ule: return CmpExprSV::Pred::LE;
    case arith::CmpIPredicate::sgt:
    case arith::CmpIPredicate::ugt: return CmpExprSV::Pred::GT;
    case arith::CmpIPredicate::sge:
    case arith::CmpIPredicate::uge: return CmpExprSV::Pred::GE;
  }
  return CmpExprSV::Pred::EQ;
}
