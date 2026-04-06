/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */

#include "TritonToGraph/LoadPatternAnalyzer.h"
#include "llvm/Support/Debug.h"
#include <optional>
#include "llvm/Support/Format.h"

#define DEBUG_TYPE "load-pattern-analyzer"

using namespace mlir;
using namespace mlir::triton;
using namespace mlir::triton::ascend;

//===----------------------------------------------------------------------===//
// TensorAccessInfo 方法实现
//===----------------------------------------------------------------------===//

int64_t TensorAccessInfo::getNumElements() const {
  if (shape.empty()) return 1;
  int64_t n = 1;
  for (auto s : shape) n *= s;
  return n;
}

int64_t TensorAccessInfo::getTotalBytes() const {
  int64_t elemSize = 0;
  if (elementType) {
    if (auto intType = dyn_cast<IntegerType>(elementType)) {
      elemSize = intType.getWidth() / 8;
    } else if (auto floatType = dyn_cast<FloatType>(elementType)) {
      elemSize = floatType.getWidth() / 8;
    }
  }
  return getNumElements() * elemSize;
}

void TensorAccessInfo::print(llvm::raw_ostream& os) const {
  os << "=== Tensor Access Info ===\n";
  os << "Base Pointer: " << basePtr << "\n";
  os << "Access Shape: [";
  for (size_t i = 0; i < shape.size(); ++i) {
    if (i > 0) os << "x";
    os << shape[i];
  }
  os << "]\n";

  os << "Strides: [";
  for (size_t i = 0; i < strides.size(); ++i) {
    if (i > 0) os << ", ";
    os << strides[i];
  }
  os << "]\n";

  os << "Element Type: ";
  if (elementType) {
    elementType.print(os);
  } else {
    os << "unknown";
  }
  os << "\n";

  os << "Contiguous Axis: " << contiguousAxis << "\n";
  os << "Row Contiguous: " << (isRowContiguous ? "yes" : "no") << "\n";
  os << "Col Contiguous: " << (isColContiguous ? "yes" : "no") << "\n";

  if (hasLengthCheck) {
    os << "Length Check: yes (bound=" << lengthBound << ")\n";
  }

  if (hasMask) {
    os << "Has Mask: yes\n";
  }

  if (hasPadding) {
    os << "Has Padding: yes\n";
  }

  os << "Access Pattern: ";
  switch (pattern) {
    case AccessPattern::Unknown: os << "Unknown"; break;
    case AccessPattern::ScalarSequential: os << "ScalarSequential"; break;
    case AccessPattern::TensorContiguous: os << "TensorContiguous"; break;
    case AccessPattern::TensorStrided: os << "TensorStrided"; break;
    case AccessPattern::GatherContiguous: os << "GatherContiguous"; break;
    case AccessPattern::GatherStrided: os << "GatherStrided"; break;
    case AccessPattern::LoopDependent: os << "LoopDependent"; break;
  }
  os << "\n";
}

void TensorAccessInfo::printYAML(llvm::raw_ostream& os) const {
  os << "PtrInfo:\n";
  os << "  base_tensor: " << basePtr << "\n";
  os << "  base_shape: [";
  for (size_t i = 0; i < shape.size(); ++i) {
    if (i > 0) os << ", ";
    os << shape[i];
  }
  os << "]\n";

  if (baseOffset) {
    os << "  base_offset: ";
    baseOffset->print(os);
    os << "\n";
  }

  os << "  strides: [";
  for (size_t i = 0; i < strides.size(); ++i) {
    if (i > 0) os << ", ";
    os << strides[i];
  }
  os << "]\n";

  os << "  element_type: ";
  if (elementType) {
    elementType.print(os);
  }
  os << "\n";

  os << "  contiguous_axis: " << contiguousAxis << "\n";

  if (hasLengthCheck) {
    os << "  length_check:\n";
    os << "    bound: " << lengthBound << "\n";
    os << "    type: min_truncation\n";
  }

  if (hasMask) {
    os << "  mask: yes\n";
  }

  if (hasPadding) {
    os << "  padding: ";
    if (paddingValue) {
      paddingValue->print(os);
    }
    os << "\n";
  }

  os << "  pattern: ";
  switch (pattern) {
    case AccessPattern::Unknown: os << "Unknown"; break;
    case AccessPattern::ScalarSequential: os << "ScalarSequential"; break;
    case AccessPattern::TensorContiguous: os << "TensorContiguous"; break;
    case AccessPattern::TensorStrided: os << "TensorStrided"; break;
    case AccessPattern::GatherContiguous: os << "GatherContiguous"; break;
    case AccessPattern::GatherStrided: os << "GatherStrided"; break;
    case AccessPattern::LoopDependent: os << "LoopDependent"; break;
  }
  os << "\n";
}

StringRef TensorAccessInfo::getLayoutDescription() const {
  if (isRowContiguous) return "row_major_contiguous";
  if (isColContiguous) return "col_major_contiguous";
  if (contiguousAxis >= 0) return "partially_contiguous";
  return "strided";
}

//===----------------------------------------------------------------------===//
// LoadPatternAnalyzer 主分析接口
//===----------------------------------------------------------------------===//

TensorAccessInfo LoadPatternAnalyzer::analyzeLoad(
    tt::LoadOp loadOp, const SymbolicExecutionState& state) {

  TensorAccessInfo info;

  // 获取load的ptr
  Value ptr = loadOp.getPtr();
  info.basePtr = ptr;
  info.basePtrType = ptr.getType();

  // 查找ptr的SymValue
  SymValue* ptrSym = state.getSymValue(ptr);

  if (!ptrSym) {
    LLVM_DEBUG(llvm::dbgs() << "No symbolic value for load ptr\n");
    return info;
  }

  // 根据ptr类型分发
  if (auto ptrBase = dyn_cast<PtrBaseSV>(ptrSym)) {
    // 标量指针
    info = extractFromPtrBase(ptrBase);
    info.pattern = TensorAccessInfo::AccessPattern::ScalarSequential;
  } else if (auto ptrTensor = dyn_cast<PtrTensorSV>(ptrSym)) {
    // Tensor指针
    info = extractFromPtrTensor(ptrTensor);

    // 分析连续性
    analyzeContiguity(info, ptrTensor);

    // 识别min截断模式
    detectMinTruncation(ptrTensor, info);

    // 确定访问模式
    if (info.contiguousAxis >= 0) {
      if (info.shape.size() == 1) {
        info.pattern = TensorAccessInfo::AccessPattern::GatherContiguous;
      } else {
        info.pattern = TensorAccessInfo::AccessPattern::TensorContiguous;
      }
    } else {
      info.pattern = TensorAccessInfo::AccessPattern::TensorStrided;
    }

    // 检查循环依赖
    if (isLoopDependent(loadOp, state)) {
      info.pattern = TensorAccessInfo::AccessPattern::LoopDependent;
    }
  }

  // 分析mask
  analyzeMask(loadOp, info, state);

  // 分析padding
  analyzePadding(loadOp, info, state);

  return info;
}

//===----------------------------------------------------------------------===//
// Ptr分析
//===----------------------------------------------------------------------===//

TensorAccessInfo LoadPatternAnalyzer::extractFromPtrTensor(
    PtrTensorSV* ptrTensor) {
  TensorAccessInfo info;

  info.basePtr = ptrTensor->getBasePtr();
  info.shape = SmallVector<int64_t>(ptrTensor->getShape());
  info.elementType = ptrTensor->getPointeeType();

  // 推导strides
  info.strides = inferStridesFromOffsets(ptrTensor, info.shape);

  // 获取第一个元素的offset作为baseOffset
  if (!info.shape.empty()) {
    SmallVector<int64_t> firstIdx(info.shape.size(), 0);
    info.baseOffset = ptrTensor->getElementOffset(firstIdx);
  }

  return info;
}

TensorAccessInfo LoadPatternAnalyzer::extractFromPtrBase(
    PtrBaseSV* ptrBase) {
  TensorAccessInfo info;

  info.basePtr = ptrBase->getBasePtr();
  info.baseOffset = ptrBase->getOffset();
  info.elementType = ptrBase->getPointeeType();

  // 标量访问没有shape
  info.shape.clear();
  info.strides.clear();

  return info;
}

//===----------------------------------------------------------------------===//
// Stride推导
//===----------------------------------------------------------------------===//

SmallVector<int64_t> LoadPatternAnalyzer::inferStridesFromOffsets(
    PtrTensorSV* ptrTensor, ArrayRef<int64_t> shape) {

  SmallVector<int64_t> strides(shape.size(), 0);

  if (shape.empty()) return strides;

  // 默认row-major strides
  strides.back() = 1;
  for (int i = shape.size() - 2; i >= 0; --i) {
    strides[i] = strides[i + 1] * shape[i + 1];
  }

  // 尝试从offsets验证/修正strides
  // 对于2D tensor，比较不同行的offset差
  if (shape.size() == 2) {
    SmallVector<int64_t> idx0 = {0, 0};
    SmallVector<int64_t> idx1 = {1, 0};

    SymValue* offset0 = ptrTensor->getElementOffset(idx0);
    SymValue* offset1 = ptrTensor->getElementOffset(idx1);

    // 尝试从offset表达式提取row stride
    if (offset0 && offset1) {
      // 简化：如果两个offset都是表达式，检查差值
      // 实际实现需要更复杂的表达式分析
      auto strideOpt = extractStrideFromOffset(offset1);
      if (strideOpt) {
        strides[0] = *strideOpt;
      }
    }
  }

  return strides;
}

std::optional<int64_t> LoadPatternAnalyzer::extractStrideFromOffset(
    SymValue* offset) {
  // 尝试从offset表达式中提取stride
  // 例如：offset = pid * stride + arange(0, len)
  // stride可能是乘法中的一个因子

  auto expr = dyn_cast<ScalarExprSV>(offset);
  if (!expr) return std::nullopt;

  // 检查是否为乘法表达式
  if (expr->getOp() == ScalarExprSV::OpKind::Mul) {
    // 如果一边是常量，另一边可能是索引
    if (auto constLHS = dyn_cast<ScalarConstantIntSV>(expr->getLHS())) {
      return constLHS->getInt();
    }
    if (auto constRHS = dyn_cast<ScalarConstantIntSV>(expr->getRHS())) {
      return constRHS->getInt();
    }
  }

  return std::nullopt;
}

//===----------------------------------------------------------------------===//
// 连续性分析
//===----------------------------------------------------------------------===//

void LoadPatternAnalyzer::analyzeContiguity(
    TensorAccessInfo& info, PtrTensorSV* ptrTensor) {

  if (info.shape.empty() || info.strides.empty()) {
    info.contiguousAxis = -1;
    return;
  }

  // 检查row-major连续性（最后一个维度连续）
  info.isRowContiguous = isRowMajorContiguous(info.shape, info.strides);

  // 检查col-major连续性（第一个维度连续）
  info.isColContiguous = isColMajorContiguous(info.shape, info.strides);

  // 确定连续轴
  if (info.isRowContiguous) {
    info.contiguousAxis = info.shape.size() - 1;
  } else if (info.isColContiguous) {
    info.contiguousAxis = 0;
  } else {
    // 检查部分连续性
    for (int i = info.shape.size() - 1; i >= 0; --i) {
      if (info.strides[i] == 1) {
        info.contiguousAxis = i;
        break;
      }
    }
  }
}

bool LoadPatternAnalyzer::isRowMajorContiguous(
    ArrayRef<int64_t> shape, ArrayRef<int64_t> strides) {

  if (shape.size() != strides.size()) return false;

  int64_t expectedStride = 1;
  for (int i = shape.size() - 1; i >= 0; --i) {
    if (strides[i] != expectedStride) {
      return false;
    }
    expectedStride *= shape[i];
  }
  return true;
}

bool LoadPatternAnalyzer::isColMajorContiguous(
    ArrayRef<int64_t> shape, ArrayRef<int64_t> strides) {

  if (shape.size() != strides.size()) return false;

  int64_t expectedStride = 1;
  for (size_t i = 0; i < shape.size(); ++i) {
    if (strides[i] != expectedStride) {
      return false;
    }
    expectedStride *= shape[i];
  }
  return true;
}

//===----------------------------------------------------------------------===//
// Min截断识别（关键功能）
//===----------------------------------------------------------------------===//

bool LoadPatternAnalyzer::detectMinTruncation(
    PtrTensorSV* ptrTensor, TensorAccessInfo& info) {

  // 遍历所有元素的offset，查找min模式
  const auto& offsets = ptrTensor->getAllOffsets();

  for (const auto& pair : offsets) {
    SymValue* offset = pair.second;

    int64_t bound;
    SymValue* range;

    if (findMinPatternInOffset(offset, bound, range)) {
      info.hasLengthCheck = true;
      info.lengthBound = bound;
      info.rangeValue = range;
      return true;
    }
  }

  return false;
}

bool LoadPatternAnalyzer::findMinPatternInOffset(
    SymValue* offset, int64_t& bound, SymValue*& range) {

  // 检查offset是否为select表达式（min模式）
  if (auto selectExpr = dyn_cast<ScalarExprSV>(offset)) {
    if (isMinSelectPattern(selectExpr, bound, range)) {
      return true;
    }
  }

  // 检查是否为tensor表达式（tensor级select）
  if (auto tensorExpr = dyn_cast<TensorExprSV>(offset)) {
    if (isMinTensorPattern(tensorExpr, bound, range)) {
      return true;
    }
  }

  // 递归检查表达式中的子表达式
  if (auto expr = dyn_cast<ScalarExprSV>(offset)) {
    // 检查LHS
    if (findMinPatternInOffset(expr->getLHS(), bound, range)) {
      return true;
    }
    // 检查RHS
    if (findMinPatternInOffset(expr->getRHS(), bound, range)) {
      return true;
    }
  }

  return false;
}

bool LoadPatternAnalyzer::isMinSelectPattern(
    ScalarExprSV* selectExpr, int64_t& bound, SymValue*& range) {

  // min(a, b) = select(a < b, a, b)
  if (selectExpr->getOp() != ScalarExprSV::OpKind::Select) {
    return false;
  }

  auto cond = selectExpr->getCondition();
  auto trueVal = selectExpr->getLHS();  // Select的true value
  auto falseVal = selectExpr->getRHS(); // Select的false value

  // 检查条件是否为比较
  auto cmpExpr = dyn_cast<ScalarExprSV>(cond);
  if (!cmpExpr) return false;

  // 检查是否为 "idx < bound" 模式
  if (cmpExpr->getOp() == ScalarExprSV::OpKind::CmpLT) {
    // 检查结构: select(idx < bound, idx, bound)
    if (cmpExpr->getLHS() == trueVal) {
      // 检查bound是否为常量
      if (auto boundConst = dyn_cast<ScalarConstantIntSV>(falseVal)) {
        bound = boundConst->getInt();
        range = trueVal;
        return true;
      }
      // 或者检查条件中的RHS
      if (auto boundConst = dyn_cast<ScalarConstantIntSV>(cmpExpr->getRHS())) {
        bound = boundConst->getInt();
        range = trueVal;
        return true;
      }
    }
  }

  return false;
}

bool LoadPatternAnalyzer::isMinTensorPattern(
    TensorExprSV* tensorExpr, int64_t& bound, SymValue*& range) {

  // 检查是否为tensor级的select
  if (tensorExpr->getOp() != TensorExprSV::OpKind::Select) {
    return false;
  }

  // 使用TensorExprSV内置的方法检查
  SymValue* r = nullptr;
  int64_t b = 0;
  if (tensorExpr->isLengthCheckPattern(r, b)) {
    bound = b;
    range = r;
    return true;
  }

  return false;
}

//===----------------------------------------------------------------------===//
// Mask/Padding分析
//===----------------------------------------------------------------------===//

void LoadPatternAnalyzer::analyzeMask(
    tt::LoadOp loadOp, TensorAccessInfo& info,
    const SymbolicExecutionState& state) {

  // 检查load是否有mask操作数
  // tt.load的mask通常是可选的最后一个操作数
  // 注意：这里需要根据实际的tt.load定义调整

  if (loadOp->getNumOperands() >= 2) {
    // 假设倒数第二个是mask，最后一个是padding
    Value mask = loadOp->getOperand(loadOp->getNumOperands() - 2);

    // 检查mask是否真的是mask（i1类型或tensor<i1>）
    Type maskType = mask.getType();
    bool isMaskType = false;

    if (auto tensorType = dyn_cast<RankedTensorType>(maskType)) {
      if (auto intType = dyn_cast<IntegerType>(tensorType.getElementType())) {
        if (intType.getWidth() == 1) {
          isMaskType = true;
        }
      }
    } else if (auto intType = dyn_cast<IntegerType>(maskType)) {
      if (intType.getWidth() == 1) {
        isMaskType = true;
      }
    }

    if (isMaskType) {
      info.hasMask = true;
      info.maskValue = mask;
      info.maskSymValue = state.getSymValue(mask);

      // 尝试从mask提取边界信息
      if (info.maskSymValue) {
        extractBoundFromMask(info.maskSymValue, info.lengthBound, info.rangeValue);
      }
    }
  }
}

void LoadPatternAnalyzer::analyzePadding(
    tt::LoadOp loadOp, TensorAccessInfo& info,
    const SymbolicExecutionState& state) {

  // tt.load的padding值通常是最后一个操作数
  if (loadOp->getNumOperands() >= 1) {
    Value padding = loadOp->getOperand(loadOp->getNumOperands() - 1);

    info.hasPadding = true;
    info.paddingValue = state.getSymValue(padding);
  }
}

bool LoadPatternAnalyzer::extractBoundFromMask(
    SymValue* maskSym, int64_t& bound, SymValue*& range) {

  // mask通常是 cmpi slt, idx, bound 的形式
  // 或者是 tensor级比较

  if (auto cmpExpr = dyn_cast<ScalarExprSV>(maskSym)) {
    if (cmpExpr->getOp() == ScalarExprSV::OpKind::CmpLT) {
      // idx < bound
      if (auto boundConst = dyn_cast<ScalarConstantIntSV>(cmpExpr->getRHS())) {
        bound = boundConst->getInt();
        range = cmpExpr->getLHS();
        return true;
      }
    }
  }

  // 对于tensor mask，尝试提取第一个元素的模式
  if (auto tensorExpr = dyn_cast<TensorExprSV>(maskSym)) {
    // 获取第一个元素
    SmallVector<int64_t> firstIdx(tensorExpr->getShape().size(), 0);
    SymValue* firstElem = tensorExpr->getElement(firstIdx);

    if (firstElem) {
      return extractBoundFromMask(firstElem, bound, range);
    }
  }

  return false;
}

//===----------------------------------------------------------------------===//
// 循环依赖分析
//===----------------------------------------------------------------------===//

bool LoadPatternAnalyzer::isLoopDependent(
    tt::LoadOp loadOp, const SymbolicExecutionState& state) {

  // 检查load是否在循环内
  if (!state.inLoop()) return false;

  // 获取load的ptr
  Value ptr = loadOp.getPtr();
  SymValue* ptrSym = state.getSymValue(ptr);

  if (!ptrSym) return false;

  // 检查ptr是否依赖于循环变量
  // 简化：检查是否在循环体内定义
  Operation* ptrDefOp = ptr.getDefiningOp();
  if (!ptrDefOp) return false;

  // 检查ptr的定义是否在循环内
  // 这需要遍历loopStack中的所有循环
  for (const auto& loopCtx : state.loopStack) {
    Region* loopRegion = &loopCtx.loopOp.getRegion();
    if (loopRegion->isAncestor(ptrDefOp->getParentRegion())) {
      return true;
    }
  }

  return false;
}

void LoadPatternAnalyzer::analyzeLoopDependency(
    tt::LoadOp loadOp, TensorAccessInfo& info,
    const SymbolicExecutionState& state) {

  if (!isLoopDependent(loadOp, state)) return;

  info.pattern = TensorAccessInfo::AccessPattern::LoopDependent;

  // 获取当前循环上下文
  const LoopContext& loopCtx = state.getCurrentLoop();

  // 分析load的ptr如何依赖于循环变量
  Value ptr = loadOp.getPtr();
  SymValue* ptrSym = state.getSymValue(ptr);

  if (auto ptrTensor = dyn_cast<PtrTensorSV>(ptrSym)) {
    // 分析offsets中哪些包含循环变量
    const auto& offsets = ptrTensor->getAllOffsets();

    for (const auto& pair : offsets) {
      SymValue* offset = pair.second;

      // 检查offset是否引用循环变量
      // 这需要递归检查表达式
      // 简化：检查是否为TensorRangeSV（循环变量的表示）
      if (auto range = dyn_cast<TensorRangeSV>(offset)) {
        // 可能是循环变量
        // 记录这种依赖关系
      }
    }
  }
}

//===----------------------------------------------------------------------===//
// 辅助方法
//===----------------------------------------------------------------------===//

int64_t LoadPatternAnalyzer::getElementTypeSize(Type type) const {
  if (auto intType = dyn_cast<IntegerType>(type)) {
    return intType.getWidth() / 8;
  }
  if (auto floatType = dyn_cast<FloatType>(type)) {
    return floatType.getWidth() / 8;
  }
  return 0;
}

int64_t LoadPatternAnalyzer::computeLinearIndex(
    ArrayRef<int64_t> indices, ArrayRef<int64_t> strides) const {

  assert(indices.size() == strides.size());

  int64_t linear = 0;
  for (size_t i = 0; i < indices.size(); ++i) {
    linear += indices[i] * strides[i];
  }
  return linear;
}

bool LoadPatternAnalyzer::decomposeOffset(
    SymValue* offset,
    SmallVector<std::pair<SymValue*, int64_t>>& terms) {

  // 尝试将offset分解为: base + sum(dim_i * stride_i)
  // 这需要递归遍历表达式树

  if (!offset) return false;

  if (auto expr = dyn_cast<ScalarExprSV>(offset)) {
    if (expr->getOp() == ScalarExprSV::OpKind::Add) {
      // 递归分解LHS和RHS
      decomposeOffset(expr->getLHS(), terms);
      decomposeOffset(expr->getRHS(), terms);
      return true;
    } else if (expr->getOp() == ScalarExprSV::OpKind::Mul) {
      // 检查是否为 dim * stride
      if (auto strideConst = dyn_cast<ScalarConstantIntSV>(expr->getRHS())) {
        terms.push_back({expr->getLHS(), strideConst->getInt()});
        return true;
      }
      if (auto strideConst = dyn_cast<ScalarConstantIntSV>(expr->getLHS())) {
        terms.push_back({expr->getRHS(), strideConst->getInt()});
        return true;
      }
    }
  }

  // 无法进一步分解，作为base项（stride=1）
  terms.push_back({offset, 1});
  return true;
}
