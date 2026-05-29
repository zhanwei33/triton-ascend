# TritonToGraph 源码详细分析

## 一、模块概览

TritonToGraph 是一个**程序分析框架**，构建在 MLIR 基础设施之上，用于对 Triton IR (TTIR) 进行深层的静态分析。整个模块分为三层架构：

```
┌─────────────────────────────────────────────────┐
│  上层分析工具                                   │
│  TensorAnalyzer / SymbolicExecution /            │
│  SValPatternAnalyzer / GraphAnalysis            │
├─────────────────────────────────────────────────┤
│  中间表示层                                     │
│  DataFlowGraph (DFG) + MemorySSA                │
│  AliasAnalysis                                  │
├─────────────────────────────────────────────────┤
│  底层图结构                                     │
│  ControlFlowGraph (CFG) / InterProceduralCFG    │
│  SymValue (符号值体系)                          │
└─────────────────────────────────────────────────┘
```

共 14 个头文件 + 11 个实现文件，约 4500+ 行代码。

---

## 二、底层基础设施

### 2.1 SymValue — 符号值体系 (`SymValue.h/.cpp`)

这是符号执行的**核心数据结构**，用于在编译时跟踪值的符号表达式而非具体数值。

**类继承体系：**

```
SymValue (基类)
├── ScalarSV (标量基类，含 dims 维度信息)
│   ├── ScalarConstantSV
│   │   ├── ScalarConstantIntSV    — 整数常量
│   │   └── ScalarConstantFloatSV  — 浮点常量
│   ├── AddExprSV / SubExprSV / MulExprSV / DivExprSV  — 四则运算
│   ├── RemExprSV                  — 取模运算
│   ├── AndExprSV                  — 按位与
│   ├── RangeExprSV                — make_range 产生的 [start, end)
│   ├── CmpExprSV                  — 比较表达式 (EQ/NE/LT/LE/GT/GE)
│   ├── SelectExprSV               — 选择表达式 (条件 ? true : false)
│   ├── PtrExprSV                  — 指针表达式 (base + offset)
│   ├── TensorPtrSV                — make_tensor_ptr 的块指针
│   ├── ProgramIDSV                — get_program_id
│   ├── GmPtrSV                    — 全局内存指针入参
│   ├── UnknownSV                  — 未知值 (如 load 结果)
│   ├── InductionSV                — for 循环迭代变量
│   ├── IterArgSV                  — for 的 iter_arg
│   └── ArgSV                      — 非指针类型入参
└── TensorSV (张量，含 elementExpr 标量表达式)
```

**关键设计点：**
- 所有 SymValue 继承自 `std::enable_shared_from_this<SymValue>`，使用 `shared_ptr` 管理生命周期
- `ScalarSV::dims` 记录了该标量在哪些维度上存在：`[-1]` 表示未关联 Tensor，`[0]`, `[1]` 等表示关联的维度
- `TensorSV` 不仅存储 shape 和 elementType，还持有一个 `elementExpr`（标量表达式），形成"张量 = 形状 + 元素表达式"的分解
- 使用 LLVM RTTI 机制 (`classof` / `isa<>` / `dyn_cast<>`) 做类型判断

### 2.2 ControlFlowGraph — 控制流图 (`ControlFlowGraph.h/.cpp`)

提供自定义的 CFG 表示，不直接使用 MLIR 的 Region/Block 结构。

**核心类：**

| 类 | 职责 |
|---|---|
| `Instruction` | 指令节点，封装 `Operation*`，含 MemorySSAInfo、子图指针 |
| `BasicBlock` | 基本块，含类型标签、指令列表、前驱/后继边 |
| `ControlFlowGraph` | 完整的函数级 CFG，管理所有块、边、yield 映射 |

**BlockType 枚举（10 种类型）：**
- `ENTRY` / `EXIT` — 函数入口/出口
- `NORMAL` — 普通块（含多条指令）
- `IF_COND` / `FOR_COND` / `WHILE_COND` — 结构化控制流条件块
- `COND_BR` / `BR` — 非结构化跳转
- `LOOP_BODY` / `LOOP_EXIT` — 循环体/出口

**关键能力：**
- **Yield 映射追踪**：`IfYieldResultMapping` 和 `ForYieldIterArgMapping` 记录了 scf.if/scf.for 的 yield value 与 result/iter_arg 的对应关系
- **结构化搜索**：`searchNormalBlock` / `searchCondBlock` 支持沿 successor 顺序的递归遍历
- **拓扑序计算**：`computeInstructionTopoOrder()` 计算每条指令在 CFG 中的拓扑序（忽略回边）
- **导出**：支持 DOT、JSON、HTML 多种导出格式

### 2.3 ControlFlowGraphBuilder — CFG 构建器 (`ControlFlowGraphBuilder.h/.cpp`)

从 MLIR 的 `triton::FuncOp` 构建 CFG。

**核心流程：**
1. `buildForFunction()` — 创建 ENTRY/EXIT 块，调用 `buildForRegion()` 处理函数体
2. `buildForRegion()` → `processBlock()` — 遍历 MLIR Block 中的每个 Operation
3. 遇到 `scf.if` → `handleIfOp()` 创建 IF_COND + then/else 分支 + 合并块
4. 遇到 `scf.for` → `handleForOp()` 创建 FOR_COND + LOOP_BODY + LOOP_EXIT
5. 遇到 `scf.while` → `handleWhileOp()` 创建 WHILE_COND + before/after 块
6. 遇到 `cf.cond_br` / `cf.br` → 创建 COND_BR / BR 块
7. 普通操作用 `createInstruction()` 封装为 Instruction

同时也是一个 MLIR Pass (`BuildCFGPass`)，可通过 `--build-cfg` 选项运行。

### 2.4 InterProceduralCFG — 过程间 CFG (`InterProceduralCFG.h/.cpp`)

跨函数的过程间控制流图 (ICFG)。

**功能：**
- 为 Module 中所有函数构建 CFG
- 收集所有调用点 (`CallSite`)
- 连接调用图边 (`connectCallGraph`)
- 计算全局可达性 (`computeReachability`)
- 导出为 DOT/HTML 进行可视化

### 2.5 tensor.h — Tensor 对象 (`tensor.h`)

`TensorObject` 是对 MLIR Tensor 的轻量级抽象，用于 Memory SSA 分析。

**属性：**
- `name` — 唯一名称（如 `"gm_obj_0"`）
- `shape` — 形状
- `type` / `elementType` — MLIR 类型信息
- `kind` — 内存层级：`GLOBAL_MEMORY` / `L2` / `L1` / `UB`

`ComputeType` 枚举将计算分类为 `CUBE` (2D)、`VECTOR` (1D)、`SCALAR` (0D)。

---

## 三、中间表示层

### 3.1 MemorySSA — 内存 SSA (`MemorySSA.h`)

为 Tensor/Pointer 类型的值实现类 SSA 的 def-use 链跟踪。

**核心类：**

| 类 | 职责 |
|---|---|
| `MemorySSADef` | 内存定义 — 关联 `TensorObject*` + `Operation*` + 版本号 |
| `MemorySSAUse` | 内存使用 — 关联 `MemorySSADef*` + `Operation*` + operand 索引 |
| `PhiInfo` | Phi 节点信息 — 支持 `ITER_ARG`/`IF_RESULT`/`WHILE_ARG` 三种类型 |
| `MemorySSAInfo` | 每条指令的 Memory SSA 信息容器 — 包含 uses, definitions, alias 信息 |

**设计思路：**
- 每个 Tensor/Pointer 写操作产生一个新的 `MemorySSADef`，带递增的 version
- 每个读/使用操作产生 `MemorySSAUse`，指向对应的 definition
- 控制流汇合点（if/for/while）产生 `PhiInfo`，记录 initial value 和 yield value

### 3.2 AliasAnalysis — 别名分析 (`AliasAnalysis.h/.cpp`)

分析指针间的别名关系。

**功能：**
- `analyzePointerAliases()` — 为整个 CFG 分析指针别名
- `getBasePointer()` — 递归查找真实的基指针
- `mayAlias()` — 判断两个指针是否指向同一 tensor
- 支持 `addptr`、`make_tensor_ptr`、`load`、`store`、`broadcast`、`splat` 等多种操作的别名跟踪
- 维护 `aliasMap` (ptr → base ptr) 和 `baseTensorMap` (value → TensorObject)

### 3.3 MemorySsaBuilder — Memory SSA 构建器 (`MemorySsaBuilder.h/.cpp`)

构建整个 CFG 的 Memory SSA 信息。

**构建流程：**
1. `createParameterDefinitions()` — 为函数入参创建 MemorySSADef
2. `processBasicBlock()` → `processInstruction()` — 遍历每个基本块的每条指令
3. 对 `scf.if` → `processIfOp()` 处理后继块中的 Phi 合并
4. 对 `scf.for` → `processForOp()` 处理 iter_args 循环携带依赖
5. 对 `scf.while` → `processWhileOp()` 处理 while 循环参数

通过 `isTensorWriter`/`isMemoryWriter`/`isPointerOp` 等辅助方法判断操作类型，决定是否创建新的 definition。

### 3.4 DataflowGraph — 数据流图 (`DataflowGraph.h/.cpp`)

整合 Memory SSA 和传统 SSA，提供统一的数据流查询接口。

**核心类 `DataFlowInfo`：**
- 存储 `memoryDefinitions` (Value → MemorySSADef*) 和 `memoryUses`
- 存储 `Phis` (Value → PhiInfo)
- 提供 `queryDataFlow(Value)` — 统一查询，返回 `DataFlowResult`（使用 LLVM RTTI 分派到 `MemorySSAResult` / `SSAResult` / `NoneResult`）
- 支持 def-use 缓存加速查询

---

## 四、上层分析工具

### 4.1 SymbolicExecution — 符号执行 (`SymbolicExecution.h/.cpp`)

**符号执行引擎**，对 MLIR 操作进行抽象解释，构建符号值表达式树。

**核心执行器覆盖：**

| 类别 | 覆盖的操作 |
|---|---|
| Arith | `constant`, `addi/subi/muli/divi`, `addf/subf/mulf/divf`, `remsi/remui`, `select`, `cmpi` |
| Triton | `get_program_id`, `make_range`, `splat`, `addptr`, `expand_dims`, `broadcast`, `make_tensor_ptr`, `load` |
| SCF | `for`, `if`, `yield` |

**关键执行逻辑：**
- **arith.constant** → 创建 `ScalarConstantIntSV` / `ScalarConstantFloatSV`，对 Tensor 常量创建 `TensorSV::createSplat`
- **arith binary** → 对 Tensor 结果调用 `TensorSV::createComputed`，对 Scalar 结果创建对应的表达式 SV（AddExprSV 等）
- **arith.cmpi** → 创建 `CmpExprSV`，对于 Tensor 版本使用 SourceKind（CmpEQ/CmpNE 等）
- **arith.select** → 创建 `SelectExprSV`（标量）或 `TensorSV::createSelect`（张量）
- **make_tensor_ptr** → 创建 `TensorPtrSV`，保存 shape/strides/offsets/blockShape
- **for** → 对 induction var 创建 `InductionSV`，对 iter_arg 创建 `IterArgSV`（通过 `cfg_->getIterArgPair` 获取精确的 init/yield 对应关系）
- **if** → 结果设为 `UnknownSV`（简化处理）
- **load** → 结果设为 `UnknownSV`

**未处理的操作**统一创建 `UnknownSV`，不阻塞分析流程。

`SymbolicExecutionState` 是一个简单的 `DenseMap<Value, shared_ptr<SymValue>>`，维护从 MLIR Value 到符号值的映射。

### 4.2 SValPatternAnalyzer — 符号值模式分析器 (`SValPatternAnalyzer.h/.cpp`)

这是整个模块中**最复杂的分析器**，用于从符号表达式树中识别内存访问模式。

**分析流程（主入口 `analyze()`）：**

```
输入: SymValue (TensorSV 或 PtrExprSV)
  │
  ├── 1. propagateDimsToRange()  — DFS 遍历，将父节点的 dims 传播给子节点中的 RangeExprSV
  │
  ├── 2. hoistSelectWithRange()  — 提升包含 RangeExpr 的 SelectExpr 节点
  │     (如果 true/false 分支包含 Range，用该分支替换 Select)
  │
  ├── 3. expandDistribution()    — 展开分配律: (a+b)*c → a*c + b*c (最多2层)
  │     (收集+重建模式，非递归)
  │
  ├── 4. normalizeTerms()        — 归一化项
  │     collectAddTerms() → 分类为:
  │       - basePtr (GmPtrSV)
  │       - offsetTerms (不含 RangeExprSV)
  │       - strideTerms (含 RangeExprSV，按 dims[0] 升序排列)
  │
  └── 5. 根据 shape.size() 分类:
        ├── 2D → analyzeMatrix()
        ├── 1D → analyzeVector()
        └── 0D → analyzeScalar()
        
        或 PtrExprSV → analyzePtrExpr()
```

**输出 `TensorPattern`：**
- `kind` — Scalar / Vector / Matrix
- `shape` — 形状
- `basePtr` — 基指针 (GmPtrSV)
- `baseOffset` — 静态偏移（归拢后的无 Range 部分）
- `axisStrides` — 各轴 stride（带连续标记）
- `isContinuous` — 各轴是否连续（stride == 1）

这个分析器对于理解一个 `tt.load` 的指针表达式背后实际是 1D 连续访问还是 2D 矩阵访问至关重要，直接影响后续的算子映射和代码生成。

### 4.3 TensorAnalyzer — Tensor 指令分析器 (`TensorAnalyzer.h/.cpp`)

面向 load/store/dot 等 Tensor 操作的高层分析接口。

**三层设计：**

1. **指令收集** — `collectLoadInstructions()` / `collectStoreInstructions()` / `collectDotInstructions()`
2. **程序切片** — `computeBackwardSlice()` 基于 DFG 向后切片，使用自定义遍历器
3. **符号执行分析** — `analyzeLoadWithSymbolicExecution()` 串联整个分析管道：

```
tt.load
  │
  ├── Step 1: 对 load 的 ptr operand 做向后切片
  │     (使用 LoadSliceBuilder，遇 Load 停止，记录 for/if 的 definedValues)
  │
  ├── Step 2: 获取拓扑序排列的切片指令
  │
  ├── Step 3: 符号执行切片中的每条指令
  │     (重置状态 → 创建入参 SymValue → 按拓扑序执行)
  │
  └── Step 4: 使用 SValPatternAnalyzer 分析 ptr 的符号值
        (识别 Vector/Matrix/Scalar 访问模式)
```

**附加功能：**
- 分析状态追踪 (`analyzedInstructions` set)
- 拓扑序缓存 (`topoOrderCache`)
- 懒加载符号执行引擎

### 4.4 GraphAnalysis — 图分析工具集 (`GraphAnalysis.h/.cpp`)

提供通用的图遍历和分析能力。

**主要组件：**

| 组件 | 功能 |
|---|---|
| `CFGTraversalBase` | CFG 遍历回调接口（preVisit/postVisit/VisitInstruction/onEnterStructure/onExitStructure/onBackEdge） |
| `CFGTraverser` | CFG 遍历器 — DFS/BFS + 正向/反向 |
| `DFGTraversalBase` | DFG 遍历回调接口（VisitDef/VisitUse/onPhi），含 `ProgramSlice slice` 成员 |
| `DFGTraverser` | DFG 遍历器 — 支持 SSA/MemorySSA 两种模式，可选跨 Phi、深度限制、停止集合 |
| `OpsRegion` | 指令集合，支持 contains/add/remove/排序迭代 |
| `ProgramSlice` | 程序切片，DenseSet 存储，支持 entryPoints/exitPoints/集合运算 |
| `RegionAnalyzer` | 区域依赖分析（DATA/CONTROL 依赖、外部依赖检测、循环依赖判断） |
| `ProgramSlicer` | 程序切片器 — 支持 BACKWARD/FORWARD/BIDIRECTIONAL |
| `RegionAbsorber` | 区域吸收器 — 从种子沿 def-use 链扩展 region |

**设计亮点：**
- Curly Recursive Template Pattern — 遍历器通过继承 `DFGTraversalBase` 自定义行为
- 双向遍历支持（从 value 沿 def 链向上 + 沿 use 链向下）
- 切片间依赖分析

---

## 五、Pass 注册 (`Passes.h` / `Passes.td`)

通过 MLIR TableGen 定义 `BuildCFG` Pass：
- 操作对象：`mlir::ModuleOp`
- 命令行参数：`--build-cfg`
- 选项：`--output-dir` 指定输出目录

---

## 六、各模块间的协作关系

以分析一个 `tt.load` 操作为例，整体数据流如下：

```
                    tt.load (MLIR Operation)
                         │
                         ▼
            TensorAnalyzer::analyzeLoadWithSymbolicExecution()
                         │
          ┌──────────────┼──────────────┐
          │              │              │
          ▼              ▼              ▼
    ProgramSlicer   SymbolicExecution  SValPatternAnalyzer
    (DFGTraverser)  (executeOperation) (analyze)
          │              │              │
          ▼              ▼              ▼
    DataFlowGraph   SymValue Tree    TensorPattern
    (MemorySSA)     (各种 SV 节点)   (Vector/Matrix/Scalar)
          │
          ▼
    ControlFlowGraph
    (BasicBlock/Instruction)
```

1. **CFG + CFGBuilder** 提供指令的拓扑结构
2. **AliasAnalysis + MemorySSABuilder** 在 CFG 上构建 Memory SSA
3. **DataFlowGraph** 整合 Memory SSA 和传统 SSA，提供统一查询
4. **ProgramSlicer** 基于 DFG 做程序切片
5. **SymbolicExecution** 对切片指令进行符号执行，构建 SymValue 树
6. **SValPatternAnalyzer** 对 SymValue 树进行模式识别，输出结构化的 `TensorPattern`

---

## 七、总结

TritonToGraph 是一个完整的 **Triton IR 静态分析框架**，它从 MLIR 的 Triton 方言出发，构建了自定义的 CFG/DFG 图表示，实现了 Memory SSA 形式的 def-use 分析，并通过符号执行 + 模式识别来理解内存访问模式。其主要目标是为 Ascend NPU 后端提供精确的访存模式信息，以便进行高效的算子映射和代码生成。
