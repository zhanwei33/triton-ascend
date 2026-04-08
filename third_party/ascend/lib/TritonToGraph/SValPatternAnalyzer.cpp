/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */

#include "TritonToGraph/SValPatternAnalyzer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/SetVector.h"

namespace mlir {
namespace triton {
namespace ascend {

//===----------------------------------------------------------------------===//
// TensorPattern 打印
//===----------------------------------------------------------------------===//

void TensorPattern::print(llvm::raw_ostream& os) const {
  os << "TensorPattern[";
  switch (kind) {
    case Kind::Scalar: os << "Scalar"; break;
    case Kind::Vector: os << "Vector"; break;
    case Kind::Matrix: os << "Matrix"; break;
  }
  os << "]\n";

  os << "  Shape: [";
  for (size_t i = 0; i < shape.size(); ++i) {
    if (i > 0) os << "x";
    os << shape[i];
  }
  os << "]\n";

  os << "  ElementType: " << elementType << "\n";

  if (basePtr) {
    os << "  BasePtr: ";
    basePtr->print(os);
    os << "\n";
  }

  if (baseOffset) {
    os << "  BaseOffset: ";
    baseOffset->print(os);
    os << "\n";
  }

  os << "  AxisStrides (" << axisStrides.size() << " axes):\n";
  for (size_t i = 0; i < axisStrides.size(); ++i) {
    os << "    Axis[" << i << "]: ";
    axisStrides[i]->print(os);
    os << "  " << (isContinuous[i] ? "[continuous]" : "[strided]") << "\n";
  }
}

//===----------------------------------------------------------------------===//
// 主入口
//===----------------------------------------------------------------------===//

std::optional<TensorPattern> SValPatternAnalyzer::analyze(
    std::shared_ptr<SymValue> sv) {
  if (!sv) return std::nullopt;

  // 处理 TensorSV
  if (auto* tensor = dyn_cast<TensorSV>(sv.get())) {
    auto tensorPtr = std::static_pointer_cast<TensorSV>(sv);

    // 获取 elementExpr
    if (!tensor->elementExpr) return std::nullopt;

    // 1. 传播 dims
    propagateDimsToRange(tensor->elementExpr, {});

    // 2. 展开分配律（最多2层）
    auto expanded = expandDistribution(tensor->elementExpr, 0);

    // 3. 归一化
    NormalizedTerms terms = normalizeTerms(expanded);

    // 4. 分类处理
    if (tensor->getShape().size() == 2) {
      return analyzeMatrix(tensorPtr, terms);
    } else if (tensor->getShape().size() == 1) {
      return analyzeVector(tensorPtr, terms);
    } else {
      return analyzeScalar(tensorPtr, terms);
    }
  }

  // 处理 PtrExprSV（标量指针）
  if (auto* ptrExpr = dyn_cast<PtrExprSV>(sv.get())) {
    return analyzePtrExpr(std::static_pointer_cast<PtrExprSV>(sv));
  }

  return std::nullopt;
}

//===----------------------------------------------------------------------===//
// 规范化阶段
//===----------------------------------------------------------------------===//

// 辅助递归函数（使用裸指针，避免 shared_ptr 问题）
static void propagateDimsToRangeImpl(ScalarSV* sv, ArrayRef<int> parentDims) {
  if (!sv) return;

  // 如果当前有 dims，更新 parentDims
  if (sv->dims.size() != 1 || sv->dims[0] != -1) {
    parentDims = sv->dims;
  }

  // 如果是 RangeExprSV，设置 dims
  if (auto* range = dyn_cast<RangeExprSV>(sv)) {
    if (!parentDims.empty() && parentDims[0] != -1) {
      range->setDims(parentDims);
    }
    return;
  }

  // 递归处理子表达式（裸指针调用）
  if (auto* add = dyn_cast<AddExprSV>(sv)) {
    propagateDimsToRangeImpl(add->getLHS(), parentDims);
    propagateDimsToRangeImpl(add->getRHS(), parentDims);
  } else if (auto* mul = dyn_cast<MulExprSV>(sv)) {
    propagateDimsToRangeImpl(mul->getLHS(), parentDims);
    propagateDimsToRangeImpl(mul->getRHS(), parentDims);
  } else if (auto* sub = dyn_cast<SubExprSV>(sv)) {
    propagateDimsToRangeImpl(sub->getLHS(), parentDims);
    propagateDimsToRangeImpl(sub->getRHS(), parentDims);
  } else if (auto* div = dyn_cast<DivExprSV>(sv)) {
    propagateDimsToRangeImpl(div->getLHS(), parentDims);
    propagateDimsToRangeImpl(div->getRHS(), parentDims);
  } else if (auto* rem = dyn_cast<RemExprSV>(sv)) {
    propagateDimsToRangeImpl(rem->getLHS(), parentDims);
    propagateDimsToRangeImpl(rem->getRHS(), parentDims);
  } else if (auto* ptrExpr = dyn_cast<PtrExprSV>(sv)) {
    propagateDimsToRangeImpl(ptrExpr->getBasePtr(), parentDims);
    propagateDimsToRangeImpl(ptrExpr->getOffset(), parentDims);
  }
}

void SValPatternAnalyzer::propagateDimsToRange(
    std::shared_ptr<ScalarSV> sv, ArrayRef<int> parentDims) {
  propagateDimsToRangeImpl(sv.get(), parentDims);
}

// 辅助函数：保存子节点 shared_ptr 的递归展开
struct ExpandContext {
  // 缓存已经处理过的节点
  DenseMap<ScalarSV*, std::shared_ptr<ScalarSV>> cache;
};

static std::shared_ptr<ScalarSV> expandDistributionRecursive(
    std::shared_ptr<ScalarSV> sv, int depth, ExpandContext& ctx);

static std::shared_ptr<ScalarSV> expandChild(
    ScalarSV* child, int depth, ExpandContext& ctx) {
  if (!child) return nullptr;

  // 查找缓存
  auto it = ctx.cache.find(child);
  if (it != ctx.cache.end()) {
    return it->second;
  }

  // 对于叶子节点，直接返回 nullptr 表示不需要展开
  if (isa<RangeExprSV>(child) ||
      isa<ScalarConstantIntSV>(child) ||
      isa<ScalarConstantFloatSV>(child) ||
      isa<ProgramIDSV>(child) ||
      isa<GmPtrSV>(child) ||
      isa<ArgSV>(child) ||
      isa<UnknownSV>(child)) {
    return nullptr;
  }

  // 对于中间节点，需要递归展开
  // 但我们需要获取 child 的 shared_ptr，这需要从父节点保存
  // 简化处理：返回 nullptr 表示使用原始节点
  return nullptr;
}

std::shared_ptr<ScalarSV> SValPatternAnalyzer::expandDistribution(
    std::shared_ptr<ScalarSV> sv, int depth) {
  if (!sv || depth >= 2) return sv;

  // 处理 MulExprSV: (a+b)*c -> a*c + b*c
  if (auto* mul = dyn_cast<MulExprSV>(sv.get())) {
    // 递归展开子表达式
    auto lhs = expandDistribution(
        std::const_pointer_cast<ScalarSV>(sv->getLHS()->shared_from_this()), depth + 1);
    auto rhs = expandDistribution(
        std::const_pointer_cast<ScalarSV>(sv->getRHS()->shared_from_this()), depth + 1);

    // (a+b)*c -> a*c + b*c
    if (auto* add = dyn_cast<AddExprSV>(lhs.get())) {
      auto ac = std::make_shared<MulExprSV>(
          std::const_pointer_cast<ScalarSV>(add->getLHS()->shared_from_this()),
          rhs, lhs->getDataType(), nullptr);
      auto bc = std::make_shared<MulExprSV>(
          std::const_pointer_cast<ScalarSV>(add->getRHS()->shared_from_this()),
          rhs, lhs->getDataType(), nullptr);
      auto result = std::make_shared<AddExprSV>(ac, bc, sv->getDataType(), nullptr);
      return expandDistribution(result, depth + 1);
    }

    // c*(a+b) -> c*a + c*b
    if (auto* add = dyn_cast<AddExprSV>(rhs.get())) {
      auto ca = std::make_shared<MulExprSV>(
          lhs,
          std::const_pointer_cast<ScalarSV>(add->getLHS()->shared_from_this()),
          rhs->getDataType(), nullptr);
      auto cb = std::make_shared<MulExprSV>(
          lhs,
          std::const_pointer_cast<ScalarSV>(add->getRHS()->shared_from_this()),
          rhs->getDataType(), nullptr);
      auto result = std::make_shared<AddExprSV>(ca, cb, sv->getDataType(), nullptr);
      return expandDistribution(result, depth + 1);
    }

    // 递归处理子项
    auto newLhs = expandDistribution(lhs, depth + 1);
    auto newRhs = expandDistribution(rhs, depth + 1);

    if (newLhs.get() != lhs.get() || newRhs.get() != rhs.get()) {
      return std::make_shared<MulExprSV>(newLhs, newRhs, sv->getDataType(), nullptr);
    }
    return sv;
  }

  // 递归处理其他表达式类型
  if (auto* add = dyn_cast<AddExprSV>(sv.get())) {
    auto lhs = expandDistribution(
        std::const_pointer_cast<ScalarSV>(add->getLHS()->shared_from_this()), depth);
    auto rhs = expandDistribution(
        std::const_pointer_cast<ScalarSV>(add->getRHS()->shared_from_this()), depth);
    if (lhs.get() != add->getLHS() || rhs.get() != add->getRHS()) {
      return std::make_shared<AddExprSV>(lhs, rhs, sv->getDataType(), nullptr);
    }
  } else if (auto* sub = dyn_cast<SubExprSV>(sv.get())) {
    auto lhs = expandDistribution(
        std::const_pointer_cast<ScalarSV>(sub->getLHS()->shared_from_this()), depth);
    auto rhs = expandDistribution(
        std::const_pointer_cast<ScalarSV>(sub->getRHS()->shared_from_this()), depth);
    if (lhs.get() != sub->getLHS() || rhs.get() != sub->getRHS()) {
      return std::make_shared<SubExprSV>(lhs, rhs, sv->getDataType(), nullptr);
    }
  } else if (auto* div = dyn_cast<DivExprSV>(sv.get())) {
    auto lhs = expandDistribution(
        std::const_pointer_cast<ScalarSV>(div->getLHS()->shared_from_this()), depth);
    auto rhs = expandDistribution(
        std::const_pointer_cast<ScalarSV>(div->getRHS()->shared_from_this()), depth);
    if (lhs.get() != div->getLHS() || rhs.get() != div->getRHS()) {
      return std::make_shared<DivExprSV>(lhs, rhs, sv->getDataType(), nullptr);
    }
  } else if (auto* rem = dyn_cast<RemExprSV>(sv.get())) {
    auto lhs = expandDistribution(
        std::const_pointer_cast<ScalarSV>(rem->getLHS()->shared_from_this()), depth);
    auto rhs = expandDistribution(
        std::const_pointer_cast<ScalarSV>(rem->getRHS()->shared_from_this()), depth);
    if (lhs.get() != rem->getLHS() || rhs.get() != rem->getRHS()) {
      return std::make_shared<RemExprSV>(lhs, rhs, sv->getDataType(), nullptr);
    }
  }

  return sv;
}

void SValPatternAnalyzer::collectAddTerms(
    std::shared_ptr<ScalarSV> sv,
    SmallVector<std::shared_ptr<ScalarSV>>& terms) {
  if (!sv) return;

  // 递归分解加法
  if (auto* add = dyn_cast<AddExprSV>(sv.get())) {
    collectAddTerms(
        std::const_pointer_cast<ScalarSV>(add->getLHS()->shared_from_this()), terms);
    collectAddTerms(
        std::const_pointer_cast<ScalarSV>(add->getRHS()->shared_from_this()), terms);
  } else {
    terms.push_back(sv);
  }
}

bool SValPatternAnalyzer::containsRangeExpr(ScalarSV* sv) {
  if (!sv) return false;

  if (isa<RangeExprSV>(sv)) return true;

  if (auto* add = dyn_cast<AddExprSV>(sv)) {
    return containsRangeExpr(add->getLHS()) || containsRangeExpr(add->getRHS());
  }
  if (auto* mul = dyn_cast<MulExprSV>(sv)) {
    return containsRangeExpr(mul->getLHS()) || containsRangeExpr(mul->getRHS());
  }
  if (auto* sub = dyn_cast<SubExprSV>(sv)) {
    return containsRangeExpr(sub->getLHS()) || containsRangeExpr(sub->getRHS());
  }
  if (auto* div = dyn_cast<DivExprSV>(sv)) {
    return containsRangeExpr(div->getLHS()) || containsRangeExpr(div->getRHS());
  }
  if (auto* rem = dyn_cast<RemExprSV>(sv)) {
    return containsRangeExpr(rem->getLHS()) || containsRangeExpr(rem->getRHS());
  }

  return false;
}

std::shared_ptr<ScalarSV> SValPatternAnalyzer::bringRangeToLeft(
    std::shared_ptr<ScalarSV> sv) {
  if (!sv) return sv;

  // 检查是否是乘法，且 RangeExprSV 在右边
  if (auto* mul = dyn_cast<MulExprSV>(sv.get())) {
    auto lhs = std::const_pointer_cast<ScalarSV>(mul->getLHS()->shared_from_this());
    auto rhs = std::const_pointer_cast<ScalarSV>(mul->getRHS()->shared_from_this());

    // 如果 rhs 是 RangeExprSV，交换
    if (isa<RangeExprSV>(rhs.get())) {
      return std::make_shared<MulExprSV>(rhs, lhs, sv->getDataType(), nullptr);
    }

    // 如果 lhs 已经是 RangeExprSV，保持不变
    if (isa<RangeExprSV>(lhs.get())) {
      return sv;
    }

    // 递归处理
    auto newLhs = bringRangeToLeft(lhs);
    auto newRhs = bringRangeToLeft(rhs);
    if (newLhs.get() != lhs.get() || newRhs.get() != rhs.get()) {
      return std::make_shared<MulExprSV>(newLhs, newRhs, sv->getDataType(), nullptr);
    }
  }

  return sv;
}

std::shared_ptr<ScalarSV> SValPatternAnalyzer::extractStrideMultiplier(
    std::shared_ptr<ScalarSV> sv) {
  if (!sv) return nullptr;

  // 如果是 RangeExprSV，返回常量 1
  if (isa<RangeExprSV>(sv.get())) {
    return std::make_shared<ScalarConstantIntSV>(1, sv->getDataType(), nullptr);
  }

  // 如果是乘法，提取非 RangeExprSV 的部分
  if (auto* mul = dyn_cast<MulExprSV>(sv.get())) {
    auto lhs = std::const_pointer_cast<ScalarSV>(mul->getLHS()->shared_from_this());
    auto rhs = std::const_pointer_cast<ScalarSV>(mul->getRHS()->shared_from_this());

    if (isa<RangeExprSV>(lhs.get())) {
      // lhs 是 range，rhs 是 multiplier
      return rhs;
    }
    if (isa<RangeExprSV>(rhs.get())) {
      // rhs 是 range，lhs 是 multiplier
      return lhs;
    }
  }

  // 其他情况返回原值
  return sv;
}

NormalizedTerms SValPatternAnalyzer::normalizeTerms(std::shared_ptr<ScalarSV> sv) {
  NormalizedTerms result;

  // 收集所有加法项
  SmallVector<std::shared_ptr<ScalarSV>> allTerms;
  collectAddTerms(sv, allTerms);

  // 分类
  for (auto& term : allTerms) {
    if (isa<GmPtrSV>(term.get())) {
      result.basePtr = std::static_pointer_cast<GmPtrSV>(term);
    } else if (containsRangeExpr(term.get())) {
      // 包含 RangeExprSV，归到 strideTerms，并把 Range 交换到左边
      auto normalized = bringRangeToLeft(term);
      result.strideTerms.push_back(normalized);
    } else {
      // 不包含 RangeExprSV，归到 offsetTerms
      result.offsetTerms.push_back(term);
    }
  }

  // 按 dims[0] 升序排列 strideTerms
  llvm::sort(result.strideTerms, [](const std::shared_ptr<ScalarSV>& a,
                                     const std::shared_ptr<ScalarSV>& b) {
    int dimA = (a->dims.empty() || a->dims[0] == -1) ? 999 : a->dims[0];
    int dimB = (b->dims.empty() || b->dims[0] == -1) ? 999 : b->dims[0];
    return dimA < dimB;
  });

  return result;
}

std::shared_ptr<ScalarSV> SValPatternAnalyzer::mergeAddTerms(
    ArrayRef<std::shared_ptr<ScalarSV>> terms) {
  if (terms.empty()) return nullptr;
  if (terms.size() == 1) return terms[0];

  // 合并为加法链
  auto result = terms[0];
  for (size_t i = 1; i < terms.size(); ++i) {
    result = std::make_shared<AddExprSV>(result, terms[i],
                                         terms[i]->getDataType(), nullptr);
  }
  return result;
}

//===----------------------------------------------------------------------===//
// 分类处理
//===----------------------------------------------------------------------===//

std::optional<TensorPattern> SValPatternAnalyzer::analyzeMatrix(
    std::shared_ptr<TensorSV> tensor, NormalizedTerms& terms) {
  TensorPattern pattern;
  pattern.kind = TensorPattern::Kind::Matrix;
  pattern.shape = tensor->getShape();
  pattern.elementType = tensor->getElementType();
  pattern.basePtr = terms.basePtr;

  // 合并 offsetTerms
  if (!terms.offsetTerms.empty()) {
    pattern.baseOffset = mergeAddTerms(terms.offsetTerms);
  }

  // 提取各轴的 stride（按 dims[0] 升序）
  for (auto& strideTerm : terms.strideTerms) {
    auto multiplier = extractStrideMultiplier(strideTerm);
    pattern.axisStrides.push_back(multiplier);

    // 连续性分析：如果 stride=1，则该轴连续
    bool continuous = false;
    if (auto* constInt = dyn_cast<ScalarConstantIntSV>(multiplier.get())) {
      continuous = (constInt->getInt() == 1);
    }
    pattern.isContinuous.push_back(continuous);
  }

  return pattern;
}

std::optional<TensorPattern> SValPatternAnalyzer::analyzeVector(
    std::shared_ptr<TensorSV> tensor, NormalizedTerms& terms) {
  TensorPattern pattern;
  pattern.kind = TensorPattern::Kind::Vector;
  pattern.shape = tensor->getShape();
  pattern.elementType = tensor->getElementType();
  pattern.basePtr = terms.basePtr;

  // 合并 offsetTerms
  if (!terms.offsetTerms.empty()) {
    pattern.baseOffset = mergeAddTerms(terms.offsetTerms);
  }

  // 提取 stride
  for (auto& strideTerm : terms.strideTerms) {
    auto multiplier = extractStrideMultiplier(strideTerm);
    pattern.axisStrides.push_back(multiplier);

    // 连续性分析
    bool continuous = false;
    if (auto* constInt = dyn_cast<ScalarConstantIntSV>(multiplier.get())) {
      continuous = (constInt->getInt() == 1);
    }
    pattern.isContinuous.push_back(continuous);
  }

  return pattern;
}

std::optional<TensorPattern> SValPatternAnalyzer::analyzeScalar(
    std::shared_ptr<TensorSV> tensor, NormalizedTerms& terms) {
  TensorPattern pattern;
  pattern.kind = TensorPattern::Kind::Scalar;
  pattern.shape = tensor->getShape();
  pattern.elementType = tensor->getElementType();
  pattern.basePtr = terms.basePtr;

  // 合并 offsetTerms
  if (!terms.offsetTerms.empty()) {
    pattern.baseOffset = mergeAddTerms(terms.offsetTerms);
  }

  // Scalar 通常没有 strideTerms
  return pattern;
}

std::optional<TensorPattern> SValPatternAnalyzer::analyzePtrExpr(
    std::shared_ptr<PtrExprSV> ptrExpr) {
  // 将 PtrExprSV 视为 0-D Tensor（标量指针）
  TensorPattern pattern;
  pattern.kind = TensorPattern::Kind::Scalar;
  pattern.elementType = ptrExpr->getPointeeType();

  // 分析 PtrExprSV 的 offset
  if (!ptrExpr->getBasePtr() || !ptrExpr->getOffset()) {
    return std::nullopt;
  }

  if (isa<GmPtrSV>(ptrExpr->getBasePtr())) {
    pattern.basePtr = std::static_pointer_cast<GmPtrSV>(
        std::const_pointer_cast<ScalarSV>(ptrExpr->getBasePtr()->shared_from_this()));
  }

  // 对 offset 进行归一化分析
  auto offset = std::const_pointer_cast<ScalarSV>(ptrExpr->getOffset()->shared_from_this());

  // 传播 dims
  propagateDimsToRange(offset, {});

  // 展开分配律
  auto expanded = expandDistribution(offset, 0);

  // 归一化
  NormalizedTerms terms = normalizeTerms(expanded);

  // 合并 offsetTerms 作为 baseOffset
  if (!terms.offsetTerms.empty()) {
    pattern.baseOffset = mergeAddTerms(terms.offsetTerms);
  }

  // strideTerms 作为 axisStrides（虽然 scalar 应该为空）
  for (auto& strideTerm : terms.strideTerms) {
    auto multiplier = extractStrideMultiplier(strideTerm);
    pattern.axisStrides.push_back(multiplier);
    pattern.isContinuous.push_back(false);
  }

  return pattern;
}

} // namespace ascend
} // namespace triton
} // namespace mlir
