/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */

#ifndef TRITON_TO_GRAPH_SVAL_PATTERN_ANALYZER_H
#define TRITON_TO_GRAPH_SVAL_PATTERN_ANALYZER_H

#include "TritonToGraph/SymValue.h"
#include "llvm/ADT/SmallVector.h"
#include <memory>
#include <optional>

namespace mlir {
namespace triton {
namespace ascend {

//===----------------------------------------------------------------------===//
// 分析结果结构
//===----------------------------------------------------------------------===//

struct TensorPattern {
  // 分类
  enum class Kind { Scalar, Vector, Matrix };

  Kind kind;

  // Shape 信息
  SmallVector<int64_t> shape;
  Type elementType;

  // Base 信息
  std::shared_ptr<GmPtrSV> basePtr;           // GmPtr base
  std::shared_ptr<ScalarSV> baseOffset;       // 无 RangeExprSV 的项归拢结果

  // 各轴的 stride 信息（按 dims[0] 升序排列）
  SmallVector<std::shared_ptr<ScalarSV>> axisStrides;

  // 连续性信息：记录哪些轴是连续的
  SmallVector<bool> isContinuous;

  void print(llvm::raw_ostream& os) const;
};

//===----------------------------------------------------------------------===//
// 规范化后的项集合
//===----------------------------------------------------------------------===//

struct NormalizedTerms {
  std::shared_ptr<GmPtrSV> basePtr;                    // GmPtr
  SmallVector<std::shared_ptr<ScalarSV>> offsetTerms;  // 无 RangeExprSV
  SmallVector<std::shared_ptr<ScalarSV>> strideTerms;  // 有 RangeExprSV，按 dim 升序
};

//===----------------------------------------------------------------------===//
// SValPatternAnalyzer - SymValue 模式分析器
//===----------------------------------------------------------------------===//

class SValPatternAnalyzer {
public:
  SValPatternAnalyzer() = default;

  // 主入口：分析 SymValue（支持 ScalarSV 和 TensorSV）
  std::optional<TensorPattern> analyze(std::shared_ptr<SymValue> sv);

private:
  //===--------------------------------------------------------------------===//
  // 规范化阶段
  //===--------------------------------------------------------------------===//

  // 1. 传播 dims 到 RangeExprSV
  void propagateDimsToRange(std::shared_ptr<ScalarSV> sv, ArrayRef<int> parentDims);

  // 2. 展开分配律: (a+b)*c -> a*c + b*c，深度最多2层
  std::shared_ptr<ScalarSV> expandDistribution(std::shared_ptr<ScalarSV> sv, int depth = 0);

  // 3. 收集所有加法项
  void collectAddTerms(std::shared_ptr<ScalarSV> sv,
                       SmallVector<std::shared_ptr<ScalarSV>>& terms);

  // 4. 检查是否包含 RangeExprSV
  bool containsRangeExpr(ScalarSV* sv);

  // 5. 把 RangeExprSV 交换到最左面
  std::shared_ptr<ScalarSV> bringRangeToLeft(std::shared_ptr<ScalarSV> sv);

  // 6. 提取 stride 乘数（RangeExprSV 外的乘数）
  std::shared_ptr<ScalarSV> extractStrideMultiplier(std::shared_ptr<ScalarSV> sv);

  // 7. 归一化项
  NormalizedTerms normalizeTerms(std::shared_ptr<ScalarSV> sv);

  // 8. 合并加法项
  std::shared_ptr<ScalarSV> mergeAddTerms(ArrayRef<std::shared_ptr<ScalarSV>> terms);

  //===--------------------------------------------------------------------===//
  // 分类处理
  //===--------------------------------------------------------------------===//

  std::optional<TensorPattern> analyzeMatrix(std::shared_ptr<TensorSV> tensor,
                                              NormalizedTerms& terms);
  std::optional<TensorPattern> analyzeVector(std::shared_ptr<TensorSV> tensor,
                                              NormalizedTerms& terms);
  std::optional<TensorPattern> analyzeScalar(std::shared_ptr<TensorSV> tensor,
                                              NormalizedTerms& terms);

  // 分析 PtrExprSV（标量指针）
  std::optional<TensorPattern> analyzePtrExpr(std::shared_ptr<PtrExprSV> ptrExpr);
};

} // namespace ascend
} // namespace triton
} // namespace mlir

#endif // TRITON_TO_GRAPH_SVAL_PATTERN_ANALYZER_H
