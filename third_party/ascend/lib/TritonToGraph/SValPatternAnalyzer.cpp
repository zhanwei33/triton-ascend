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
    basePtr->print(os, 0);
    os << "\n";
  }

  if (baseOffset) {
    os << "  BaseOffset: ";
    baseOffset->print(os, 0);
    os << "\n";
  }

  os << "  AxisStrides (" << axisStrides.size() << " axes):\n";
  for (size_t i = 0; i < axisStrides.size(); ++i) {
    os << "    Axis[" << i << "]: ";
    axisStrides[i]->print(os, 0);
    os << "  " << (isContinuous[i] ? "[continuous]" : "[strided]") << "\n";

    // 打印原始 stride term（如果可用）
    if (i < strideTerms.size() && strideTerms[i]) {
      os << "  (from: ";
      strideTerms[i]->print(os, 0);
      os << ")";
    }
    os << "\n";
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

    // 1. 传播 dims 到 RangeExprSV
    propagateDimsToRange(tensor->elementExpr.get(), {});

    // 2. 提升包含 RangeExpr 的 SelectExpr
    auto hoisted = hoistSelectWithRange(tensor->elementExpr);

    // 3. 展开分配律（最多2层）
    auto expanded = expandDistribution(hoisted, 0);

    // 4. 归一化
    NormalizedTerms terms = normalizeTerms(expanded.get());

    // 5. 根据 shape 分类处理
    auto shape = tensor->getShape();
    Type elemType = tensor->getElementType();
    Operation* op = tensor->getOperation();

    if (shape.size() == 2) {
      return analyzeMatrix(shape, elemType, terms, op);
    } else if (shape.size() == 1) {
      return analyzeVector(shape, elemType, terms, op);
    } else {
      return analyzeScalar(shape, elemType, terms, op);
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

// DFS 传播 dims 到 RangeExprSV
void SValPatternAnalyzer::propagateDimsToRange(
    ScalarSV* sv, ArrayRef<int> parentDims) {
  if (!sv) return;

  // 确定当前节点应该使用的 dims
  ArrayRef<int> currentDims = parentDims;

  // 如果是 RangeExprSV，设置 dims
  if (auto* range = dyn_cast<RangeExprSV>(sv)) {
    if (!currentDims.empty() && currentDims[0] != -1) {
      range->setDims(SmallVector<int>(currentDims));
    }
    return;
  }

  if(sv->isAssociated())
    currentDims = sv->dims;
  
  // 递归处理子表达式 - 直接使用裸指针
  if (auto* add = dyn_cast<AddExprSV>(sv)) {
    propagateDimsToRange(add->getLHS(), currentDims);
    propagateDimsToRange(add->getRHS(), currentDims);
  } else if (auto* mul = dyn_cast<MulExprSV>(sv)) {
    propagateDimsToRange(mul->getLHS(), currentDims);
    propagateDimsToRange(mul->getRHS(), currentDims);
  } else if (auto* sub = dyn_cast<SubExprSV>(sv)) {
    propagateDimsToRange(sub->getLHS(), currentDims);
    propagateDimsToRange(sub->getRHS(), currentDims);
  } else if (auto* div = dyn_cast<DivExprSV>(sv)) {
    propagateDimsToRange(div->getLHS(), currentDims);
    propagateDimsToRange(div->getRHS(), currentDims);
  } else if (auto* rem = dyn_cast<RemExprSV>(sv)) {
    propagateDimsToRange(rem->getLHS(), currentDims);
    propagateDimsToRange(rem->getRHS(), currentDims);
  } else if (auto* andExpr = dyn_cast<AndExprSV>(sv)) {
    propagateDimsToRange(andExpr->getLHS(), currentDims);
    propagateDimsToRange(andExpr->getRHS(), currentDims);
  } else if (auto* ptrExpr = dyn_cast<PtrExprSV>(sv)) {
    propagateDimsToRange(ptrExpr->getBasePtr(), currentDims);
    propagateDimsToRange(ptrExpr->getOffset(), currentDims);
  } else if (auto* select = dyn_cast<SelectExprSV>(sv)) {
    propagateDimsToRange(select->getTrueVal(), currentDims);
    propagateDimsToRange(select->getFalseVal(), currentDims);
  }
}

// 辅助函数：从裸指针获取 shared_ptr（简化内部使用）
static std::shared_ptr<ScalarSV> shared_from_raw(ScalarSV* ptr) {
  if (!ptr) return nullptr;
  return std::static_pointer_cast<ScalarSV>(ptr->shared_from_this());
}

// 记录 Select 节点与父节点的关系
struct SelectNodeInfo {
  SelectExprSV* select;       // SelectExpr 节点
  ScalarSV* parent;           // 父节点
  bool isLeftChild;           // 在父节点中是左子节点(true)还是右子节点(false)
  bool isTrueBranch;          // 对于 SelectExpr 的父节点，表示是 trueVal(false) 还是 falseVal(true)
  int childIndex;             // 对于 PtrExprSV: 0=base, 1=offset
};

// DFS 收集所有 SelectExpr 节点（非递归，使用栈）
static void collectSelectNodes(ScalarSV* sv, ScalarSV* parent, bool isLeft,
                                bool isTrueBranch, int childIndex,
                                SmallVector<SelectNodeInfo>& selectNodes) {
  if (!sv) return;

  // 如果是 SelectExpr，记录信息
  if (auto* select = dyn_cast<SelectExprSV>(sv)) {
    selectNodes.push_back({select, parent, isLeft, isTrueBranch, childIndex});
  }

  // 递归遍历子节点
  if (auto* add = dyn_cast<AddExprSV>(sv)) {
    collectSelectNodes(add->getLHS(), sv, true, false, 0, selectNodes);
    collectSelectNodes(add->getRHS(), sv, false, false, 0, selectNodes);
  } else if (auto* sub = dyn_cast<SubExprSV>(sv)) {
    collectSelectNodes(sub->getLHS(), sv, true, false, 0, selectNodes);
    collectSelectNodes(sub->getRHS(), sv, false, false, 0, selectNodes);
  } else if (auto* mul = dyn_cast<MulExprSV>(sv)) {
    collectSelectNodes(mul->getLHS(), sv, true, false, 0, selectNodes);
    collectSelectNodes(mul->getRHS(), sv, false, false, 0, selectNodes);
  } else if (auto* div = dyn_cast<DivExprSV>(sv)) {
    collectSelectNodes(div->getLHS(), sv, true, false, 0, selectNodes);
    collectSelectNodes(div->getRHS(), sv, false, false, 0, selectNodes);
  } else if (auto* rem = dyn_cast<RemExprSV>(sv)) {
    collectSelectNodes(rem->getLHS(), sv, true, false, 0, selectNodes);
    collectSelectNodes(rem->getRHS(), sv, false, false, 0, selectNodes);
  } else if (auto* andExpr = dyn_cast<AndExprSV>(sv)) {
    collectSelectNodes(andExpr->getLHS(), sv, true, false, 0, selectNodes);
    collectSelectNodes(andExpr->getRHS(), sv, false, false, 0, selectNodes);
  } else if (auto* cmp = dyn_cast<CmpExprSV>(sv)) {
    collectSelectNodes(cmp->getLHS(), sv, true, false, 0, selectNodes);
    collectSelectNodes(cmp->getRHS(), sv, false, false, 0, selectNodes);
  } else if (auto* ptrExpr = dyn_cast<PtrExprSV>(sv)) {
    collectSelectNodes(ptrExpr->getBasePtr(), sv, false, false, 0, selectNodes);
    collectSelectNodes(ptrExpr->getOffset(), sv, false, false, 1, selectNodes);
  } else if (auto* select = dyn_cast<SelectExprSV>(sv)) {
    // SelectExpr 的条件不可能是 SelectExpr（条件是 CmpExpr），但 true/false 可能是
    collectSelectNodes(select->getTrueVal(), sv, false, true, 0, selectNodes);
    collectSelectNodes(select->getFalseVal(), sv, false, false, 0, selectNodes);
  }
}

// 替换父节点的子节点
static void replaceChild(ScalarSV* parent, ScalarSV* oldChild,
                         std::shared_ptr<ScalarSV> newChild,
                         const SelectNodeInfo& info) {
  if (!parent) return;

  if (auto* add = dyn_cast<AddExprSV>(parent)) {
    if (info.isLeftChild) add->setLHS(newChild);
    else add->setRHS(newChild);
  } else if (auto* sub = dyn_cast<SubExprSV>(parent)) {
    if (info.isLeftChild) sub->setLHS(newChild);
    else sub->setRHS(newChild);
  } else if (auto* mul = dyn_cast<MulExprSV>(parent)) {
    if (info.isLeftChild) mul->setLHS(newChild);
    else mul->setRHS(newChild);
  } else if (auto* div = dyn_cast<DivExprSV>(parent)) {
    if (info.isLeftChild) div->setLHS(newChild);
    else div->setRHS(newChild);
  } else if (auto* rem = dyn_cast<RemExprSV>(parent)) {
    if (info.isLeftChild) rem->setLHS(newChild);
    else rem->setRHS(newChild);
  } else if (auto* andExpr = dyn_cast<AndExprSV>(parent)) {
    if (info.isLeftChild) andExpr->setLHS(newChild);
    else andExpr->setRHS(newChild);
  } else if (auto* cmp = dyn_cast<CmpExprSV>(parent)) {
    if (info.isLeftChild) cmp->setLHS(newChild);
    else cmp->setRHS(newChild);
  } else if (auto* ptrExpr = dyn_cast<PtrExprSV>(parent)) {
    if (info.childIndex == 0) ptrExpr->setBasePtr(newChild);
    else ptrExpr->setOffset(newChild);
  } else if (auto* select = dyn_cast<SelectExprSV>(parent)) {
    if (info.isTrueBranch) select->setTrueVal(newChild);
    else select->setFalseVal(newChild);
  }
}

// 提升包含 RangeExprSV 的 SelectExprSV（非递归方案）
std::shared_ptr<ScalarSV> SValPatternAnalyzer::hoistSelectWithRange(
    std::shared_ptr<ScalarSV> sv) {
  if (!sv) return sv;

  // 1. DFS 收集所有 SelectExpr 节点
  SmallVector<SelectNodeInfo> selectNodes;
  collectSelectNodes(sv.get(), nullptr, false, false, 0, selectNodes);

  // 2. 如果没有 Select，直接返回
  if (selectNodes.empty()) return sv;

  // 3. 判断每个 Select 是否包含 RangeExpr，直接替换父节点的子节点
  for (auto& info : selectNodes) {
    ScalarSV* replacement = nullptr;

    // 检查 trueVal 是否包含 RangeExpr
    if (containsRangeExpr(info.select->getTrueVal())) {
      replacement = info.select->getTrueVal();
    }
    // 检查 falseVal 是否包含 RangeExpr
    else if (containsRangeExpr(info.select->getFalseVal())) {
      replacement = info.select->getFalseVal();
    }

    // 如果找到包含 RangeExpr 的分支，替换父节点的对应子节点
    if (replacement && info.parent) {
      replaceChild(info.parent, info.select, shared_from_raw(replacement), info);
    }
  }

  return sv;
}

// 记录展开分配律的节点信息
struct ExpandNodeInfo {
  ScalarSV* node;           // 当前节点
  ScalarSV* parent;         // 父节点
  bool isLeftChild;         // 在父节点中是左子节点
};

// DFS 收集所有表达式节点
static void collectExprNodes(ScalarSV* sv, ScalarSV* parent, bool isLeft,
                              SmallVector<ExpandNodeInfo>& nodes) {
  if (!sv) return;

  nodes.push_back({sv, parent, isLeft});

  // 递归遍历子节点
  if (auto* add = dyn_cast<AddExprSV>(sv)) {
    collectExprNodes(add->getLHS(), sv, true, nodes);
    collectExprNodes(add->getRHS(), sv, false, nodes);
  } else if (auto* sub = dyn_cast<SubExprSV>(sv)) {
    collectExprNodes(sub->getLHS(), sv, true, nodes);
    collectExprNodes(sub->getRHS(), sv, false, nodes);
  } else if (auto* mul = dyn_cast<MulExprSV>(sv)) {
    collectExprNodes(mul->getLHS(), sv, true, nodes);
    collectExprNodes(mul->getRHS(), sv, false, nodes);
  } else if (auto* div = dyn_cast<DivExprSV>(sv)) {
    collectExprNodes(div->getLHS(), sv, true, nodes);
    collectExprNodes(div->getRHS(), sv, false, nodes);
  } else if (auto* rem = dyn_cast<RemExprSV>(sv)) {
    collectExprNodes(rem->getLHS(), sv, true, nodes);
    collectExprNodes(rem->getRHS(), sv, false, nodes);
  } else if (auto* andExpr = dyn_cast<AndExprSV>(sv)) {
    collectExprNodes(andExpr->getLHS(), sv, true, nodes);
    collectExprNodes(andExpr->getRHS(), sv, false, nodes);
  } else if (auto* ptrExpr = dyn_cast<PtrExprSV>(sv)) {
    collectExprNodes(ptrExpr->getBasePtr(), sv, true, nodes);
    collectExprNodes(ptrExpr->getOffset(), sv, false, nodes);
  } else if (auto* select = dyn_cast<SelectExprSV>(sv)) {
    collectExprNodes(select->getTrueVal(), sv, true, nodes);
    collectExprNodes(select->getFalseVal(), sv, false, nodes);
  }
}

// 重建表达式树，应用分配律展开
static std::shared_ptr<ScalarSV> rebuildWithExpansion(
    ScalarSV* sv,
    const DenseMap<ScalarSV*, std::shared_ptr<ScalarSV>>& expandMap,
    int depth) {
  if (!sv) return nullptr;

  // 检查当前节点是否有展开映射
  auto it = expandMap.find(sv);
  if (it != expandMap.end()) {
    // 返回展开后的新节点（已经是重建好的）
    return it->second;
  }

  // 否则正常递归重建
  if (auto* add = dyn_cast<AddExprSV>(sv)) {
    auto lhs = rebuildWithExpansion(add->getLHS(), expandMap, depth);
    auto rhs = rebuildWithExpansion(add->getRHS(), expandMap, depth);
    return std::make_shared<AddExprSV>(lhs, rhs, sv->getDataType(), sv->getOperation());
  } else if (auto* sub = dyn_cast<SubExprSV>(sv)) {
    auto lhs = rebuildWithExpansion(sub->getLHS(), expandMap, depth);
    auto rhs = rebuildWithExpansion(sub->getRHS(), expandMap, depth);
    return std::make_shared<SubExprSV>(lhs, rhs, sv->getDataType(), sv->getOperation());
  } else if (auto* mul = dyn_cast<MulExprSV>(sv)) {
    auto lhs = rebuildWithExpansion(mul->getLHS(), expandMap, depth);
    auto rhs = rebuildWithExpansion(mul->getRHS(), expandMap, depth);
    return std::make_shared<MulExprSV>(lhs, rhs, sv->getDataType(), sv->getOperation());
  } else if (auto* div = dyn_cast<DivExprSV>(sv)) {
    auto lhs = rebuildWithExpansion(div->getLHS(), expandMap, depth);
    auto rhs = rebuildWithExpansion(div->getRHS(), expandMap, depth);
    return std::make_shared<DivExprSV>(lhs, rhs, sv->getDataType(), sv->getOperation());
  } else if (auto* rem = dyn_cast<RemExprSV>(sv)) {
    auto lhs = rebuildWithExpansion(rem->getLHS(), expandMap, depth);
    auto rhs = rebuildWithExpansion(rem->getRHS(), expandMap, depth);
    return std::make_shared<RemExprSV>(lhs, rhs, sv->getDataType(), sv->getOperation());
  } else if (auto* andExpr = dyn_cast<AndExprSV>(sv)) {
    auto lhs = rebuildWithExpansion(andExpr->getLHS(), expandMap, depth);
    auto rhs = rebuildWithExpansion(andExpr->getRHS(), expandMap, depth);
    return std::make_shared<AndExprSV>(lhs, rhs, sv->getDataType(), sv->getOperation());
  } else if (auto* ptrExpr = dyn_cast<PtrExprSV>(sv)) {
    auto base = rebuildWithExpansion(ptrExpr->getBasePtr(), expandMap, depth);
    auto offset = rebuildWithExpansion(ptrExpr->getOffset(), expandMap, depth);
    return std::make_shared<PtrExprSV>(base, offset, ptrExpr->getPointeeType(), sv->getOperation());
  } else if (auto* select = dyn_cast<SelectExprSV>(sv)) {
    auto cond = std::static_pointer_cast<CmpExprSV>(
        rebuildWithExpansion(select->getCondition(), expandMap, depth));
    auto trueVal = rebuildWithExpansion(select->getTrueVal(), expandMap, depth);
    auto falseVal = rebuildWithExpansion(select->getFalseVal(), expandMap, depth);
    return std::make_shared<SelectExprSV>(cond, trueVal, falseVal, sv->getDataType(), sv->getOperation());
  }

  // 叶子节点
  return shared_from_raw(sv);
}

// 展开分配律（非递归：收集 + 重建）
std::shared_ptr<ScalarSV> SValPatternAnalyzer::expandDistribution(
    std::shared_ptr<ScalarSV> sv, int depth) {
  if (!sv || depth >= 1) return sv;

  // 1. 收集所有节点
  SmallVector<ExpandNodeInfo> nodes;
  collectExprNodes(sv.get(), nullptr, false, nodes);

  // 2. 找到所有可以展开的模式，建立映射
  DenseMap<ScalarSV*, std::shared_ptr<ScalarSV>> expandMap;

  for (auto& info : nodes) {
    auto* mul = dyn_cast<MulExprSV>(info.node);
    if (!mul) continue;

    auto* lhs = mul->getLHS();
    auto* rhs = mul->getRHS();

    // 判断是否需要展开：a或b必须包含 RangeExprSV
    bool lhsHasRange = containsRangeExpr(lhs);
    bool rhsHasRange = containsRangeExpr(rhs);
    bool shouldExpand = lhsHasRange || rhsHasRange;

    if (!shouldExpand) continue;

    // (a+b)*c -> a*c + b*c
    if (auto* add = dyn_cast<AddExprSV>(lhs)) {
      auto ac = std::make_shared<MulExprSV>(
          shared_from_raw(add->getLHS()), shared_from_raw(rhs),
          mul->getDataType(), mul->getOperation());
      auto bc = std::make_shared<MulExprSV>(
          shared_from_raw(add->getRHS()), shared_from_raw(rhs),
          mul->getDataType(), mul->getOperation());
      auto result = std::make_shared<AddExprSV>(
          ac, bc, mul->getDataType(), mul->getOperation());
      expandMap[mul] = result;
    }
    // c*(a+b) -> c*a + c*b
    else if (auto* add = dyn_cast<AddExprSV>(rhs)) {
      auto ca = std::make_shared<MulExprSV>(
          shared_from_raw(lhs), shared_from_raw(add->getLHS()),
          mul->getDataType(), mul->getOperation());
      auto cb = std::make_shared<MulExprSV>(
          shared_from_raw(lhs), shared_from_raw(add->getRHS()),
          mul->getDataType(), mul->getOperation());
      auto result = std::make_shared<AddExprSV>(
          ca, cb, mul->getDataType(), mul->getOperation());
      expandMap[mul] = result;
    }
  }

  // 3. 如果没有展开，直接返回
  if (expandMap.empty()) return sv;

  // 4. 重建表达式树
  auto result = rebuildWithExpansion(sv.get(), expandMap, depth);

  // 5. 递归处理下一层（因为展开后可能还有新的展开机会）
  return expandDistribution(result, depth + 1);
}

// 收集所有加法项
void SValPatternAnalyzer::collectAddTerms(
    ScalarSV* sv, SmallVector<ScalarSV*>& terms) {
  if (!sv) return;

  // 递归分解加法
  if (auto* add = dyn_cast<AddExprSV>(sv)) 
  {
    collectAddTerms(add->getLHS(), terms);
    collectAddTerms(add->getRHS(), terms);
  }
  // 如果是 PtrExprSV，提取 base + offset 结构
  else if (auto* ptrExpr = dyn_cast<PtrExprSV>(sv)) 
  {
    // 收集 base 部分
    collectAddTerms(ptrExpr->getBasePtr(), terms);
    // 收集 offset 部分
    collectAddTerms(ptrExpr->getOffset(), terms);
  } 
  else 
  {
    terms.push_back(sv);
  }
}

// 检查是否包含 RangeExprSV
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

// 把 RangeExprSV 交换到乘法最左面（原地修改）
void SValPatternAnalyzer::bringRangeToLeft(ScalarSV* sv) {
  if (!sv) return;

  // 检查是否是乘法
  if (auto* mul = dyn_cast<MulExprSV>(sv)) {
    auto* lhs = mul->getLHS();
    auto* rhs = mul->getRHS();

    // 如果 rhs 是 RangeExprSV，交换
    if (isa<RangeExprSV>(rhs)) {
      mul->setLHS(shared_from_raw(rhs));
      mul->setRHS(shared_from_raw(lhs));
    }
  }
}

// 提取 stride 乘数（RangeExprSV 外的乘数部分）
std::shared_ptr<ScalarSV> SValPatternAnalyzer::extractStrideMultiplier(ScalarSV* sv) {
  if (!sv) return nullptr;

  // 如果是 RangeExprSV，创建常量1
  if (isa<RangeExprSV>(sv)) {
    auto* range = cast<RangeExprSV>(sv);
    return std::make_shared<ScalarConstantIntSV>(1, range->getDataType(), sv->getOperation());
  }

  // 如果是乘法，提取非 RangeExprSV 的部分
  if (auto* mul = dyn_cast<MulExprSV>(sv)) {
    auto* lhs = mul->getLHS();
    auto* rhs = mul->getRHS();

    if (isa<RangeExprSV>(lhs)) {
      // lhs 是 range，rhs 是 multiplier
      return shared_from_raw(rhs);
    }
    if (isa<RangeExprSV>(rhs)) {
      // rhs 是 range，lhs 是 multiplier
      return shared_from_raw(lhs);
    }
  }

  // 其他情况返回原值
  return shared_from_raw(sv);
}

// 获取 RangeExprSV 所在的维度
int SValPatternAnalyzer::getRangeDim(ScalarSV* sv) {
  if (!sv) return -1;

  if (auto* range = dyn_cast<RangeExprSV>(sv)) {
    if (!range->dims.empty() && range->dims[0] != -1) {
      return range->dims[0];
    }
    return -1;
  }

  if (auto* mul = dyn_cast<MulExprSV>(sv)) {
    auto* lhs = mul->getLHS();
    auto* rhs = mul->getRHS();

    if (isa<RangeExprSV>(lhs)) {
      return getRangeDim(lhs);
    }
    if (isa<RangeExprSV>(rhs)) {
      return getRangeDim(rhs);
    }
  }

  return -1;
}

// 归一化项
NormalizedTerms SValPatternAnalyzer::normalizeTerms(ScalarSV* sv) {
  NormalizedTerms result;

  // 收集所有加法项
  SmallVector<ScalarSV*> allTerms;
  collectAddTerms(sv, allTerms);

  // 分类
  for (auto* term : allTerms) {
    // 将 RangeExprSV 交换到最左面
    bringRangeToLeft(term);

    if (isa<GmPtrSV>(term)) {
      result.basePtr = std::static_pointer_cast<GmPtrSV>(shared_from_raw(term));
    } else if (containsRangeExpr(term)) {
      // 包含 RangeExprSV，归到 strideTerms
      result.strideTerms.push_back(shared_from_raw(term));
    } else {
      // 不包含 RangeExprSV，归到 offsetTerms
      result.offsetTerms.push_back(shared_from_raw(term));
    }
  }

  // 按 dims[0] 升序排列 strideTerms
  llvm::sort(result.strideTerms, [this](const std::shared_ptr<ScalarSV>& a,
                                         const std::shared_ptr<ScalarSV>& b) {
    int dimA = getRangeDim(a.get());
    int dimB = getRangeDim(b.get());
    // -1 放在最后
    if (dimA == -1) return false;
    if (dimB == -1) return true;
    return dimA < dimB;
  });

  return result;
}

// 合并加法项
std::shared_ptr<ScalarSV> SValPatternAnalyzer::mergeAddTerms(ArrayRef<std::shared_ptr<ScalarSV>> terms) 
{
  if (terms.empty()) return nullptr;
  if (terms.size() == 1) return terms[0];

  // 合并为加法链
  auto result = terms[0];
  for (size_t i = 1; i < terms.size(); ++i) {
    result = std::make_shared<AddExprSV>(result, terms[i],
                                         terms[i]->getDataType(), terms[i]->getOperation());
  }
  return result;
}

//===----------------------------------------------------------------------===//
// 分类处理
//===----------------------------------------------------------------------===//

std::optional<TensorPattern> SValPatternAnalyzer::analyzeMatrix(
    ArrayRef<int64_t> shape, Type elemType,
    NormalizedTerms& terms, Operation* op) {
  TensorPattern pattern;
  pattern.kind = TensorPattern::Kind::Matrix;
  pattern.shape.append(shape.begin(), shape.end());
  pattern.elementType = elemType;
  pattern.basePtr = terms.basePtr;

  // 合并 offsetTerms 作为 baseOffset
  if (!terms.offsetTerms.empty()) {
    pattern.baseOffset = mergeAddTerms(terms.offsetTerms);
  }

  // 提取各轴的 stride（按 dims[0] 升序）
  for (auto& strideTerm : terms.strideTerms) {
    // 保存原始 term
    pattern.strideTerms.push_back(strideTerm);

    auto multiplier = extractStrideMultiplier(strideTerm.get());
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
    ArrayRef<int64_t> shape, Type elemType,
    NormalizedTerms& terms, Operation* op) {
  TensorPattern pattern;
  pattern.kind = TensorPattern::Kind::Vector;
  pattern.shape.append(shape.begin(), shape.end());
  pattern.elementType = elemType;
  pattern.basePtr = terms.basePtr;

  // 合并 offsetTerms
  if (!terms.offsetTerms.empty()) {
    pattern.baseOffset = mergeAddTerms(terms.offsetTerms);
  }

  // 提取 stride
  for (auto& strideTerm : terms.strideTerms) {
    // 保存原始 term
    pattern.strideTerms.push_back(strideTerm);

    auto multiplier = extractStrideMultiplier(strideTerm.get());
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
    ArrayRef<int64_t> shape, Type elemType,
    NormalizedTerms& terms, Operation* op) {
  TensorPattern pattern;
  pattern.kind = TensorPattern::Kind::Scalar;
  pattern.shape.append(shape.begin(), shape.end());
  pattern.elementType = elemType;
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

  // 获取 basePtr
  auto base = std::static_pointer_cast<ScalarSV>(
      std::const_pointer_cast<SymValue>(ptrExpr->getBasePtr()->shared_from_this()));
  if (isa<GmPtrSV>(base.get())) {
    pattern.basePtr = std::static_pointer_cast<GmPtrSV>(base);
  }

  // 对 offset 进行归一化分析
  auto offset = std::static_pointer_cast<ScalarSV>(ptrExpr->getOffset()->shared_from_this());

  // 传播 dims
  propagateDimsToRange(offset.get(), {});

  // 提升 SelectExpr
  auto hoisted = hoistSelectWithRange(offset);

  // 展开分配律
  auto expanded = expandDistribution(hoisted, 0);

  // 归一化
  NormalizedTerms terms = normalizeTerms(expanded.get());

  // 合并 offsetTerms 作为 baseOffset
  if (!terms.offsetTerms.empty()) {
    pattern.baseOffset = mergeAddTerms(terms.offsetTerms);
  }

  // strideTerms 作为 axisStrides（虽然 scalar 应该为空）
  for (auto& strideTerm : terms.strideTerms) {
    auto multiplier = extractStrideMultiplier(strideTerm.get());
    pattern.axisStrides.push_back(multiplier);
    pattern.isContinuous.push_back(false);
  }

  return pattern;
}

} // namespace ascend
} // namespace triton
} // namespace mlir
