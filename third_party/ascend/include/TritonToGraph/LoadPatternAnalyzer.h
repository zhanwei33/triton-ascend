/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */

#ifndef TRITON_TO_GRAPH_LOAD_PATTERN_ANALYZER_H
#define TRITON_TO_GRAPH_LOAD_PATTERN_ANALYZER_H

#include "TritonToGraph/SymValue.h"
#include "TritonToGraph/SymbolicExecution.h"
#include "triton/Dialect/Triton/IR/Dialect.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/raw_ostream.h"

namespace mlir {
namespace triton {
namespace ascend {

//===----------------------------------------------------------------------===//
// Tensor访问信息 - 分析结果结构体
//===----------------------------------------------------------------------===//

struct TensorAccessInfo {
  //===---------------------------------------------------------------------===
  // 基础信息
  //===---------------------------------------------------------------------===
  Value basePtr;           // 原始base指针
  Type basePtrType;        // base指针类型
  SymValue* baseOffset;    // base地址的offset（行/列base地址）

  //===---------------------------------------------------------------------===
  // Shape信息
  //===---------------------------------------------------------------------===
  SmallVector<int64_t> shape;      // 访问的tensor形状
  SmallVector<int64_t> strides;    // 各维度stride（元素个数）
  Type elementType;                // 元素数据类型

  //===---------------------------------------------------------------------===
  // 连续性分析
  //===---------------------------------------------------------------------===
  bool isRowContiguous;    // 是否行连续
  bool isColContiguous;    // 是否列连续
  int contiguousAxis;      // 在哪个轴上连续（-1表示都不连续）

  //===---------------------------------------------------------------------===
  // 边界信息（用于识别min截断）
  //===---------------------------------------------------------------------===
  bool hasLengthCheck;     // 是否有长度检测（min模式）
  int64_t lengthBound;     // 长度边界值
  SymValue* rangeValue;    // range值（用于长度检测）

  //===---------------------------------------------------------------------===
  // Mask信息
  //===---------------------------------------------------------------------===
  bool hasMask;            // 是否有mask
  Value maskValue;         // mask值
  SymValue* maskSymValue;  // mask的符号值

  //===---------------------------------------------------------------------===
  // Padding信息
  //===---------------------------------------------------------------------===
  bool hasPadding;         // 是否有padding值
  SymValue* paddingValue;  // padding的符号值

  //===---------------------------------------------------------------------===
  // 访问模式分类
  //===---------------------------------------------------------------------===
  enum class AccessPattern {
    Unknown,           // 未知
    ScalarSequential,  // 标量顺序访问（如varlen序列长度）
    TensorContiguous,  // Tensor连续访问
    TensorStrided,     // Tensor strided访问
    GatherContiguous,  // Gather连续（128元素gather）
    GatherStrided,     // Gather strided
    LoopDependent,     // 循环依赖的访问（如K在for内）
  };
  AccessPattern pattern;

  //===---------------------------------------------------------------------===
  // 构造函数
  //===---------------------------------------------------------------------===
  TensorAccessInfo()
      : baseOffset(nullptr),
        basePtrType(nullptr),
        elementType(nullptr),
        isRowContiguous(false),
        isColContiguous(false),
        contiguousAxis(-1),
        hasLengthCheck(false),
        lengthBound(0),
        rangeValue(nullptr),
        hasMask(false),
        maskSymValue(nullptr),
        hasPadding(false),
        paddingValue(nullptr),
        pattern(AccessPattern::Unknown) {}

  //===---------------------------------------------------------------------===
  // 方法
  //===---------------------------------------------------------------------===

  /// 计算总元素数
  int64_t getNumElements() const;

  /// 计算总字节数
  int64_t getTotalBytes() const;

  /// 获取维度数
  size_t getRank() const { return shape.size(); }

  /// 打印分析结果
  void print(llvm::raw_ostream& os) const;

  /// 输出为YAML格式
  void printYAML(llvm::raw_ostream& os) const;

  /// 是否为标量访问
  bool isScalarAccess() const { return shape.empty(); }

  /// 是否为2D访问
  bool is2DAccess() const { return shape.size() == 2; }

  /// 获取内存布局描述
  StringRef getLayoutDescription() const;
};

//===----------------------------------------------------------------------===//
// Load模式分析器
//===----------------------------------------------------------------------===//

class LoadPatternAnalyzer {
public:
  LoadPatternAnalyzer() = default;

  //===---------------------------------------------------------------------===
  // 主要分析接口
  //===---------------------------------------------------------------------===

  /// 分析load指令的访问模式
  /// @param loadOp 要分析的load操作
  /// @param state 符号执行状态（包含所有SymValue）
  /// @return TensorAccessInfo 完整的访问信息
  TensorAccessInfo analyzeLoad(tt::LoadOp loadOp,
                               const SymbolicExecutionState& state);

  /// 分析标量load（用于varlen序列长度加载）
  TensorAccessInfo analyzeScalarLoad(tt::LoadOp loadOp,
                                     SymValue* ptrSym);

  /// 分析tensor load
  TensorAccessInfo analyzeTensorLoad(tt::LoadOp loadOp,
                                     PtrTensorSV* ptrTensor);

private:
  //===---------------------------------------------------------------------===
  // Ptr分析
  //===---------------------------------------------------------------------===

  /// 从PtrTensorSV提取结构化信息
  TensorAccessInfo extractFromPtrTensor(PtrTensorSV* ptrTensor);

  /// 从PtrBaseSV提取标量访问信息
  TensorAccessInfo extractFromPtrBase(PtrBaseSV* ptrBase);

  //===---------------------------------------------------------------------===
  // Stride推导
  //===---------------------------------------------------------------------===

  /// 基于offsets推导stride信息
  SmallVector<int64_t> inferStridesFromOffsets(
      PtrTensorSV* ptrTensor, ArrayRef<int64_t> shape);

  /// 分析offset表达式推导stride
  std::optional<int64_t> extractStrideFromOffset(SymValue* offset);

  //===---------------------------------------------------------------------===
  // 连续性分析
  //===---------------------------------------------------------------------===

  /// 分析访问的连续性
  void analyzeContiguity(TensorAccessInfo& info, PtrTensorSV* ptrTensor);

  /// 检查是否行连续（最后一个维度连续）
  bool isRowMajorContiguous(ArrayRef<int64_t> shape,
                            ArrayRef<int64_t> strides);

  /// 检查是否列连续（第一个维度连续）
  bool isColMajorContiguous(ArrayRef<int64_t> shape,
                            ArrayRef<int64_t> strides);

  //===---------------------------------------------------------------------===
  // Min截断识别（关键功能）
  //===---------------------------------------------------------------------===

  /// 识别min截断模式
  /// 模式: select(cmp_lt(idx, bound), idx, bound)
  bool detectMinTruncation(PtrTensorSV* ptrTensor,
                          TensorAccessInfo& info);

  /// 在offset表达式中查找min模式
  bool findMinPatternInOffset(SymValue* offset,
                              int64_t& bound,
                              SymValue*& range);

  /// 识别arith.select中的min模式
  bool isMinSelectPattern(ScalarExprSV* selectExpr,
                         int64_t& bound,
                         SymValue*& range);

  /// 识别TensorExpr中的min模式（用于tensor级select）
  bool isMinTensorPattern(TensorExprSV* tensorExpr,
                         int64_t& bound,
                         SymValue*& range);

  //===---------------------------------------------------------------------===
  // Mask/Padding分析
  //===---------------------------------------------------------------------===

  /// 分析load的mask
  void analyzeMask(tt::LoadOp loadOp,
                   TensorAccessInfo& info,
                   const SymbolicExecutionState& state);

  /// 分析load的padding值
  void analyzePadding(tt::LoadOp loadOp,
                      TensorAccessInfo& info,
                      const SymbolicExecutionState& state);

  /// 从mask推导边界信息
  bool extractBoundFromMask(SymValue* maskSym,
                           int64_t& bound,
                           SymValue*& range);

  //===---------------------------------------------------------------------===
  // 循环上下文分析
  //===---------------------------------------------------------------------===

  /// 检查load是否在循环内，且依赖循环变量
  bool isLoopDependent(tt::LoadOp loadOp,
                       const SymbolicExecutionState& state);

  /// 获取循环依赖信息
  void analyzeLoopDependency(tt::LoadOp loadOp,
                             TensorAccessInfo& info,
                             const SymbolicExecutionState& state);

  //===---------------------------------------------------------------------===
  // 辅助方法
  //===---------------------------------------------------------------------===

  /// 获取元素的类型大小（字节）
  int64_t getElementTypeSize(Type type) const;

  /// 计算线性索引
  int64_t computeLinearIndex(ArrayRef<int64_t> indices,
                            ArrayRef<int64_t> strides) const;

  /// 解析offset表达式为线性形式: base + sum(dim_i * stride_i)
  bool decomposeOffset(SymValue* offset,
                      SmallVector<std::pair<SymValue*, int64_t>>& terms);
};

} // namespace ascend
} // namespace triton
} // namespace mlir

#endif // TRITON_TO_GRAPH_LOAD_PATTERN_ANALYZER_H
