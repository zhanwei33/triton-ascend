/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */

#include "TritonToGraph/TensorAnalyzer.h"
#include "triton/Dialect/Triton/IR/Dialect.h"
#include "triton/Dialect/Triton/IR/OpInterfaces.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "llvm/Support/Debug.h"
#include <memory>

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
ProgramSlice& TensorAnalyzer::computeBackwardSlice(Value value,
                                                   DFGTraversalBase& visitor,
                                                   bool useMemorySSA) {
  SliceCriterion criterion;
  criterion.seeds.push_back(value);
  criterion.dir = SliceCriterion::BACKWARD;
  criterion.dfgOpts.useMemorySSA = useMemorySSA;
  criterion.dfgOpts.followPhi = true;

  LLVM_DEBUG(llvm::dbgs()
                 << "[TensorAnalyzer] Computing backward slice with custom visitor\n");

  // 使用自定义 visitor 进行切片计算
  // visitor 通过 DFGTraversalBase::slice 引用收集指令
  slicer.compute(criterion, visitor);

  // 从 visitor 的 slice 成员获取结果
  ProgramSlice& slice = visitor.slice;

  // 记录切片中的指令为已分析
  markSliceAnalyzed(slice);

  LLVM_DEBUG(llvm::dbgs()
                 << "[TensorAnalyzer] Slice complete: " << slice.size()
                 << " instructions\n");

  return slice;
}

SmallVector<Instruction*> TensorAnalyzer::getOrderedSliceInstructions(
    const ProgramSlice& slice) const {
  SmallVector<Instruction*> result;
  result.reserve(slice.size());

  // 1. 收集切片中的所有指令
  for (Instruction* inst : slice) {
    result.push_back(inst);
  }

  // 2. 按拓扑序（从小到大）显式排序
  llvm::sort(result, [this](Instruction* a, Instruction* b) {
    return getTopoOrder(a) < getTopoOrder(b);
  });

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

//===----------------------------------------------------------------------===//
// 符号执行分析实现（T14 新增）
//===----------------------------------------------------------------------===//

void TensorAnalyzer::ensureSymbolicExecutionInitialized() const {
  if (!symExecState) {
    symExecState = std::make_unique<ascend::SymbolicExecutionState>();
  }
  if (!symExecEngine) {
    symExecEngine = std::make_unique<ascend::SymbolicExecutionEngine>();
  }
}

ascend::TensorAccessInfo TensorAnalyzer::analyzeLoadWithSymbolicExecution(
    triton::LoadOp loadOp) {
  using namespace ascend;

  // 确保符号执行引擎已初始化
  ensureSymbolicExecutionInitialized();

  LLVM_DEBUG(llvm::dbgs()
                 << "[TensorAnalyzer] Starting symbolic execution analysis for load\n");

  // Step 1: 对 load 的 ptr 进行程序切片（使用定制的遍历器）
  Value ptr = loadOp.getPtr();

  // 定制的 DFGTraversal：遇到 Load 停止追踪，记录 for/if 的 definedValues
  class LoadSliceBuilder : public DFGTraversalBase {
  public:
    LoadSliceBuilder(ControlFlowGraph& c) : cfg(c) {}

    bool VisitDef(Value value, Operation* defOp, int depth) override {
      // 将指令加入 slice
      if (Instruction* inst = cfg.getInstruction(defOp)) {
        slice.add(inst);
      }

      // 如果是 Load 操作，停止追踪其 operand
      if (isa<triton::LoadOp>(defOp)) {
        return false;
      }

      // 如果是 for/if 操作，记录其定义的 values
      if (isa<scf::ForOp>(defOp) || isa<scf::IfOp>(defOp)) {
          slice.definedValues_[defOp] = &value;
      }

      return true;
    }

  private:
    ControlFlowGraph& cfg;
  };

  LoadSliceBuilder builder(cfg);
  computeBackwardSlice(ptr, builder, /*useMemorySSA=*/false);

  LLVM_DEBUG(llvm::dbgs()
                 << "[TensorAnalyzer] Computed backward slice with "
                 << builder.slice.size() << " instructions\n");

  // Step 2: 获取拓扑序排列的切片指令
  SmallVector<Instruction*> orderedInsts = getOrderedSliceInstructions(builder.slice);

  // Step 3: 符号执行切片中的指令
  // 重置符号执行状态（为每个load创建新的状态）
  symExecState = std::make_unique<SymbolicExecutionState>();

  for (Instruction* inst : orderedInsts) {
    Operation* op = inst->getOperation();
    if (op) {
      symExecEngine->executeOperation(op, *symExecState);
    }
  }

  LLVM_DEBUG(llvm::dbgs()
                 << "[TensorAnalyzer] Symbolic execution completed\n");

  // Step 4: 使用 LoadPatternAnalyzer 分析访问模式
  LoadPatternAnalyzer patternAnalyzer;
  TensorAccessInfo info = patternAnalyzer.analyzeLoad(loadOp, *symExecState);

  LLVM_DEBUG(llvm::dbgs()
                 << "[TensorAnalyzer] Load pattern analysis completed\n");

  return info;
}

SmallVector<ascend::TensorAccessInfo>
TensorAnalyzer::analyzeAllLoadsWithSymbolicExecution() {
  using namespace ascend;

  SmallVector<TensorAccessInfo> results;

  // 收集所有 load 指令
  SmallVector<Instruction*> loads = collectLoadInstructions();

  LLVM_DEBUG(llvm::dbgs()
                 << "[TensorAnalyzer] Analyzing " << loads.size()
                 << " load instructions with symbolic execution\n");

  for (Instruction* inst : loads) {
    if (auto loadOp = dyn_cast<triton::LoadOp>(inst->getOperation())) {
      TensorAccessInfo info = analyzeLoadWithSymbolicExecution(loadOp);
      results.push_back(info);

      // 打印分析结果（调试模式）
      LLVM_DEBUG({
        llvm::dbgs() << "=== Load Analysis Result ===\n";
        info.print(llvm::dbgs());
      });
    }
  }

  return results;
}
