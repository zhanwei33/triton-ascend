/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */

#include "TritonToGraph/TensorAnalyzer.h"
#include "triton/Dialect/Triton/IR/Dialect.h"
#include "triton/Dialect/Triton/IR/OpInterfaces.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "tensor-analyzer"

using namespace mlir;
using namespace triton;
using namespace cfg;

//===----------------------------------------------------------------------===//
// 辅助函数
//===----------------------------------------------------------------------===//

bool TensorAnalyzer::isLoadOp(Operation* op) {
  if (!op) return false;
  return isa<triton::LoadOp>(op) ||
         op->getName().getStringRef().contains("load");
}

bool TensorAnalyzer::isStoreOp(Operation* op) {
  if (!op) return false;
  return isa<triton::StoreOp>(op) ||
         op->getName().getStringRef().contains("store");
}

bool TensorAnalyzer::isDotOp(Operation* op) {
  if (!op) return false;
  return isa<triton::DotOp>(op) ||
         op->getName().getStringRef().contains("dot");
}

//===----------------------------------------------------------------------===//
// 指令收集实现
//===----------------------------------------------------------------------===//

SmallVector<Instruction*> TensorAnalyzer::collectLoadInstructions() const {
  SmallVector<Instruction*> result;

  // 使用 CFG 的 traverse API 遍历所有基本块
  cfg.traverse([&result](BasicBlock& bb) {
    for (const auto& instPtr : bb.getInstructions()) {
      Instruction* inst = instPtr.get();
      if (isLoadOp(inst->getOperation())) {
        result.push_back(inst);
      }
    }
  });

  LLVM_DEBUG(llvm::dbgs() << "[TensorAnalyzer] Collected " << result.size()
                    << " load instructions\n");
  return result;
}

SmallVector<Instruction*> TensorAnalyzer::collectStoreInstructions() const {
  SmallVector<Instruction*> result;

  // 使用 CFG 的 traverse API 遍历所有基本块
  cfg.traverse([&result](BasicBlock& bb) {
    for (const auto& instPtr : bb.getInstructions()) {
      Instruction* inst = instPtr.get();
      if (isStoreOp(inst->getOperation())) {
        result.push_back(inst);
      }
    }
  });

  LLVM_DEBUG(llvm::dbgs() << "[TensorAnalyzer] Collected " << result.size()
                    << " store instructions\n");
  return result;
}

SmallVector<Instruction*> TensorAnalyzer::collectDotInstructions() const {
  SmallVector<Instruction*> result;

  // 使用 CFG 的 traverse API 遍历所有基本块
  cfg.traverse([&result](BasicBlock& bb) {
    for (const auto& instPtr : bb.getInstructions()) {
      Instruction* inst = instPtr.get();
      if (isDotOp(inst->getOperation())) {
        result.push_back(inst);
      }
    }
  });

  LLVM_DEBUG(llvm::dbgs() << "[TensorAnalyzer] Collected " << result.size()
                    << " dot instructions\n");
  return result;
}

TensorAnalyzer::TensorInstructions
TensorAnalyzer::collectAllTensorInstructions() const {
  TensorInstructions result;
  result.loads = collectLoadInstructions();
  result.stores = collectStoreInstructions();
  result.dots = collectDotInstructions();

  LLVM_DEBUG(llvm::dbgs()
                 << "[TensorAnalyzer] Total tensor instructions: "
                 << result.loads.size() << " loads, "
                 << result.stores.size() << " stores, "
                 << result.dots.size() << " dots\n");
  return result;
}

//===----------------------------------------------------------------------===//
// 拓扑序缓存
//===----------------------------------------------------------------------===//

void TensorAnalyzer::buildTopoOrderCache() const {
  if (topoOrderCached) return;

  // 直接使用 CFG 提供的拓扑序计算函数
  topoOrderCache = cfg.computeInstructionTopoOrder();
  topoOrderCached = true;

  LLVM_DEBUG(llvm::dbgs() << "[TensorAnalyzer] Built topo order cache for "
                    << topoOrderCache.size() << " instructions\n");
}

unsigned TensorAnalyzer::getTopoOrder(Instruction* inst) const {
  buildTopoOrderCache();
  auto it = topoOrderCache.find(inst);
  if (it != topoOrderCache.end()) {
    return it->second;
  }
  return UINT_MAX;  // 未找到时返回最大值
}

//===----------------------------------------------------------------------===//
// 程序切片实现
// 复用 GraphAnalysis::ProgramSlicer
//===----------------------------------------------------------------------===//

ProgramSlice TensorAnalyzer::computeBackwardSlice(Value value, bool useMemorySSA) {
  SliceCriterion criterion;
  criterion.seeds.push_back(value);
  criterion.dir = SliceCriterion::BACKWARD;
  criterion.dfgOpts.useMemorySSA = useMemorySSA;
  criterion.dfgOpts.followPhi = true;

  LLVM_DEBUG(llvm::dbgs()
                 << "[TensorAnalyzer] Computing backward slice for value\n");

  ProgramSlice slice = slicer.compute(criterion);

  // 记录切片中的指令为已分析
  markSliceAnalyzed(slice);

  LLVM_DEBUG(llvm::dbgs()
                 << "[TensorAnalyzer] Slice complete: " << slice.size()
                 << " instructions\n");

  return slice;
}

ProgramSlice TensorAnalyzer::computeBackwardSliceForValues(
    ArrayRef<Value> values, bool useMemorySSA) {
  SliceCriterion criterion;
  criterion.seeds = SmallVector<Value>(values);
  criterion.dir = SliceCriterion::BACKWARD;
  criterion.dfgOpts.useMemorySSA = useMemorySSA;
  criterion.dfgOpts.followPhi = true;

  LLVM_DEBUG(llvm::dbgs()
                 << "[TensorAnalyzer] Computing backward slice for "
                 << values.size() << " values\n");

  ProgramSlice slice = slicer.compute(criterion);

  // 记录切片中的指令为已分析
  markSliceAnalyzed(slice);

  LLVM_DEBUG(llvm::dbgs()
                 << "[TensorAnalyzer] Multi-value slice complete: "
                 << slice.size() << " instructions\n");

  return slice;
}

SmallVector<Instruction*> TensorAnalyzer::getOrderedSliceInstructions(
    const ProgramSlice& slice) const {
  SmallVector<Instruction*> result;
  result.reserve(slice.size());

  // 将切片中的指令按拓扑序排序
  // 方法：遍历 CFG 的拓扑序，选择在切片中的指令
  buildTopoOrderCache();

  for (const auto& entry : topoOrderCache) {
    Instruction* inst = entry.first;
    if (slice.contains(inst)) {
      result.push_back(inst);
    }
  }

  return result;
}

//===----------------------------------------------------------------------===//
// 分析状态追踪
//===----------------------------------------------------------------------===//

bool TensorAnalyzer::isOperationAnalyzed(Operation* op) const {
  if (!op) return false;
  Instruction* inst = cfg.getInstruction(op);
  if (!inst) return false;
  return isInstructionAnalyzed(inst);
}

void TensorAnalyzer::markSliceAnalyzed(const ProgramSlice& slice) {
  for (Instruction* inst : slice) {
    analyzedInstructions.insert(inst);
  }
}
