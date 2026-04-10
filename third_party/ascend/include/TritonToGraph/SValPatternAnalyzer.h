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

  // 主入口：分析 SymValue（支持 TensorSV 和 PtrExprSV）
  std::optional<TensorPattern> analyze(std::shared_ptr<SymValue> sv);

private:
  //===--------------------------------------------------------------------===//
  // 规范化阶段
  //===--------------------------------------------------------------------===//

  // 1. 传播 dims 到 RangeExprSV（DFS遍历）
  // 如果子项的 dims[0] != -1，将其传播给子项下的 RangeExprSV
  void propagateDimsToRange(ScalarSV* sv, ArrayRef<int> parentDims);

  // 2. 提升包含 RangeExprSV 的 SelectExprSV
  // 如果 SelectExpr 的 true/false 分支包含 RangeExprSV，
  // 用该分支替换 SelectExpr 在父表达式中的位置
  std::shared_ptr<ScalarSV> hoistSelectWithRange(std::shared_ptr<ScalarSV> sv);

  // 3. 展开分配律: (a+b)*c -> a*c + b*c，深度最多2层（非递归：收集+重建）
  // 将包含 RangeExprSV 的项展开到最外层
  std::shared_ptr<ScalarSV> expandDistribution(std::shared_ptr<ScalarSV> sv, int depth = 0);

  // 4. 收集所有加法项（展开后的表达式是加法链）
  void collectAddTerms(ScalarSV* sv, SmallVector<ScalarSV*>& terms);

  // 5. 检查是否包含 RangeExprSV
  bool containsRangeExpr(ScalarSV* sv);

  // 6. 把 RangeExprSV 交换到乘法最左面 (a*b, b是Range -> b*a)
  // 原地修改，不重建节点
  void bringRangeToLeft(ScalarSV* sv);

  // 7. 提取 stride 乘数（RangeExprSV 外的乘数部分）
  // 例如：Range[0,128)*k -> 返回 k
  ScalarSV* extractStrideMultiplier(ScalarSV* sv);

  // 8. 获取 RangeExprSV 所在的维度 (dims[0])
  int getRangeDim(ScalarSV* sv);

  // 9. 归一化项：分类为 basePtr, offsetTerms, strideTerms，并排序
  NormalizedTerms normalizeTerms(ScalarSV* sv);

  // 10. 合并加法项为一个表达式
  std::shared_ptr<ScalarSV> mergeAddTerms(ArrayRef<std::shared_ptr<ScalarSV>> terms);

  //===--------------------------------------------------------------------===//
  // 分类处理
  //===--------------------------------------------------------------------===//

  // 分析 Matrix (2D Tensor)
  std::optional<TensorPattern> analyzeMatrix(
      ArrayRef<int64_t> shape, Type elemType,
      NormalizedTerms& terms, Operation* op);

  // 分析 Vector (1D Tensor)
  std::optional<TensorPattern> analyzeVector(
      ArrayRef<int64_t> shape, Type elemType,
      NormalizedTerms& terms, Operation* op);

  // 分析 Scalar (0D)
  std::optional<TensorPattern> analyzeScalar(
      ArrayRef<int64_t> shape, Type elemType,
      NormalizedTerms& terms, Operation* op);

  // 分析 PtrExprSV（标量指针，作为 0D 处理）
  std::optional<TensorPattern> analyzePtrExpr(
      std::shared_ptr<PtrExprSV> ptrExpr);
};

} // namespace ascend
} // namespace triton
} // namespace mlir

#endif // TRITON_TO_GRAPH_SVAL_PATTERN_ANALYZER_H
