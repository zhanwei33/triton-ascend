/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */

#ifndef TRITON_TO_GRAPH_GRAPH_ANALYSIS_H
#define TRITON_TO_GRAPH_GRAPH_ANALYSIS_H

#include "TritonToGraph/ControlFlowGraph.h"
#include "TritonToGraph/DataflowGraph.h"
#include "mlir/IR/Value.h"
#include "mlir/IR/Operation.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include <type_traits>

namespace mlir {
namespace triton {
namespace cfg {

//===----------------------------------------------------------------------===//
// Traversal Context - 遍历时维护上下文
//===----------------------------------------------------------------------===//

struct TraversalContext {
  // 当前所处的嵌套结构栈（从外到内）
  SmallVector<BasicBlock*> structureStack;

  // 当前深度
  int depth = 0;

  void push(BasicBlock* structure) { structureStack.push_back(structure); }
  void pop() { if (!structureStack.empty()) structureStack.pop_back(); }
  BasicBlock* currentStructure() const {
    return structureStack.empty() ? nullptr : structureStack.back();
  }
  bool isEmpty() const { return structureStack.empty(); }
};

/// CFGTraversalBase - CFG 遍历基类
/// 用户继承此类，实现 preVisit/postVisit 回调
/// 可在子类中添加自定义状态字段
class CFGTraversalBase {
public:
  virtual ~CFGTraversalBase() = default;

  // 访问基本块前调用，返回 false 则跳过此块
  virtual bool preVisitBlock(BasicBlock* block, TraversalContext& ctx);

  // 访问基本块后调用
  virtual void postVisitBlock(BasicBlock* block, TraversalContext& ctx) {}

  // 访问指令后调用
  virtual void VisitInstruction(Instruction* inst, TraversalContext& ctx) {}

  // 遇到控制流结构（for/if/while）时调用
  virtual void onEnterStructure(BasicBlock* structure, TraversalContext& ctx) {}
  virtual void onExitStructure(BasicBlock* structure, TraversalContext& ctx) {}

  // 遇到回边时调用
  virtual void onBackEdge(BasicBlock* from, BasicBlock* to, TraversalContext& ctx) {}
};

/// CFGTraverser - CFG 遍历器（双向支持）
class CFGTraverser {
public:
  explicit CFGTraverser(ControlFlowGraph& cfg) : cfg(cfg) {}

  //===----------------------------------------------------------------------===
  // Forward Traversal (沿后继节点向下遍历)
  //===----------------------------------------------------------------------===

  /// 从入口开始的 DFS
  void dfsForward(CFGTraversalBase& visitor);

  /// 从指定块开始的 DFS
  void dfsForward(BasicBlock* start, CFGTraversalBase& visitor);

  /// BFS 遍历
  void bfsForward(CFGTraversalBase& visitor);
  void bfsForward(BasicBlock* start, CFGTraversalBase& visitor);

  //===----------------------------------------------------------------------===
  // Backward Traversal (沿前驱节点向上遍历)
  //===----------------------------------------------------------------------===

  /// 从指定块反向 DFS（沿前驱遍历）
  void dfsBackward(BasicBlock* start, CFGTraversalBase& visitor);

  /// 从指定块反向 BFS
  void bfsBackward(BasicBlock* start, CFGTraversalBase& visitor);

private:
  ControlFlowGraph& cfg;

  void dfsForwardImpl(BasicBlock* block, DenseSet<BasicBlock*>& visited,
                      TraversalContext& ctx, CFGTraversalBase& visitor);
  void dfsBackwardImpl(BasicBlock* block, DenseSet<BasicBlock*>& visited,
                       TraversalContext& ctx, CFGTraversalBase& visitor);
};

/// Region - 指令集合（替代原始代码中的 SmallVector<Operation*>）
class OpsRegion {
public:
  explicit OpsRegion(StringRef name = "") : name_(name.str()) {}

  void add(Instruction* inst);
  void add(Operation* op, ControlFlowGraph& cfg);
  void addAll(ArrayRef<Instruction*> insts);

  bool contains(Instruction* inst) const;
  bool contains(Operation* op) const;

  void remove(Instruction* inst);
  void clear();

  size_t size() const { return instSet_.size(); }
  bool empty() const { return instSet_.empty(); }

  // 获取按块内顺序排序的指令列表
  SmallVector<Instruction*> orderedInstructions() const;

  // 获取所有操作
  SmallVector<Operation*> operations() const;

  StringRef name() const { return name_; }
  void setName(StringRef name) { name_ = name.str(); }

  // 迭代器支持
  auto begin() const { return instSet_.begin(); }
  auto end() const { return instSet_.end(); }

private:
  std::string name_;
  DenseSet<Instruction*> instSet_;
};

/// ProgramSlice - 程序切片
class ProgramSlice {
public:
  void add(Instruction* inst) { instructions_.insert(inst); }
  void addAll(const OpsRegion& region);

  bool contains(Instruction* inst) const {
    return instructions_.contains(inst);
  }

  size_t size() const { return instructions_.size(); }
  bool empty() const { return instructions_.empty(); }

  // 获取入口点（没有前驱在切片中）
  SmallVector<Instruction*> entryPoints(DataFlowGraph& dfg) const;

  // 获取出口点（没有后继在切片中）
  SmallVector<Instruction*> exitPoints(DataFlowGraph& dfg) const;

  // 集合操作
  void merge(const ProgramSlice& other);
  void intersect(const ProgramSlice& other);
  void subtract(const ProgramSlice& other);

  // 转换为 Region
  OpsRegion toRegion(StringRef name = "") const;

  // 迭代器
  auto begin() const { return instructions_.begin(); }
  auto end() const { return instructions_.end(); }

  /// 记录 defOp 到 value 的映射（用于 for/if results, iter_args 等）
  DenseMap<Operation*, Value> definedValues_;

  /// 获取 defOp 对应的 value（用于 for/if results, iter_args 等）
  Value getDefinedValue(Operation* defOp) const;

private:
  DenseSet<Instruction*> instructions_;
};

//===----------------------------------------------------------------------===//
// Curly Recursive Template Pattern for DFG Traversal
//===----------------------------------------------------------------------===//

/// DFGTraversalBase - DFG 遍历基类
class DFGTraversalBase {
public:
  ProgramSlice slice;

  virtual ~DFGTraversalBase() = default;

  // 访问定义前调用（反向遍历 value -> def）
  virtual bool VisitDef(Value value, Operation* defOp, int depth) 
  {
    return true;
  }

  // 访问使用前调用（正向遍历 value -> use）
  virtual bool VisitUse(Value value, OpOperand* use, int depth)
  {
    return true;
  }

  // 遇到 phi/iter_arg 时调用
  virtual void onPhi(Value phiValue, const PhiInfo& phiInfo, int depth) {}
};

/// DFGTraverser - DFG 遍历器（双向 SSA/MemorySSA）
class DFGTraverser {
public:
  struct Options {
    bool useMemorySSA = false;      // false=传统 SSA, true=Memory SSA
    bool followPhi = true;          // 是否跨越 phi/iter_arg
    int maxDepth = -1;              // -1 = 无限制
    DenseSet<Operation*> stopOps;   // 遇到停止的操作

    // 显式默认构造，解决聚合初始化限制
    Options() 
    {
      useMemorySSA = false;
      followPhi = true;
      maxDepth = -1;
    }
    
    // 如果需要带参数的构造，可以添加
    explicit Options(bool memSSA) : useMemorySSA(memSSA) {}
  };

  explicit DFGTraverser(DataFlowGraph& dfg) : dfg(dfg) {}

  //===----------------------------------------------------------------------===
  // Backward Traversal (value -> definitions)
  //===----------------------------------------------------------------------===

  /// 从 value 开始反向追踪所有定义
  void dfsBackward(Value seed, DFGTraversalBase& visitor,
                   const Options& opts = {});

  /// BFS 反向追踪
  void bfsBackward(Value seed, DFGTraversalBase& visitor,
                   const Options& opts = {});

  //===----------------------------------------------------------------------===
  // Forward Traversal (value -> uses)
  //===----------------------------------------------------------------------===

  /// 从 value 开始正向追踪所有使用
  void dfsForward(Value seed, DFGTraversalBase& visitor,
                  const Options& opts = {});

  /// 多起点正向 DFS
  void dfsForward(ArrayRef<Value> seeds, DFGTraversalBase& visitor,
                  const Options& opts = {});

  /// BFS 正向追踪
  void bfsForward(Value seed, DFGTraversalBase& visitor,
                  const Options& opts = {});

  //===----------------------------------------------------------------------===
  // Bidirectional
  //===----------------------------------------------------------------------===

  /// 从 value 双向遍历（先 backward 到根，再 forward 到所有 uses）
  void traverseBidirectional(Value seed, DFGTraversalBase& visitor,
                             const Options& opts = {});

private:
  DataFlowGraph& dfg;

  void dfsBackwardImpl(Value value, DFGTraversalBase& visitor,
                       DenseSet<Operation*>& visited,
                       const Options& opts, int depth);
  void dfsForwardImpl(Value value, DFGTraversalBase& visitor,
                      DenseSet<Operation*>& visited,
                      const Options& opts, int depth);
};

//===----------------------------------------------------------------------===//
// Region Abstraction and Analysis
//===----------------------------------------------------------------------===//

/// RegionAnalyzer - Region 分析器
class RegionAnalyzer {
public:
  struct Dependency {
    enum Type { DATA, CONTROL };
    Type type;
    Value value;
    Instruction* from;
    Instruction* to;
  };

  struct ExternalDeps {
    // 外部定义 -> 内部使用
    struct Input {
      Value value;
      Instruction* externalDef;  // null 表示函数参数
      SmallVector<Instruction*> internalUses;
    };
    // 内部定义 -> 外部使用
    struct Output {
      Value value;
      Instruction* internalDef;
      SmallVector<Instruction*> externalUses;
    };
    SmallVector<Input> inputs;
    SmallVector<Output> outputs;
  };

  explicit RegionAnalyzer(DataFlowGraph& dfg, ControlFlowGraph& cfg)
      : dfg(dfg), cfg(cfg) {}

  // 检查 region 之间是否有依赖
  bool hasDependency(const OpsRegion& from, const OpsRegion& to) const;

  // 获取两个 region 间的所有依赖
  SmallVector<Dependency> getDependencies(const OpsRegion& from,
                                          const OpsRegion& to) const;

  // 分析 region 的外部依赖
  ExternalDeps analyzeExternalDeps(const OpsRegion& region) const;

  // 检查依赖是否为循环依赖（region A 依赖 B，B 又依赖 A）
  bool isCyclicDependency(const OpsRegion& a, const OpsRegion& b) const;

private:
  DataFlowGraph& dfg;
  ControlFlowGraph& cfg;
};

//===----------------------------------------------------------------------===//
// Program Slicing
//===----------------------------------------------------------------------===//

/// SliceCriterion - 切片准则
struct SliceCriterion {
  enum Direction { BACKWARD, FORWARD, BIDIRECTIONAL };
  SmallVector<Value> seeds;
  Direction dir = BACKWARD;
  DFGTraverser::Options dfgOpts;
};

/// ProgramSlicer - 程序切片器
class ProgramSlicer {
public:
  ProgramSlicer(DataFlowGraph& dfg, ControlFlowGraph& cfg)
      : dfg(dfg), cfg(cfg) {}

  // 计算切片（传入自定义遍历器）
  void compute(const SliceCriterion& criterion, DFGTraversalBase& visitor);

  // 计算切片（使用默认遍历器）
  void compute(const SliceCriterion& criterion);

  // 多切片操作
  static ProgramSlice merge(ArrayRef<ProgramSlice> slices);
  static ProgramSlice intersect(ArrayRef<ProgramSlice> slices);

  // 检查切片间依赖
  struct SliceDependency {
    const ProgramSlice* from;
    const ProgramSlice* to;
    SmallVector<Value> values;
  };
  SmallVector<SliceDependency> computeDependencies(
      ArrayRef<ProgramSlice> slices);

private:
  DataFlowGraph& dfg;
  ControlFlowGraph& cfg;
};

//===----------------------------------------------------------------------===//
// Region Absorption
//===----------------------------------------------------------------------===//

/// AbsorptionPolicy - 吸收策略
struct AbsorptionPolicy {
  enum Direction { UPSTREAM, DOWNSTREAM, BOTH };
  Direction dir = BOTH;
  int maxDepth = -1;
  bool crossRegionBoundary = false;
  DenseSet<Operation*> stopOps;
  std::function<bool(Instruction*)> shouldStop = nullptr;
};

/// RegionAbsorber - Region 吸收器
class RegionAbsorber {
public:
  RegionAbsorber(DataFlowGraph& dfg, ControlFlowGraph& cfg)
      : dfg(dfg), cfg(cfg) {}

  // 从种子指令开始吸收
  void absorb(OpsRegion& region, ArrayRef<Instruction*> seeds,
              const AbsorptionPolicy& policy);

  // 从 value 的 def/use 链吸收
  void absorbFromValue(OpsRegion& region, Value value,
                       const AbsorptionPolicy& policy);

  // 吸收直到遇到边界
  void absorbUntilBoundary(OpsRegion& region, ArrayRef<Instruction*> seeds,
                           std::function<bool(Instruction*)> isBoundary);

private:
  DataFlowGraph& dfg;
  ControlFlowGraph& cfg;

  void absorbUpstream(OpsRegion& region, Instruction* inst,
                      const AbsorptionPolicy& policy,
                      DenseSet<Instruction*>& visited, int depth);
  void absorbDownstream(OpsRegion& region, Instruction* inst,
                        const AbsorptionPolicy& policy,
                        DenseSet<Instruction*>& visited, int depth);
};

} // namespace cfg
} // namespace triton
} // namespace mlir

#endif // TRITON_TO_GRAPH_GRAPH_ANALYSIS_H
