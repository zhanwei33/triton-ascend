# LoadPatternAnalyzer 重构设计方案

## 概述

基于最新的 SymValue.h 和 SymbolicExecution.h，重新设计 LoadPatternAnalyzer 的实现方案。

## 1. 核心 API 映射（旧 → 新）

| 旧 API | 新 API | 说明 |
|--------|--------|------|
| `PtrTensorSV` | `TensorPtrSV` | 块指针，包含 shape/strides/offsets/blockShape |
| `PtrBaseSV` | `GmPtrSV` | kernel 入参指针 |
| `ScalarExprSV` | `AddExprSV`, `CmpExprSV`, `SelectExprSV` 等 | 具体表达式类型 |
| `TensorExprSV` | `TensorSV` + `elementExpr` | 通过 elementExpr 访问元素表达式 |
| `TensorRangeSV` | `RangeExprSV` | make_range 生成的 range |
| `UnknownValueSV` | `UnknownSV` | 未知值 |

## 2. 数据结构更新

```cpp
// TensorAccessInfo 保持不变，但需要更新内部使用
struct TensorAccessInfo {
  // ... 现有字段 ...

  // 新增：用于存储指针表达式的各种形式
  std::shared_ptr<TensorPtrSV> tensorPtr;  // make_tensor_ptr
  std::shared_ptr<PtrExprSV> ptrExpr;      // addptr 结果
  std::shared_ptr<GmPtrSV> gmPtr;          // kernel 入参

  // 新增：offset 分析结果
  SmallVector<std::shared_ptr<ScalarSV>> offsetExprs;  // 各维度 offset 表达式

  // 新增：是否使用块指针
  bool isBlockPtr = false;
};
```

## 3. 核心分析流程重构

```cpp
class LoadPatternAnalyzer {
public:
  TensorAccessInfo analyzeLoad(tt::LoadOp loadOp,
                               const SymbolicExecutionState& state);

private:
  //===---------------------------------------------------------------------===
  // 指针类型分发（关键重构）
  //===---------------------------------------------------------------------===

  /// 分析 TensorPtrSV (make_tensor_ptr 结果)
  TensorAccessInfo analyzeTensorPtr(
      std::shared_ptr<TensorPtrSV> tensorPtr,
      tt::LoadOp loadOp);

  /// 分析 PtrExprSV (addptr 结果)
  TensorAccessInfo analyzePtrExpr(
      std::shared_ptr<PtrExprSV> ptrExpr,
      tt::LoadOp loadOp);

  /// 分析 GmPtrSV (kernel 入参)
  TensorAccessInfo analyzeGmPtr(
      std::shared_ptr<GmPtrSV> gmPtr,
      tt::LoadOp loadOp);

  //===---------------------------------------------------------------------===
  // Offset 分析（基于新 ScalarSV 层次结构）
  //===---------------------------------------------------------------------===

  /// 从 TensorPtrSV 的 offsets 推导访问模式
  void analyzeTensorPtrOffsets(
      TensorPtrSV* tensorPtr,
      TensorAccessInfo& info);

  /// 从 PtrExprSV 分析偏移表达式
  void analyzePtrExprOffset(
      PtrExprSV* ptrExpr,
      TensorAccessInfo& info);

  /// 递归分析 offset 表达式的结构
  /// 识别: pid * stride + arange(0, N) 模式
  OffsetPattern analyzeOffsetRecursive(
      ScalarSV* offset);

  //===---------------------------------------------------------------------===
  // Min 截断识别（基于 SelectExprSV）
  //===---------------------------------------------------------------------===

  /// 识别 select(cmp_lt(idx, bound), idx, 0) 模式
  bool detectMinTruncation(
      ScalarSV* offset,
      int64_t& bound,
      ScalarSV*& range);

  /// 检查 SelectExprSV 是否为 min 模式
  bool isMinPattern(SelectExprSV* select);

  //===---------------------------------------------------------------------===
  // 连续性分析
  //===---------------------------------------------------------------------===

  /// 基于 strides 和 offsets 分析连续性
  void analyzeContiguity(TensorAccessInfo& info);

  /// 检查 offset 表达式是否为连续模式
  /// 例如: arange(0, 128) 或 pid * 128 + arange(0, 128)
  bool isContiguousPattern(ScalarSV* offset);

  //===---------------------------------------------------------------------===
  // Mask 分析
  //===---------------------------------------------------------------------===

  /// 分析 load 的 mask 操作数
  void analyzeMask(tt::LoadOp loadOp,
                   TensorAccessInfo& info,
                   const SymbolicExecutionState& state);

  /// 从 TensorSV (mask 通常是 Tensor) 提取边界
  bool extractBoundFromMaskTensor(
      TensorSV* maskTensor,
      int64_t& bound);

  /// 从 CmpExprSV 提取边界: cmp_lt(idx, bound)
  bool extractBoundFromCmp(
      CmpExprSV* cmp,
      int64_t& bound);
};
```

## 4. 关键实现细节

### 4.1 指针类型分发逻辑

```cpp
TensorAccessInfo LoadPatternAnalyzer::analyzeLoad(
    tt::LoadOp loadOp, const SymbolicExecutionState& state) {

  TensorAccessInfo info;
  Value ptr = loadOp.getPtr();
  info.basePtr = ptr;

  auto ptrSym = state.getSymValue(ptr);
  if (!ptrSym) return info;

  // 根据类型分发
  if (auto tensorPtr = std::dynamic_pointer_cast<TensorPtrSV>(ptrSym)) {
    // make_tensor_ptr 结果
    info = analyzeTensorPtr(tensorPtr, loadOp);
    info.isBlockPtr = true;
  }
  else if (auto ptrExpr = std::dynamic_pointer_cast<PtrExprSV>(ptrSym)) {
    // tt.addptr 结果
    info = analyzePtrExpr(ptrExpr, loadOp);
  }
  else if (auto gmPtr = std::dynamic_pointer_cast<GmPtrSV>(ptrSym)) {
    // kernel 入参指针
    info = analyzeGmPtr(gmPtr, loadOp);
  }
  else if (auto ptrScalar = std::dynamic_pointer_cast<ScalarSV>(ptrSym)) {
    // 其他标量指针（如纯 offset 计算结果）
    info.baseOffset = ptrScalar.get();
  }

  // 分析 mask 和其他属性
  analyzeMask(loadOp, info, state);
  analyzePadding(loadOp, info, state);

  return info;
}
```

### 4.2 TensorPtrSV 分析

```cpp
TensorAccessInfo LoadPatternAnalyzer::analyzeTensorPtr(
    std::shared_ptr<TensorPtrSV> tensorPtr,
    tt::LoadOp loadOp) {

  TensorAccessInfo info;
  info.tensorPtr = tensorPtr;

  // 提取 shape（符号表达式）
  auto shapeExprs = tensorPtr->getShape();
  info.shape.clear();
  for (auto& s : shapeExprs) {
    if (auto constInt = std::dynamic_pointer_cast<ScalarConstantIntSV>(s)) {
      info.shape.push_back(constInt->getInt());
    } else {
      info.shape.push_back(-1);  // 未知维度
    }
  }

  // 提取 blockShape
  info.blockShape = SmallVector<int64_t>(tensorPtr->getBlockShape());

  // 提取 element type
  info.elementType = tensorPtr->getPointeeType();

  // 分析 offsets（关键）
  analyzeTensorPtrOffsets(tensorPtr.get(), info);

  // 推导 strides
  auto strideExprs = tensorPtr->getStrides();
  info.strides.clear();
  for (auto& s : strideExprs) {
    if (auto constInt = std::dynamic_pointer_cast<ScalarConstantIntSV>(s)) {
      info.strides.push_back(constInt->getInt());
    } else {
      info.strides.push_back(-1);
    }
  }

  return info;
}
```

### 4.3 Offset 递归分析（识别访问模式）

```cpp
enum class OffsetPatternKind {
  Unknown,
  Constant,           // 常量偏移
  ProgramID,          // pid.x/y/z
  Range,              // arange(start, end)
  Linear,             // base + stride * idx
  AddExpr,            // offset1 + offset2
  MinTruncation,      // select(idx < bound, idx, 0)
  Broadcast           // splat 值
};

struct OffsetPattern {
  OffsetPatternKind kind;
  int64_t constantValue = 0;
  int axis = 0;  // for ProgramID
  int64_t rangeStart = 0, rangeEnd = 0;  // for Range
  ScalarSV* base = nullptr;
  int64_t stride = 0;
  ScalarSV* idx = nullptr;
  // ... 其他字段
};

OffsetPattern LoadPatternAnalyzer::analyzeOffsetRecursive(ScalarSV* offset) {
  OffsetPattern pattern;

  if (!offset) {
    pattern.kind = OffsetPatternKind::Unknown;
    return pattern;
  }

  // 检查具体类型
  if (auto constInt = dyn_cast<ScalarConstantIntSV>(offset)) {
    pattern.kind = OffsetPatternKind::Constant;
    pattern.constantValue = constInt->getInt();
  }
  else if (auto pid = dyn_cast<ProgramIDSV>(offset)) {
    pattern.kind = OffsetPatternKind::ProgramID;
    pattern.axis = pid->getAxis();
  }
  else if (auto range = dyn_cast<RangeExprSV>(offset)) {
    pattern.kind = OffsetPatternKind::Range;
    pattern.rangeStart = range->getStart();
    pattern.rangeEnd = range->getEnd();
  }
  else if (auto add = dyn_cast<AddExprSV>(offset)) {
    // 分析 LHS 和 RHS
    auto lhsPattern = analyzeOffsetRecursive(add->getLHS());
    auto rhsPattern = analyzeOffsetRecursive(add->getRHS());

    // 检查是否为线性模式: base + stride * idx
    if (lhsPattern.kind == OffsetPatternKind::ProgramID &&
        rhsPattern.kind == OffsetPatternKind::Range) {
      pattern.kind = OffsetPatternKind::Linear;
      pattern.base = add->getLHS();
      pattern.stride = 1;  // 需要进一步分析
      pattern.idx = add->getRHS();
    }
    else {
      pattern.kind = OffsetPatternKind::AddExpr;
    }
  }
  else if (auto mul = dyn_cast<MulExprSV>(offset)) {
    // 检查是否为 stride * idx
    if (auto strideConst = dyn_cast<ScalarConstantIntSV>(mul->getRHS())) {
      pattern.stride = strideConst->getInt();
      pattern.idx = mul->getLHS();
      pattern.base = nullptr;  // 纯乘法，无 base
      pattern.kind = OffsetPatternKind::Linear;
    }
  }
  else if (auto select = dyn_cast<SelectExprSV>(offset)) {
    // 检查是否为 min 截断模式
    if (isMinPattern(select)) {
      pattern.kind = OffsetPatternKind::MinTruncation;
    }
  }

  return pattern;
}
```

### 4.4 Min 截断识别（基于 SelectExprSV）

```cpp
bool LoadPatternAnalyzer::isMinPattern(SelectExprSV* select) {
  if (!select) return false;

  // 使用 SelectExprSV 的内置方法
  if (select->isMinPattern()) {
    return true;
  }

  // 手动检查: select(cmp_lt(x, y), x, y)
  auto cond = select->getCondition();
  if (!cond) return false;

  // 检查条件是否为小于比较
  if (cond->getPred() != CmpExprSV::Pred::LT &&
      cond->getPred() != CmpExprSV::Pred::LE) {
    return false;
  }

  // 检查结构: trueVal 应该是 condition 的 lhs
  if (select->getTrueVal() != cond->getLHS()) {
    return false;
  }

  // falseVal 可以是 condition 的 rhs 或其他默认值
  return true;
}

bool LoadPatternAnalyzer::detectMinTruncation(
    ScalarSV* offset, int64_t& bound, ScalarSV*& range) {

  // 递归查找 select 表达式
  if (auto select = dyn_cast<SelectExprSV>(offset)) {
    if (isMinPattern(select)) {
      auto cond = select->getCondition();
      range = cond->getLHS();  // idx

      // 尝试从 RHS 提取 bound
      if (auto boundConst = dyn_cast<ScalarConstantIntSV>(cond->getRHS())) {
        bound = boundConst->getInt();
        return true;
      }
    }
  }

  // 递归检查子表达式
  if (auto add = dyn_cast<AddExprSV>(offset)) {
    if (detectMinTruncation(add->getLHS(), bound, range)) return true;
    if (detectMinTruncation(add->getRHS(), bound, range)) return true;
  }
  if (auto mul = dyn_cast<MulExprSV>(offset)) {
    if (detectMinTruncation(mul->getLHS(), bound, range)) return true;
    if (detectMinTruncation(mul->getRHS(), bound, range)) return true;
  }

  return false;
}
```

### 4.5 Mask 分析（基于 TensorSV）

```cpp
void LoadPatternAnalyzer::analyzeMask(
    tt::LoadOp loadOp, TensorAccessInfo& info,
    const SymbolicExecutionState& state) {

  // 获取 mask 操作数（假设是倒数第二个）
  if (loadOp->getNumOperands() < 2) return;

  Value mask = loadOp->getOperand(loadOp->getNumOperands() - 2);
  auto maskSym = state.getTensorValue(mask);

  if (!maskSym) return;

  info.hasMask = true;
  info.maskValue = mask;
  info.maskSymValue = maskSym.get();

  // mask 通常是 TensorSV，其 elementExpr 是比较表达式
  auto elemExpr = maskSym->getElementExpr();
  if (!elemExpr) return;

  // 检查是否为比较表达式
  if (auto cmp = dyn_cast<CmpExprSV>(elemExpr)) {
    int64_t bound;
    if (extractBoundFromCmp(cmp, bound)) {
      info.lengthBound = bound;
      info.hasLengthCheck = true;
    }
  }
}

bool LoadPatternAnalyzer::extractBoundFromCmp(
    CmpExprSV* cmp, int64_t& bound) {

  // 检查 cmp_lt(idx, bound) 模式
  if (cmp->getPred() == CmpExprSV::Pred::LT ||
      cmp->getPred() == CmpExprSV::Pred::LE) {
    if (auto boundConst = dyn_cast<ScalarConstantIntSV>(cmp->getRHS())) {
      bound = boundConst->getInt();
      return true;
    }
  }
  return false;
}
```

## 5. 访问模式分类逻辑

```cpp
void LoadPatternAnalyzer::classifyAccessPattern(TensorAccessInfo& info) {
  // 1. 检查是否为循环依赖
  if (info.isLoopDependent) {
    info.pattern = TensorAccessInfo::AccessPattern::LoopDependent;
    return;
  }

  // 2. 检查是否为标量访问
  if (info.isScalarAccess()) {
    info.pattern = TensorAccessInfo::AccessPattern::ScalarSequential;
    return;
  }

  // 3. 基于连续性分类
  if (info.contiguousAxis >= 0) {
    if (info.shape.size() == 1) {
      // 1D 连续访问 = Gather 连续
      info.pattern = TensorAccessInfo::AccessPattern::GatherContiguous;
    } else {
      info.pattern = TensorAccessInfo::AccessPattern::TensorContiguous;
    }
  } else {
    info.pattern = TensorAccessInfo::AccessPattern::TensorStrided;
  }

  // 4. 特殊处理：如果有 length check，可能是 varlen 序列
  if (info.hasLengthCheck && info.shape.size() == 1) {
    // 可能是变长序列加载
    info.pattern = TensorAccessInfo::AccessPattern::ScalarSequential;
  }
}
```

## 6. 关键变化总结

| 方面 | 旧实现 | 新实现 |
|------|--------|--------|
| **指针表示** | `PtrTensorSV`, `PtrBaseSV` | `TensorPtrSV`, `PtrExprSV`, `GmPtrSV` |
| **表达式** | `ScalarExprSV` (统一) | 具体类型 (`AddExprSV`, `CmpExprSV`, `SelectExprSV`) |
| **Range** | `TensorRangeSV` | `RangeExprSV` |
| **Min 检测** | `ScalarExprSV::isLengthCheckPattern()` | `SelectExprSV::isMinPattern()` |
| **Offset 获取** | `ptrTensor->getElementOffset(idx)` | 直接从 `tensorPtr->getOffsets()` 获取 |
| **连续性** | 基于 stride 计算 | 基于 `offsetExprs` 的模式识别 |

## 7. 实现检查清单

- [ ] 更新 LoadPatternAnalyzer.h 头文件
- [ ] 实现指针类型分发逻辑
- [ ] 实现 TensorPtrSV 分析
- [ ] 实现 PtrExprSV 分析
- [ ] 实现 GmPtrSV 分析
- [ ] 实现 Offset 递归分析
- [ ] 实现 Min 截断识别
- [ ] 实现连续性分析
- [ ] 实现 Mask 分析
- [ ] 添加单元测试
