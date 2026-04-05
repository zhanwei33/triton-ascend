/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */

#include "TritonToGraph/SymValue.h"
#include "mlir/IR/Types.h"
#include "llvm/Support/raw_ostream.h"

namespace mlir {
namespace triton {
namespace ascend {

//===----------------------------------------------------------------------===//
// SymValue
//===----------------------------------------------------------------------===//

bool SymValue::isScalar() const {
  return kind != Kind::Tensor;
}

bool SymValue::isTensor() const {
  return kind == Kind::Tensor;
}

//===----------------------------------------------------------------------===//
// ScalarConstantIntSV
//===----------------------------------------------------------------------===//

int64_t ScalarConstantIntSV::getInt() const {
  if (value.getBitWidth() <= 64) {
    return value.getSExtValue();
  }
  return value.trunc(64).getSExtValue();
}

void ScalarConstantIntSV::print(llvm::raw_ostream& os) const {
  os << "const.i" << value.getBitWidth() << "(" << getInt() << ")";
  if (isAssociated()) {
    os << "@[";
    for (size_t i = 0; i < dims.size(); ++i) {
      if (i > 0) os << ",";
      os << dims[i];
    }
    os << "]";
  }
}

//===----------------------------------------------------------------------===//
// ScalarConstantFloatSV
//===----------------------------------------------------------------------===//

void ScalarConstantFloatSV::print(llvm::raw_ostream& os) const {
  os << "const.f(" << getFloat() << ")";
  if (isAssociated()) {
    os << "@[";
    for (size_t i = 0; i < dims.size(); ++i) {
      if (i > 0) os << ",";
      os << dims[i];
    }
    os << "]";
  }
}

//===----------------------------------------------------------------------===//
// AddExprSV
//===----------------------------------------------------------------------===//

AddExprSV::AddExprSV(std::shared_ptr<ScalarSV> l, std::shared_ptr<ScalarSV> r, Type type)
    : ScalarSV(Kind::AddExpr), lhs(l), rhs(r), dataType(type) {
  // dims 默认为 [-1] (未关联)
  // 在 symbolic execution 时根据结果 tensor 的 shape 设置 dims
}

void AddExprSV::print(llvm::raw_ostream& os) const {
  os << "(";
  lhs->print(os);
  os << " + ";
  rhs->print(os);
  os << ")";
  if (isAssociated()) {
    os << "@[";
    for (size_t i = 0; i < dims.size(); ++i) {
      if (i > 0) os << ",";
      os << dims[i];
    }
    os << "]";
  }
}

//===----------------------------------------------------------------------===//
// SubExprSV
//===----------------------------------------------------------------------===//

SubExprSV::SubExprSV(std::shared_ptr<ScalarSV> l, std::shared_ptr<ScalarSV> r, Type type)
    : ScalarSV(Kind::SubExpr), lhs(l), rhs(r), dataType(type) {
  // dims 默认为 [-1] (未关联)
}

void SubExprSV::print(llvm::raw_ostream& os) const {
  os << "(";
  lhs->print(os);
  os << " - ";
  rhs->print(os);
  os << ")";
  if (isAssociated()) {
    os << "@[";
    for (size_t i = 0; i < dims.size(); ++i) {
      if (i > 0) os << ",";
      os << dims[i];
    }
    os << "]";
  }
}

//===----------------------------------------------------------------------===//
// MulExprSV
//===----------------------------------------------------------------------===//

MulExprSV::MulExprSV(std::shared_ptr<ScalarSV> l, std::shared_ptr<ScalarSV> r, Type type)
    : ScalarSV(Kind::MulExpr), lhs(l), rhs(r), dataType(type) {
  // dims 默认为 [-1] (未关联)
}

void MulExprSV::print(llvm::raw_ostream& os) const {
  os << "(";
  lhs->print(os);
  os << " * ";
  rhs->print(os);
  os << ")";
  if (isAssociated()) {
    os << "@[";
    for (size_t i = 0; i < dims.size(); ++i) {
      if (i > 0) os << ",";
      os << dims[i];
    }
    os << "]";
  }
}

//===----------------------------------------------------------------------===//
// DivExprSV
//===----------------------------------------------------------------------===//

DivExprSV::DivExprSV(std::shared_ptr<ScalarSV> l, std::shared_ptr<ScalarSV> r, Type type)
    : ScalarSV(Kind::DivExpr), lhs(l), rhs(r), dataType(type) {
  // dims 默认为 [-1] (未关联)
}

void DivExprSV::print(llvm::raw_ostream& os) const {
  os << "(";
  lhs->print(os);
  os << " / ";
  rhs->print(os);
  os << ")";
  if (isAssociated()) {
    os << "@[";
    for (size_t i = 0; i < dims.size(); ++i) {
      if (i > 0) os << ",";
      os << dims[i];
    }
    os << "]";
  }
}

//===----------------------------------------------------------------------===//
// RangeExprSV
//===----------------------------------------------------------------------===//

RangeExprSV::RangeExprSV(int64_t s, int64_t e, Type type)
    : ScalarSV(Kind::RangeExpr), start(s), end(e), dataType(type) {
  // make_range 产生的 range 默认维度为 0（1D）
  setDims({0});
}

void RangeExprSV::print(llvm::raw_ostream& os) const {
  os << "range[" << start << ", " << end << ")";
  if (isAssociated()) {
    os << "@[";
    for (size_t i = 0; i < dims.size(); ++i) {
      if (i > 0) os << ",";
      os << dims[i];
    }
    os << "]";
  }
}

//===----------------------------------------------------------------------===//
// CmpExprSV
//===----------------------------------------------------------------------===//

CmpExprSV::CmpExprSV(Pred p, std::shared_ptr<ScalarSV> l,
                     std::shared_ptr<ScalarSV> r, Type type)
    : ScalarSV(Kind::CmpExpr), pred(p), lhs(l), rhs(r), dataType(type) {
  // dims 默认为 [-1] (未关联)
}

void CmpExprSV::print(llvm::raw_ostream& os) const {
  os << "(";
  lhs->print(os);
  switch (pred) {
    case Pred::EQ: os << " == "; break;
    case Pred::NE: os << " != "; break;
    case Pred::LT: os << " < "; break;
    case Pred::LE: os << " <= "; break;
    case Pred::GT: os << " > "; break;
    case Pred::GE: os << " >= "; break;
  }
  rhs->print(os);
  os << ")";
  if (isAssociated()) {
    os << "@[";
    for (size_t i = 0; i < dims.size(); ++i) {
      if (i > 0) os << ",";
      os << dims[i];
    }
    os << "]";
  }
}

//===----------------------------------------------------------------------===//
// SelectExprSV
//===----------------------------------------------------------------------===//

SelectExprSV::SelectExprSV(std::shared_ptr<CmpExprSV> cond,
                           std::shared_ptr<ScalarSV> t,
                           std::shared_ptr<ScalarSV> f, Type type)
    : ScalarSV(Kind::SelectExpr), condition(cond),
      trueVal(t), falseVal(f), dataType(type) {
  // dims 默认为 [-1] (未关联)
}

bool SelectExprSV::isMinPattern() const {
  // select(cmp_lt(x, y), x, y) -> min(x, y)
  if (!condition) return false;
  if (condition->getPred() != CmpExprSV::Pred::LT &&
      condition->getPred() != CmpExprSV::Pred::LE) {
    return false;
  }
  // 检查 trueVal 是否等于 condition 的 lhs
  // 检查 falseVal 是否等于 condition 的 rhs
  // 简化实现：比较指针
  return condition->getLHS() == trueVal.get() &&
         condition->getRHS() == falseVal.get();
}

bool SelectExprSV::isMaxPattern() const {
  // select(cmp_gt(x, y), x, y) -> max(x, y)
  if (!condition) return false;
  if (condition->getPred() != CmpExprSV::Pred::GT &&
      condition->getPred() != CmpExprSV::Pred::GE) {
    return false;
  }
  return condition->getLHS() == trueVal.get() &&
         condition->getRHS() == falseVal.get();
}

bool SelectExprSV::isLengthCheck() const {
  // select(cmp_lt(idx, bound), idx, const) -> 长度检测模式
  if (!condition) return false;
  if (condition->getPred() != CmpExprSV::Pred::LT &&
      condition->getPred() != CmpExprSV::Pred::LE) {
    return false;
  }
  // trueVal 应该是 condition 的 lhs (idx)
  if (condition->getLHS() != trueVal.get()) return false;
  // falseVal 应该是常量 0 或其他默认值
  if (auto* c = dyn_cast<ScalarConstantIntSV>(falseVal.get())) {
    return c->getInt() == 0;
  }
  return false;
}

void SelectExprSV::print(llvm::raw_ostream& os) const {
  os << "select(";
  condition->print(os);
  os << " ? ";
  trueVal->print(os);
  os << " : ";
  falseVal->print(os);
  os << ")";
  if (isMinPattern()) os << "[min]";
  if (isMaxPattern()) os << "[max]";
  if (isLengthCheck()) os << "[len-check]";
  if (isAssociated()) {
    os << "@[";
    for (size_t i = 0; i < dims.size(); ++i) {
      if (i > 0) os << ",";
      os << dims[i];
    }
    os << "]";
  }
}

//===----------------------------------------------------------------------===//
// PtrExprSV
//===----------------------------------------------------------------------===//

PtrExprSV::PtrExprSV(std::shared_ptr<ScalarSV> base,
                     std::shared_ptr<ScalarSV> off,
                     Type pt)
    : ScalarSV(Kind::PtrExpr), basePtr(base),
      offset(off), pointeeType(pt) {
  // dims 默认为 [-1] (未关联)
}

std::shared_ptr<AddExprSV> PtrExprSV::computeTotalOffset() const {
  // basePtr + offset
  return std::make_shared<AddExprSV>(basePtr, offset,
      IntegerType::get(pointeeType.getContext(), 64));
}

void PtrExprSV::print(llvm::raw_ostream& os) const {
  os << "ptr(";
  basePtr->print(os);
  os << " + ";
  offset->print(os);
  os << ")";
  if (isAssociated()) {
    os << "@[";
    for (size_t i = 0; i < dims.size(); ++i) {
      if (i > 0) os << ",";
      os << dims[i];
    }
    os << "]";
  }
}

//===----------------------------------------------------------------------===//
// TensorPtrSV
//===----------------------------------------------------------------------===//

void TensorPtrSV::print(llvm::raw_ostream& os) const {
  os << "tensorptr<" << pointeeType << ">";
  os << "[shape=";
  for (size_t i = 0; i < shape.size(); ++i) {
    if (i > 0) os << "x";
    if (shape[i]) shape[i]->print(os);
    else os << "?";
  }
  os << ",strides=";
  for (size_t i = 0; i < strides.size(); ++i) {
    if (i > 0) os << "x";
    if (strides[i]) strides[i]->print(os);
    else os << "?";
  }
  os << ",offsets=";
  for (size_t i = 0; i < offsets.size(); ++i) {
    if (i > 0) os << "x";
    if (offsets[i]) offsets[i]->print(os);
    else os << "?";
  }
  os << ",block=";
  for (size_t i = 0; i < blockShape.size(); ++i) {
    if (i > 0) os << "x";
    os << blockShape[i];
  }
  os << "]";
  if (isAssociated()) {
    os << "@[";
    for (size_t i = 0; i < dims.size(); ++i) {
      if (i > 0) os << ",";
      os << dims[i];
    }
    os << "]";
  }
}

//===----------------------------------------------------------------------===//
// ProgramIDSV
//===----------------------------------------------------------------------===//

void ProgramIDSV::print(llvm::raw_ostream& os) const {
  os << "pid." << (axis == 0 ? "x" : (axis == 1 ? "y" : "z"));
  if (isAssociated()) {
    os << "@[";
    for (size_t i = 0; i < dims.size(); ++i) {
      if (i > 0) os << ",";
      os << dims[i];
    }
    os << "]";
  }
}

//===----------------------------------------------------------------------===//
// GmPtrSV
//===----------------------------------------------------------------------===//

void GmPtrSV::print(llvm::raw_ostream& os) const {
  os << "gmptr<" << pointeeType << ">";
  if (param) {
    os << "(" << param << ")";
  }
  if (isAssociated()) {
    os << "@[";
    for (size_t i = 0; i < dims.size(); ++i) {
      if (i > 0) os << ",";
      os << dims[i];
    }
    os << "]";
  }
}

//===----------------------------------------------------------------------===//
// RemExprSV
//===----------------------------------------------------------------------===//

RemExprSV::RemExprSV(std::shared_ptr<ScalarSV> l, std::shared_ptr<ScalarSV> r, Type type)
    : ScalarSV(Kind::RemExpr), lhs(l), rhs(r), dataType(type) {
  // dims 默认为 [-1] (未关联)
}

void RemExprSV::print(llvm::raw_ostream& os) const {
  os << "(";
  lhs->print(os);
  os << " % ";
  rhs->print(os);
  os << ")";
  if (isAssociated()) {
    os << "@[";
    for (size_t i = 0; i < dims.size(); ++i) {
      if (i > 0) os << ",";
      os << dims[i];
    }
    os << "]";
  }
}

//===----------------------------------------------------------------------===//
// UnknownSV
//===----------------------------------------------------------------------===//

void UnknownSV::print(llvm::raw_ostream& os) const {
  os << "unknown<" << dataType << ">";
  if (isAssociated()) {
    os << "@[";
    for (size_t i = 0; i < dims.size(); ++i) {
      if (i > 0) os << ",";
      os << dims[i];
    }
    os << "]";
  }
}

//===----------------------------------------------------------------------===//
// InductionSV
//===----------------------------------------------------------------------===//

void InductionSV::print(llvm::raw_ostream& os) const {
  os << "induction[";
  if (init) init->print(os);
  else os << "?";
  os << "..";
  if (end) end->print(os);
  else os << "?";
  os << " step ";
  if (step) step->print(os);
  else os << "?";
  os << "]";
  if (isAssociated()) {
    os << "@[";
    for (size_t i = 0; i < dims.size(); ++i) {
      if (i > 0) os << ",";
      os << dims[i];
    }
    os << "]";
  }
}

//===----------------------------------------------------------------------===//
// IterArgSV
//===----------------------------------------------------------------------===//

void IterArgSV::print(llvm::raw_ostream& os) const {
  os << "iterarg<" << dataType << ">";
  if (isAssociated()) {
    os << "@[";
    for (size_t i = 0; i < dims.size(); ++i) {
      if (i > 0) os << ",";
      os << dims[i];
    }
    os << "]";
  }
}

//===----------------------------------------------------------------------===//
// TensorSV
//===----------------------------------------------------------------------===//

std::shared_ptr<TensorSV> TensorSV::createMakeRange(
    int64_t start, int64_t end, Type elemType) {
  auto tensor = std::make_shared<TensorSV>(
      SourceKind::MakeRange, SmallVector<int64_t>{end - start}, elemType);
  tensor->elementExpr = std::make_shared<RangeExprSV>(start, end, elemType);
  // RangeExprSV 的 dims 已经是 {0}
  return tensor;
}

std::shared_ptr<TensorSV> TensorSV::createSplat(
    std::shared_ptr<ScalarSV> val,
    ArrayRef<int64_t> shape, Type elemType) {
  auto tensor = std::make_shared<TensorSV>(
      SourceKind::Splat, shape, elemType);
  // 设置 elementExpr 的 dims 为所有维度
  SmallVector<int> elemDims;
  for (size_t i = 0; i < shape.size(); ++i) {
    elemDims.push_back(i);
  }
  val->setDims(elemDims);
  tensor->elementExpr = std::move(val);
  return tensor;
}

std::shared_ptr<TensorSV> TensorSV::createExpandDims(
    const TensorSV* input, int axis) {
  if (!input) return nullptr;

  SmallVector<int64_t> newShape;
  // 复制 axis 之前的维度
  for (size_t i = 0; i < static_cast<size_t>(axis); ++i) {
    newShape.push_back(input->shape[i]);
  }
  // 插入新维度（大小为1）
  newShape.push_back(1);
  // 复制 axis 及之后的原维度
  for (size_t i = axis; i < input->shape.size(); ++i) {
    newShape.push_back(input->shape[i]);
  }

  auto tensor = std::make_shared<TensorSV>(
      SourceKind::ExpandDims, newShape, input->elementType);

  // 复制 elementExpr 并调整 dims
  // expand_dims(axis)：被扩展的维度不保持原值，其他维度保持
  // 例如原 dims=[0]，expand_dims(0) -> 新 dims=[1]（原0维变为1维）
  if (input->elementExpr) {
    // 克隆 elementExpr（简化实现：直接使用原值）
    tensor->elementExpr = input->elementExpr;
    // 更新 dims：原维度0变为维度1
    SmallVector<int> newDims;
    for (int d : input->elementExpr->dims) {
      if (d >= axis) {
        newDims.push_back(d + 1);
      } else {
        newDims.push_back(d);
      }
    }
    tensor->elementExpr->setDims(newDims);
  }

  return tensor;
}

std::shared_ptr<TensorSV> TensorSV::createBroadcast(
    const TensorSV* input, ArrayRef<int64_t> newShape) {
  if (!input) return nullptr;

  auto tensor = std::make_shared<TensorSV>(
      SourceKind::Broadcast, newShape, input->elementType);

  // broadcast 保持 dims 不变
  if (input->elementExpr) {
    tensor->elementExpr = input->elementExpr;
  }

  return tensor;
}

std::shared_ptr<TensorSV> TensorSV::createComputed(
    SourceKind op, const TensorSV* lhs, const TensorSV* rhs) {
  if (!lhs || !rhs) return nullptr;

  // 简化：假设形状相同
  auto tensor = std::make_shared<TensorSV>(
      op, lhs->shape, lhs->elementType);

  // 创建元素级运算表达式
  if (lhs->elementExpr && rhs->elementExpr) {
    std::shared_ptr<ScalarSV> elemExpr;
    Type elemType = lhs->elementType;

    switch (op) {
      // 四则运算
      case SourceKind::Add:
      case SourceKind::Computed:  // 默认使用 Add 保持向后兼容
        elemExpr = std::make_shared<AddExprSV>(
            lhs->elementExpr, rhs->elementExpr, elemType);
        break;
      case SourceKind::Sub:
        elemExpr = std::make_shared<SubExprSV>(
            lhs->elementExpr, rhs->elementExpr, elemType);
        break;
      case SourceKind::Mul:
        elemExpr = std::make_shared<MulExprSV>(
            lhs->elementExpr, rhs->elementExpr, elemType);
        break;
      case SourceKind::Div:
        elemExpr = std::make_shared<DivExprSV>(
            lhs->elementExpr, rhs->elementExpr, elemType);
        break;
      case SourceKind::Rem:
        elemExpr = std::make_shared<RemExprSV>(
            lhs->elementExpr, rhs->elementExpr, elemType);
        break;
      // 比较运算
      case SourceKind::CmpEQ:
        elemExpr = std::make_shared<CmpExprSV>(
            CmpExprSV::Pred::EQ, lhs->elementExpr, rhs->elementExpr, elemType);
        break;
      case SourceKind::CmpNE:
        elemExpr = std::make_shared<CmpExprSV>(
            CmpExprSV::Pred::NE, lhs->elementExpr, rhs->elementExpr, elemType);
        break;
      case SourceKind::CmpLT:
        elemExpr = std::make_shared<CmpExprSV>(
            CmpExprSV::Pred::LT, lhs->elementExpr, rhs->elementExpr, elemType);
        break;
      case SourceKind::CmpLE:
        elemExpr = std::make_shared<CmpExprSV>(
            CmpExprSV::Pred::LE, lhs->elementExpr, rhs->elementExpr, elemType);
        break;
      case SourceKind::CmpGT:
        elemExpr = std::make_shared<CmpExprSV>(
            CmpExprSV::Pred::GT, lhs->elementExpr, rhs->elementExpr, elemType);
        break;
      case SourceKind::CmpGE:
        elemExpr = std::make_shared<CmpExprSV>(
            CmpExprSV::Pred::GE, lhs->elementExpr, rhs->elementExpr, elemType);
        break;
      default:
        // 不支持的运算类型，保持 elementExpr 为空
        break;
    }

    tensor->elementExpr = std::move(elemExpr);

    // 设置 dims 为完整维度
    if (tensor->elementExpr) {
      SmallVector<int> elemDims;
      for (size_t i = 0; i < lhs->shape.size(); ++i) {
        elemDims.push_back(i);
      }
      tensor->elementExpr->setDims(elemDims);
    }
  }

  return tensor;
}

std::shared_ptr<TensorSV> TensorSV::createLoad(
    ArrayRef<int64_t> shape, Type elemType) {
  auto tensor = std::make_shared<TensorSV>(
      SourceKind::Load, shape, elemType);

  // elementExpr 为 UnknownSV（load 产生的结果是未知的）
  tensor->elementExpr = std::make_shared<UnknownSV>(elemType);

  // 设置 dims 为完整维度
  SmallVector<int> elemDims;
  for (size_t i = 0; i < shape.size(); ++i) {
    elemDims.push_back(i);
  }
  tensor->elementExpr->setDims(elemDims);

  return tensor;
}

void TensorSV::updateElementDims() {
  if (!elementExpr) return;

  SmallVector<int> newDims;
  for (size_t i = 0; i < shape.size(); ++i) {
    newDims.push_back(i);
  }
  elementExpr->setDims(newDims);
}

void TensorSV::print(llvm::raw_ostream& os) const {
  os << "Tensor[";
  for (size_t i = 0; i < shape.size(); ++i) {
    if (i > 0) os << "x";
    os << shape[i];
  }
  os << "]";

  switch (source) {
    case SourceKind::MakeRange: os << ":make_range"; break;
    case SourceKind::Splat: os << ":splat"; break;
    case SourceKind::ExpandDims: os << ":expand_dims"; break;
    case SourceKind::Broadcast: os << ":broadcast"; break;
    // 四则运算
    case SourceKind::Add: os << ":add"; break;
    case SourceKind::Sub: os << ":sub"; break;
    case SourceKind::Mul: os << ":mul"; break;
    case SourceKind::Div: os << ":div"; break;
    case SourceKind::Rem: os << ":rem"; break;
    // 比较运算
    case SourceKind::CmpEQ: os << ":cmp_eq"; break;
    case SourceKind::CmpNE: os << ":cmp_ne"; break;
    case SourceKind::CmpLT: os << ":cmp_lt"; break;
    case SourceKind::CmpLE: os << ":cmp_le"; break;
    case SourceKind::CmpGT: os << ":cmp_gt"; break;
    case SourceKind::CmpGE: os << ":cmp_ge"; break;
    // 选择运算
    case SourceKind::Select: os << ":select"; break;
    // Load 运算
    case SourceKind::Load: os << ":load"; break;
    // 通用/向后兼容
    case SourceKind::Computed: os << ":computed"; break;
  }

  if (elementExpr) {
    os << "{elem=";
    elementExpr->print(os);
    os << "}";
  }
}

TensorSV::TensorSV(SourceKind src, ArrayRef<int64_t> s, Type elemType)
    : SymValue(Kind::Tensor), source(src), elementType(elemType) {
  shape.append(s.begin(), s.end());
}

//===----------------------------------------------------------------------===//
// 辅助函数
//===----------------------------------------------------------------------===//

bool isScalar(const SymValue* v) { return v && v->isScalar(); }
bool isTensor(const SymValue* v) { return v && v->isTensor(); }

llvm::Optional<int64_t> getConstantInt(const SymValue* sv) {
  if (auto* ci = dyn_cast<ScalarConstantIntSV>(sv)) {
    return ci->getInt();
  }
  return llvm::None;
}

llvm::Optional<double> getConstantFloat(const SymValue* sv) {
  if (auto* cf = dyn_cast<ScalarConstantFloatSV>(sv)) {
    return cf->getFloat();
  }
  return llvm::None;
}

} // namespace ascend
} // namespace triton
} // namespace mlir
