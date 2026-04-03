/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */

#ifndef TRITON_TO_GRAPH_TENSOR_ANALYZER_H
#define TRITON_TO_GRAPH_TENSOR_ANALYZER_H

#include "TritonToGraph/ControlFlowGraph.h"
#include "TritonToGraph/DataflowGraph.h"
#include "TritonToGraph/GraphAnalysis.h"
#include "mlir/IR/Value.h"
#include "mlir/IR/Operation.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/DenseMap.h"

namespace mlir {
namespace triton {
namespace cfg {

//===----------------------------------------------------------------------===//
// TensorAnalyzer - Tensor 指令分析器
// 用于收集 load/store/dot 指令并进行程序切片分析
//
// 本类使用 GraphAnalysis 中的 ProgramSlicer 进行切片计算，
// 专注于 Tensor 相关的指令分析和指针结构解构
//===----------------------------------------------------------------------===//

class TensorAnalyzer {
public:
  TensorAnalyzer(ControlFlowGraph& cfg, DataFlowGraph& dfg)
      : cfg(cfg), dfg(dfg), slicer(dfg, cfg) {}

  //===----------------------------------------------------------------------===
  // 指令收集 API
  //===----------------------------------------------------------------------===

  /// 收集 CFG 中所有 load 指令
  /// 包括：tt.load, triton.load 等
  SmallVector<Instruction*> collectLoadInstructions() const;

  /// 收集 CFG 中所有 store 指令
  /// 包括：tt.store, triton.store 等
  SmallVector<Instruction*> collectStoreInstructions() const;

  /// 收集 CFG 中所有 dot 指令
  /// 包括：tt.dot, triton.dot 等
  SmallVector<Instruction*> collectDotInstructions() const;

  /// 同时收集所有三种指令
  struct TensorInstructions {
    SmallVector<Instruction*> loads;
    SmallVector<Instruction*> stores;
    SmallVector<Instruction*> dots;
  };
  TensorInstructions collectAllTensorInstructions() const;

  //===----------------------------------------------------------------------===
  // 程序切片 API
  // 复用 GraphAnalysis::ProgramSlicer 进行切片计算
  //===----------------------------------------------------------------------===

  /// 对指定 value 进行向上的基于 DFG 的数据切片
  /// 使用 SSA 关系追踪数据的定义链
  ///
  /// @param value 起始分析的 value
  /// @param useMemorySSA 是否使用 Memory SSA（true=追踪tensor/pointer，false=追踪标量）
  /// @return ProgramSlice（使用 GraphAnalysis 中的类）
  ProgramSlice computeBackwardSlice(Value value, bool useMemorySSA = false);

  /// 批量对多个 values 进行切片
  /// 在同一个切片内自动去重（避免同一切片中重复包含同一指令）
  /// 注意：不同切片之间可以重复使用同一指令
  ProgramSlice computeBackwardSliceForValues(ArrayRef<Value> values,
                                              bool useMemorySSA = false);

  /// 获取按拓扑序排列的切片指令
  /// 由于 ProgramSlice 内部使用 DenseSet 存储，不保证顺序，
  /// 此方法返回按 CFG 拓扑序排序的指令列表
  SmallVector<Instruction*> getOrderedSliceInstructions(const ProgramSlice& slice) const;

  //===----------------------------------------------------------------------===
  // 分析状态追踪 API
  //===----------------------------------------------------------------------===

  /// 检查指定 instruction 是否已出现在至少一个切片中（被分析过）
  /// 注意：这只是查询状态，不会阻止该指令出现在其他切片中
  bool isInstructionAnalyzed(Instruction* inst) const {
    return analyzedInstructions.contains(inst);
  }

  /// 检查指定 instruction 是否已出现在至少一个切片中（通过 operation 查询）
  bool isOperationAnalyzed(Operation* op) const;

  /// 记录指定 instruction 已出现在某个切片中
  /// 注意：这仅用于追踪分析状态，不会阻止该指令出现在其他切片中
  void markInstructionAnalyzed(Instruction* inst) {
    analyzedInstructions.insert(inst);
  }

  /// 记录整个切片的指令为已分析
  void markSliceAnalyzed(const ProgramSlice& slice);

  /// 获取所有已分析的指令
  const DenseSet<Instruction*>& getAnalyzedInstructions() const {
    return analyzedInstructions;
  }

  /// 清除已分析标记（用于重新开始分析）
  void clearAnalyzedInstructions() {
    analyzedInstructions.clear();
  }

  /// 获取指定 instruction 的拓扑序（缓存）
  unsigned getTopoOrder(Instruction* inst) const;

private:
  ControlFlowGraph& cfg;
  DataFlowGraph& dfg;
  ProgramSlicer slicer;  // 复用 GraphAnalysis 中的切片器

  // 已分析的指令集合（记录哪些指令已出现在至少一个切片中，用于追踪分析状态）
  // 注意：不同切片可以重复使用同一条指令，此集合仅用于查询某指令是否被分析过
  DenseSet<Instruction*> analyzedInstructions;

  // 拓扑序缓存
  mutable std::unordered_map<Instruction*, unsigned> topoOrderCache;
  mutable bool topoOrderCached = false;

  // 构建拓扑序缓存
  void buildTopoOrderCache() const;

  // 判断是否为 load 操作
  static bool isLoadOp(Operation* op);

  // 判断是否为 store 操作
  static bool isStoreOp(Operation* op);

  // 判断是否为 dot 操作
  static bool isDotOp(Operation* op);
};

} // namespace cfg
} // namespace triton
} // namespace mlir

#endif // TRITON_TO_GRAPH_TENSOR_ANALYZER_H
