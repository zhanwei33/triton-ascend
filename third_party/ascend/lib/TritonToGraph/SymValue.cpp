/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */

#include "TritonToGraph/SymValue.h"
#include "mlir/IR/Types.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Operation.h"
#include "llvm/Support/raw_ostream.h"
#include <optional>
#include <sstream>

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

/// 获取 Operation 的单行字符串表示
std::string SymValue::getOperationStr(Operation* op) {
  if (!op) return "";

  std::string str;
  llvm::raw_string_ostream rss(str);
  op->print(rss);

  // 只取第一行
  size_t newlinePos = str.find('\n');
  if (newlinePos != std::string::npos) {
    str = str.substr(0, newlinePos);
  }

  // 截断过长的字符串
  const size_t maxLen = 80;
  if (str.length() > maxLen) {
    str = str.substr(0, maxLen - 3) + "...";
  }

  return str;
}

void SymValue::printIndent(llvm::raw_ostream& os, unsigned indent) {
  for (unsigned i = 0; i < indent; ++i) os << "  ";
}

/// 打印 operation 信息（右对齐）
void SymValue::printOperationInfo(llvm::raw_ostream& os, Operation* op,
                                   unsigned currentIndent) {
  if (!op) return;

  std::string opStr = getOperationStr(op);
  if (opStr.empty()) return;

  // 计算当前行长度（假设每级缩进2字符）
  size_t currentPos = currentIndent * 2 + 40;  // 估算当前内容长度

  // 对齐到右边（假设总宽度 100）
  const unsigned totalWidth = 100;
  if (currentPos < totalWidth - opStr.length()) {
    for (size_t i = currentPos; i < totalWidth - opStr.length(); ++i) {
      os << " ";
    }
  } else {
    os << "  ";
  }
  os << "// " << opStr;
}

//===----------------------------------------------------------------------===//
// ScalarSV - 基类辅助方法
//===----------------------------------------------------------------------===//

void ScalarSV::printDims(llvm::raw_ostream& os) const {
  if (!isAssociated()) return;
  os << " @dims[";
  for (size_t i = 0; i < dims.size(); ++i) {
    if (i > 0) os << ",";
    os << dims[i];
  }
  os << "]";
}

void ScalarSV::printWithOp(llvm::raw_ostream& os, unsigned indent,
                            const std::string& content) const {
  printIndent(os, indent);
  os << content;
  printDims(os);
  printOperationInfo(os, sourceOp, indent);
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
  printDims(os);
  printOperationInfo(os, sourceOp, 0);
}

void ScalarConstantIntSV::print(llvm::raw_ostream& os, unsigned indent) const {
  std::string content = "const.i" + std::to_string(value.getBitWidth()) +
                        "(" + std::to_string(getInt()) + ")";
  printWithOp(os, indent, content);
}

//===----------------------------------------------------------------------===//
// ScalarConstantFloatSV
//===----------------------------------------------------------------------===//

double ScalarConstantFloatSV::getFloat() const {
  return value.convertToDouble();
}

void ScalarConstantFloatSV::print(llvm::raw_ostream& os) const {
  os << "const.f(" << getFloat() << ")";
  printDims(os);
  printOperationInfo(os, sourceOp, 0);
}

void ScalarConstantFloatSV::print(llvm::raw_ostream& os, unsigned indent) const {
  std::string content = "const.f(" + std::to_string(getFloat()) + ")";
  printWithOp(os, indent, content);
}

//===----------------------------------------------------------------------===//
// AddExprSV
//===----------------------------------------------------------------------===//

AddExprSV::AddExprSV(std::shared_ptr<ScalarSV> l, std::shared_ptr<ScalarSV> r,
                     Type type, Operation* op)
    : ScalarSV(Kind::AddExpr, op), lhs(l), rhs(r), dataType(type) {
  // dims 默认为 [-1] (未关联)
  // 在 symbolic execution 时根据结果 tensor 的 shape 设置 dims
}

void AddExprSV::print(llvm::raw_ostream& os) const {
  os << "Add<" << dataType << ">";
  printDims(os);
  printOperationInfo(os, sourceOp, 0);
  os << "\n";

  printIndent(os, 1);
  os << "lhs:\n";
  lhs->print(os, 2);

  os << "\n";
  printIndent(os, 1);
  os << "rhs:\n";
  rhs->print(os, 2);
}

void AddExprSV::print(llvm::raw_ostream& os, unsigned indent) const {
  printIndent(os, indent);
  os << "Add<" << dataType << ">";
  printDims(os);
  printOperationInfo(os, sourceOp, indent);
  os << "\n";

  printIndent(os, indent + 1);
  os << "lhs:\n";
  lhs->print(os, indent + 2);

  os << "\n";
  printIndent(os, indent + 1);
  os << "rhs:\n";
  rhs->print(os, indent + 2);
}

//===----------------------------------------------------------------------===//
// SubExprSV
//===----------------------------------------------------------------------===//

SubExprSV::SubExprSV(std::shared_ptr<ScalarSV> l, std::shared_ptr<ScalarSV> r,
                     Type type, Operation* op)
    : ScalarSV(Kind::SubExpr, op), lhs(l), rhs(r), dataType(type) {
  // dims 默认为 [-1] (未关联)
}

void SubExprSV::print(llvm::raw_ostream& os) const {
  os << "Sub<" << dataType << ">";
  printDims(os);
  printOperationInfo(os, sourceOp, 0);
  os << "\n";

  printIndent(os, 1);
  os << "lhs:\n";
  lhs->print(os, 2);

  os << "\n";
  printIndent(os, 1);
  os << "rhs:\n";
  rhs->print(os, 2);
}

void SubExprSV::print(llvm::raw_ostream& os, unsigned indent) const {
  printIndent(os, indent);
  os << "Sub<" << dataType << ">";
  printDims(os);
  printOperationInfo(os, sourceOp, indent);
  os << "\n";

  printIndent(os, indent + 1);
  os << "lhs:\n";
  lhs->print(os, indent + 2);

  os << "\n";
  printIndent(os, indent + 1);
  os << "rhs:\n";
  rhs->print(os, indent + 2);
}

//===----------------------------------------------------------------------===//
// MulExprSV
//===----------------------------------------------------------------------===//

MulExprSV::MulExprSV(std::shared_ptr<ScalarSV> l, std::shared_ptr<ScalarSV> r,
                     Type type, Operation* op)
    : ScalarSV(Kind::MulExpr, op), lhs(l), rhs(r), dataType(type) {
  // dims 默认为 [-1] (未关联)
}

void MulExprSV::print(llvm::raw_ostream& os) const {
  os << "Mul<" << dataType << ">";
  printDims(os);
  printOperationInfo(os, sourceOp, 0);
  os << "\n";

  printIndent(os, 1);
  os << "lhs:\n";
  lhs->print(os, 2);

  os << "\n";
  printIndent(os, 1);
  os << "rhs:\n";
  rhs->print(os, 2);
}

void MulExprSV::print(llvm::raw_ostream& os, unsigned indent) const {
  printIndent(os, indent);
  os << "Mul<" << dataType << ">";
  printDims(os);
  printOperationInfo(os, sourceOp, indent);
  os << "\n";

  printIndent(os, indent + 1);
  os << "lhs:\n";
  lhs->print(os, indent + 2);

  os << "\n";
  printIndent(os, indent + 1);
  os << "rhs:\n";
  rhs->print(os, indent + 2);
}

//===----------------------------------------------------------------------===//
// DivExprSV
//===----------------------------------------------------------------------===//

DivExprSV::DivExprSV(std::shared_ptr<ScalarSV> l, std::shared_ptr<ScalarSV> r,
                     Type type, Operation* op)
    : ScalarSV(Kind::DivExpr, op), lhs(l), rhs(r), dataType(type) {
  // dims 默认为 [-1] (未关联)
}

void DivExprSV::print(llvm::raw_ostream& os) const {
  os << "Div<" << dataType << ">";
  printDims(os);
  printOperationInfo(os, sourceOp, 0);
  os << "\n";

  printIndent(os, 1);
  os << "lhs:\n";
  lhs->print(os, 2);

  os << "\n";
  printIndent(os, 1);
  os << "rhs:\n";
  rhs->print(os, 2);
}

void DivExprSV::print(llvm::raw_ostream& os, unsigned indent) const {
  printIndent(os, indent);
  os << "Div<" << dataType << ">";
  printDims(os);
  printOperationInfo(os, sourceOp, indent);
  os << "\n";

  printIndent(os, indent + 1);
  os << "lhs:\n";
  lhs->print(os, indent + 2);

  os << "\n";
  printIndent(os, indent + 1);
  os << "rhs:\n";
  rhs->print(os, indent + 2);
}

//===----------------------------------------------------------------------===//
// RangeExprSV
//===----------------------------------------------------------------------===//

RangeExprSV::RangeExprSV(int64_t s, int64_t e, Type type, Operation* op)
    : ScalarSV(Kind::RangeExpr, op), start(s), end(e), dataType(type) {
  // make_range 产生的 range 默认维度为 0（1D）
  setDims({0});
}

void RangeExprSV::print(llvm::raw_ostream& os) const {
  os << "Range[" << start << ", " << end << ")";
  printDims(os);
  printOperationInfo(os, sourceOp, 0);
}

void RangeExprSV::print(llvm::raw_ostream& os, unsigned indent) const {
  std::string content = "Range[" + std::to_string(start) + ", " +
                        std::to_string(end) + ")";
  printWithOp(os, indent, content);
}

//===----------------------------------------------------------------------===//
// CmpExprSV
//===----------------------------------------------------------------------===//

CmpExprSV::CmpExprSV(Pred p, std::shared_ptr<ScalarSV> l,
                     std::shared_ptr<ScalarSV> r, Type type, Operation* op)
    : ScalarSV(Kind::CmpExpr, op), pred(p), lhs(l), rhs(r), dataType(type) {
  // dims 默认为 [-1] (未关联)
}

const char* CmpExprSV::getPredStr() const {
  switch (pred) {
    case Pred::EQ: return "EQ";
    case Pred::NE: return "NE";
    case Pred::LT: return "LT";
    case Pred::LE: return "LE";
    case Pred::GT: return "GT";
    case Pred::GE: return "GE";
  }
  return "?";
}

void CmpExprSV::print(llvm::raw_ostream& os) const {
  os << "Cmp" << getPredStr() << "<" << dataType << ">";
  printDims(os);
  printOperationInfo(os, sourceOp, 0);
  os << "\n";

  printIndent(os, 1);
  os << "lhs:\n";
  lhs->print(os, 2);

  os << "\n";
  printIndent(os, 1);
  os << "rhs:\n";
  rhs->print(os, 2);
}

void CmpExprSV::print(llvm::raw_ostream& os, unsigned indent) const {
  printIndent(os, indent);
  os << "Cmp" << getPredStr() << "<" << dataType << ">";
  printDims(os);
  printOperationInfo(os, sourceOp, indent);
  os << "\n";

  printIndent(os, indent + 1);
  os << "lhs:\n";
  lhs->print(os, indent + 2);

  os << "\n";
  printIndent(os, indent + 1);
  os << "rhs:\n";
  rhs->print(os, indent + 2);
}

//===----------------------------------------------------------------------===//
// SelectExprSV
//===----------------------------------------------------------------------===//

SelectExprSV::SelectExprSV(std::shared_ptr<CmpExprSV> cond,
                           std::shared_ptr<ScalarSV> t,
                           std::shared_ptr<ScalarSV> f, Type type,
                           Operation* op)
    : ScalarSV(Kind::SelectExpr, op), condition(cond),
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
  // Min/Max 模式打印为函数调用格式
  if (isMinPattern()) {
    os << "Min(";
    trueVal->print(os);
    os << ", ";
    falseVal->print(os);
    os << ")";
    printDims(os);
    printOperationInfo(os, sourceOp, 0);
  } else if (isMaxPattern()) {
    os << "Max(";
    trueVal->print(os);
    os << ", ";
    falseVal->print(os);
    os << ")";
    printDims(os);
    printOperationInfo(os, sourceOp, 0);
  } else if (isLengthCheck()) {
    os << "ClampToZero(";
    trueVal->print(os);
    os << ", ";
    if (condition) {
      condition->getRHS()->print(os);
    }
    os << ")";
    printDims(os);
    printOperationInfo(os, sourceOp, 0);
  } else {
    // 默认格式: select(cond) ? trueVal : falseVal
    os << "select(";
    if (condition) {
      condition->print(os);
    } else {
      os << "null";
    }
    os << ") ? ";
    trueVal->print(os);
    os << " : ";
    falseVal->print(os);
    printDims(os);
    printOperationInfo(os, sourceOp, 0);
  }
}

void SelectExprSV::print(llvm::raw_ostream& os, unsigned indent) const {
  printIndent(os, indent);
  // Min/Max 模式打印为函数调用格式
  bool bPattern = false;
  std::string pat;

  if(isMinPattern())
  {
    bPattern = true;
    pat = "Min";
  }
  else if (isMaxPattern())
  {
    bPattern = true;
    pat = "Max";
  }
  else if(isLengthCheck())
  {
    bPattern = true;
    pat = "ClampToZero";
  }

  if(bPattern)
  {
    os << pat << "( ";
    printDims(os);
    printOperationInfo(os, sourceOp, indent);
    os << "\n";

    trueVal->print(os, indent+1);

    os << "\n";
    printIndent(os, indent);
    os << ", \n";
    
    if (condition) {
      condition->getRHS()->print(os, indent+1);
      os << "\n";
    }

    printIndent(os, indent);
    os << ")";
  }
  else 
  {
    // 默认格式: select(cond) ? trueVal : falseVal
    os << "select( ";
    printDims(os);
    printOperationInfo(os, sourceOp, indent); 
    os << "\n";

    if (condition) {
      condition->print(os, indent+1);
    } else {
      os << "null";
    }
    
    printIndent(os, indent);
    os << ") ? \n ";
    trueVal->print(os, indent+1);
    
    printIndent(os, indent);
    os << " : \n";
    
    falseVal->print(os, indent+1);
  }
}

//===----------------------------------------------------------------------===//
// PtrExprSV
//===----------------------------------------------------------------------===//

PtrExprSV::PtrExprSV(std::shared_ptr<ScalarSV> base,
                     std::shared_ptr<ScalarSV> off,
                     Type pt, Operation* op)
    : ScalarSV(Kind::PtrExpr, op), basePtr(base),
      offset(off), pointeeType(pt) {
  // dims 默认为 [-1] (未关联)
}

std::shared_ptr<AddExprSV> PtrExprSV::computeTotalOffset() const {
  // basePtr + offset
  return std::make_shared<AddExprSV>(basePtr, offset,
      IntegerType::get(pointeeType.getContext(), 64));
}

void PtrExprSV::print(llvm::raw_ostream& os) const {
  os << "PtrExpr<" << pointeeType << ">";
  printDims(os);
  printOperationInfo(os, sourceOp, 0);
  os << "\n";

  printIndent(os, 1);
  os << "base:\n";
  basePtr->print(os, 2);

  os << "\n";
  printIndent(os, 1);
  os << "offset:\n";
  offset->print(os, 2);
}

void PtrExprSV::print(llvm::raw_ostream& os, unsigned indent) const {
  printIndent(os, indent);
  os << "PtrExpr<" << pointeeType << ">";
  printDims(os);
  printOperationInfo(os, sourceOp, indent);
  os << "\n";

  printIndent(os, indent + 1);
  os << "base:\n";
  basePtr->print(os, indent + 2);

  os << "\n";
  printIndent(os, indent + 1);
  os << "offset:\n";
  offset->print(os, indent + 2);
}

//===----------------------------------------------------------------------===//
// TensorPtrSV
//===----------------------------------------------------------------------===//

void TensorPtrSV::print(llvm::raw_ostream& os) const {
  os << "TensorPtr<" << pointeeType << ">";
  printDims(os);
  printOperationInfo(os, sourceOp, 0);
  os << "\n";

  printIndent(os, 1);
  os << "shape=[";
  for (size_t i = 0; i < shape.size(); ++i) {
    if (i > 0) os << "x";
    if (shape[i]) shape[i]->print(os);
    else os << "?";
  }
  os << "]\n";

  printIndent(os, 1);
  os << "strides=[";
  for (size_t i = 0; i < strides.size(); ++i) {
    if (i > 0) os << "x";
    if (strides[i]) strides[i]->print(os);
    else os << "?";
  }
  os << "]\n";

  printIndent(os, 1);
  os << "offsets=[";
  for (size_t i = 0; i < offsets.size(); ++i) {
    if (i > 0) os << "x";
    if (offsets[i]) offsets[i]->print(os);
    else os << "?";
  }
  os << "]\n";

  printIndent(os, 1);
  os << "block=[";
  for (size_t i = 0; i < blockShape.size(); ++i) {
    if (i > 0) os << "x";
    os << blockShape[i];
  }
  os << "]";
}

void TensorPtrSV::print(llvm::raw_ostream& os, unsigned indent) const {
  printIndent(os, indent);
  os << "TensorPtr<" << pointeeType << ">";
  printDims(os);
  printOperationInfo(os, sourceOp, indent);
  os << "\n";

  printIndent(os, indent + 1);
  os << "shape=[";
  for (size_t i = 0; i < shape.size(); ++i) {
    if (i > 0) os << "x";
    if (shape[i]) shape[i]->print(os);
    else os << "?";
  }
  os << "]\n";

  printIndent(os, indent + 1);
  os << "strides=[";
  for (size_t i = 0; i < strides.size(); ++i) {
    if (i > 0) os << "x";
    if (strides[i]) strides[i]->print(os);
    else os << "?";
  }
  os << "]\n";

  printIndent(os, indent + 1);
  os << "offsets=[";
  for (size_t i = 0; i < offsets.size(); ++i) {
    if (i > 0) os << "x";
    if (offsets[i]) offsets[i]->print(os);
    else os << "?";
  }
  os << "]\n";

  printIndent(os, indent + 1);
  os << "block=[";
  for (size_t i = 0; i < blockShape.size(); ++i) {
    if (i > 0) os << "x";
    os << blockShape[i];
  }
  os << "]";
}

//===----------------------------------------------------------------------===//
// ProgramIDSV
//===----------------------------------------------------------------------===//

void ProgramIDSV::print(llvm::raw_ostream& os) const {
  os << "pid." << (axis == 0 ? "x" : (axis == 1 ? "y" : "z"));
  printDims(os);
  printOperationInfo(os, sourceOp, 0);
}

void ProgramIDSV::print(llvm::raw_ostream& os, unsigned indent) const {
  std::string content = "pid." + std::string(axis == 0 ? "x" : (axis == 1 ? "y" : "z"));
  printWithOp(os, indent, content);
}

//===----------------------------------------------------------------------===//
// GmPtrSV
//===----------------------------------------------------------------------===//

void GmPtrSV::print(llvm::raw_ostream& os) const {
  os << "GmPtr<" << pointeeType << ">";
  if (param) {
    os << "(arg" << dyn_cast<BlockArgument>(param).getArgNumber() << ")";
  }
  printDims(os);
  printOperationInfo(os, sourceOp, 0);
}

void GmPtrSV::print(llvm::raw_ostream& os, unsigned indent) const {
  std::string content;
  llvm::raw_string_ostream rso(content);

  rso << "GmPtr<" << pointeeType << ">";
  if (param) {
    rso << "(arg" << dyn_cast<BlockArgument>(param).getArgNumber() << ")";
  }
  rso.flush();  // 确保写入 string
  
  printWithOp(os, indent, content);
}

//===----------------------------------------------------------------------===//
// RemExprSV
//===----------------------------------------------------------------------===//

RemExprSV::RemExprSV(std::shared_ptr<ScalarSV> l, std::shared_ptr<ScalarSV> r,
                     Type type, Operation* op)
    : ScalarSV(Kind::RemExpr, op), lhs(l), rhs(r), dataType(type) {
  // dims 默认为 [-1] (未关联)
}

void RemExprSV::print(llvm::raw_ostream& os) const {
  os << "Rem<" << dataType << ">";
  printDims(os);
  printOperationInfo(os, sourceOp, 0);
  os << "\n";

  printIndent(os, 1);
  os << "lhs:\n";
  lhs->print(os, 2);

  os << "\n";
  printIndent(os, 1);
  os << "rhs:\n";
  rhs->print(os, 2);
}

void RemExprSV::print(llvm::raw_ostream& os, unsigned indent) const {
  printIndent(os, indent);
  os << "Rem<" << dataType << ">";
  printDims(os);
  printOperationInfo(os, sourceOp, indent);
  os << "\n";

  printIndent(os, indent + 1);
  os << "lhs:\n";
  lhs->print(os, indent + 2);

  os << "\n";
  printIndent(os, indent + 1);
  os << "rhs:\n";
  rhs->print(os, indent + 2);
}

//===----------------------------------------------------------------------===//
// UnknownSV
//===----------------------------------------------------------------------===//

void UnknownSV::print(llvm::raw_ostream& os) const {
  os << "Unknown<" << dataType << ">";
  printDims(os);
  printOperationInfo(os, sourceOp, 0);
}

void UnknownSV::print(llvm::raw_ostream& os, unsigned indent) const {
  std::string content;
  llvm::raw_string_ostream rso(content);

  rso << "Unknown<" << dataType << ">";
  rso.flush();  // 确保写入 string

  printWithOp(os, indent, content);
}

//===----------------------------------------------------------------------===//
// InductionSV
//===----------------------------------------------------------------------===//

void InductionSV::print(llvm::raw_ostream& os) const {
  os << "Induction[";
  if (init) init->print(os);
  else os << "?";
  os << "..";
  if (end) end->print(os);
  else os << "?";
  os << " step ";
  if (step) step->print(os);
  else os << "?";
  os << "]";
  printDims(os);
  printOperationInfo(os, sourceOp, 0);
}

void InductionSV::print(llvm::raw_ostream& os, unsigned indent) const {
  std::string content = "Induction[";
  // 简化：不递归打印，只显示结构
  content += init ? "init" : "?";
  content += "..";
  content += end ? "end" : "?";
  content += " step ";
  content += step ? "step" : "?";
  content += "]";
  printWithOp(os, indent, content);
}

//===----------------------------------------------------------------------===//
// IterArgSV
//===----------------------------------------------------------------------===//

void IterArgSV::print(llvm::raw_ostream& os) const {
  os << "IterArg<" << dataType << ">";
  printDims(os);
  printOperationInfo(os, sourceOp, 0);
}

void IterArgSV::print(llvm::raw_ostream& os, unsigned indent) const {
  std::string content;
  llvm::raw_string_ostream rso(content);

  rso << "IterArg<" << dataType << ">";
  rso.flush();  // 确保写入 string

  printWithOp(os, indent, content);
}

//===----------------------------------------------------------------------===//
// ArgSV
//===----------------------------------------------------------------------===//

void ArgSV::print(llvm::raw_ostream& os) const {
  os << "Arg<" << dataType << ">(arg" << argIndex;
  if (hasName()) os << ": " << name;
  os << ")";
  printDims(os);
  printOperationInfo(os, sourceOp, 0);
}

void ArgSV::print(llvm::raw_ostream& os, unsigned indent) const {
  std::string content;
  llvm::raw_string_ostream rso(content);

  rso << "Arg<" << dataType << ">(arg" << std::to_string(argIndex);
  rso.flush();  // 确保写入 string

  if (hasName()) content += ": " + name;
  content += ")";
  printWithOp(os, indent, content);
}

//===----------------------------------------------------------------------===//
// TensorSV
//===----------------------------------------------------------------------===//

std::shared_ptr<TensorSV> TensorSV::createMakeRange(
    int64_t start, int64_t end, Type elemType, Operation* op) {
  auto tensor = std::make_shared<TensorSV>(
      SourceKind::MakeRange, SmallVector<int64_t>{end - start}, elemType, op);
  tensor->elementExpr = std::make_shared<RangeExprSV>(start, end, elemType, op);
  // RangeExprSV 的 dims 已经是 {0}
  return tensor;
}

std::shared_ptr<TensorSV> TensorSV::createSplat(
    std::shared_ptr<ScalarSV> val,
    ArrayRef<int64_t> shape, Type elemType, Operation* op) {
  auto tensor = std::make_shared<TensorSV>(
      SourceKind::Splat, shape, elemType, op);
  // 设置 elementExpr 的 dims 为所有维度
  SmallVector<int> elemDims;
  for (size_t i = 0; i < shape.size(); ++i) {
    elemDims.push_back(i);
  }
  //val->setDims(elemDims);
  tensor->elementExpr = std::move(val);
  return tensor;
}

std::shared_ptr<TensorSV> TensorSV::createExpandDims(
    const TensorSV* input, int axis, Operation* op) {
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
      SourceKind::ExpandDims, newShape, input->elementType, op);

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
    const TensorSV* input, ArrayRef<int64_t> newShape, Operation* op) {
  if (!input) return nullptr;

  auto tensor = std::make_shared<TensorSV>(
      SourceKind::Broadcast, newShape, input->elementType, op);

  // broadcast 保持 dims 不变
  if (input->elementExpr) {
    tensor->elementExpr = input->elementExpr;
  }

  return tensor;
}

std::shared_ptr<TensorSV> TensorSV::createComputed(
    SourceKind op, const TensorSV* lhs, const TensorSV* rhs,
    Operation* mlirOp) {
  if (!lhs || !rhs) return nullptr;

  // 简化：假设形状相同
  auto tensor = std::make_shared<TensorSV>(
      op, lhs->shape, lhs->elementType, mlirOp);

  // 创建元素级运算表达式
  if (lhs->elementExpr && rhs->elementExpr) {
    std::shared_ptr<ScalarSV> elemExpr;
    Type elemType = lhs->elementType;

    switch (op) {
      // 四则运算
      case SourceKind::Add:
        elemExpr = std::make_shared<AddExprSV>(
            lhs->elementExpr, rhs->elementExpr, elemType, mlirOp);
        break;
      case SourceKind::Sub:
        elemExpr = std::make_shared<SubExprSV>(
            lhs->elementExpr, rhs->elementExpr, elemType, mlirOp);
        break;
      case SourceKind::Mul:
        elemExpr = std::make_shared<MulExprSV>(
            lhs->elementExpr, rhs->elementExpr, elemType, mlirOp);
        break;
      case SourceKind::Div:
        elemExpr = std::make_shared<DivExprSV>(
            lhs->elementExpr, rhs->elementExpr, elemType, mlirOp);
        break;
      case SourceKind::Rem:
        elemExpr = std::make_shared<RemExprSV>(
            lhs->elementExpr, rhs->elementExpr, elemType, mlirOp);
        break;
      // 比较运算
      case SourceKind::CmpEQ:
        elemExpr = std::make_shared<CmpExprSV>(
            CmpExprSV::Pred::EQ, lhs->elementExpr, rhs->elementExpr, elemType, mlirOp);
        break;
      case SourceKind::CmpNE:
        elemExpr = std::make_shared<CmpExprSV>(
            CmpExprSV::Pred::NE, lhs->elementExpr, rhs->elementExpr, elemType, mlirOp);
        break;
      case SourceKind::CmpLT:
        elemExpr = std::make_shared<CmpExprSV>(
            CmpExprSV::Pred::LT, lhs->elementExpr, rhs->elementExpr, elemType, mlirOp);
        break;
      case SourceKind::CmpLE:
        elemExpr = std::make_shared<CmpExprSV>(
            CmpExprSV::Pred::LE, lhs->elementExpr, rhs->elementExpr, elemType, mlirOp);
        break;
      case SourceKind::CmpGT:
        elemExpr = std::make_shared<CmpExprSV>(
            CmpExprSV::Pred::GT, lhs->elementExpr, rhs->elementExpr, elemType, mlirOp);
        break;
      case SourceKind::CmpGE:
        elemExpr = std::make_shared<CmpExprSV>(
            CmpExprSV::Pred::GE, lhs->elementExpr, rhs->elementExpr, elemType, mlirOp);
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
      //tensor->elementExpr->setDims(elemDims);
    }
  }

  return tensor;
}

std::shared_ptr<TensorSV> TensorSV::createSelect(
    const TensorSV* trueTensor,
    const TensorSV* falseTensor,
    std::shared_ptr<CmpExprSV> condition, Operation* op) {
  if (!trueTensor || !falseTensor) return nullptr;

  // 创建新的 TensorSV（Select 类型）
  auto tensor = std::make_shared<TensorSV>(
      SourceKind::Select, trueTensor->shape, trueTensor->elementType, op);

  // 创建元素的 Select 表达式
  if (trueTensor->elementExpr && falseTensor->elementExpr) {
    tensor->elementExpr = std::make_shared<SelectExprSV>(
        condition,
        trueTensor->elementExpr,
        falseTensor->elementExpr,
        trueTensor->elementType,
        op);

    // 设置 dims 为完整维度
    SmallVector<int> elemDims;
    for (size_t i = 0; i < trueTensor->shape.size(); ++i) {
      elemDims.push_back(i);
    }
    //ensor->elementExpr->setDims(elemDims);
  }

  return tensor;
}

std::shared_ptr<TensorSV> TensorSV::createLoad(
    ArrayRef<int64_t> shape, Type elemType, Operation* op) {
  auto tensor = std::make_shared<TensorSV>(
      SourceKind::Load, shape, elemType, op);

  // elementExpr 为 UnknownSV（load 产生的结果是未知的）
  tensor->elementExpr = std::make_shared<UnknownSV>(elemType, op);

  // 设置 dims 为完整维度
  SmallVector<int> elemDims;
  for (size_t i = 0; i < shape.size(); ++i) {
    elemDims.push_back(i);
  }
  //tensor->elementExpr->setDims(elemDims);

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
    case SourceKind::MakeRange: os << " <- make_range"; break;
    case SourceKind::Splat: os << " <- splat"; break;
    case SourceKind::ExpandDims: os << " <- expand_dims"; break;
    case SourceKind::Broadcast: os << " <- broadcast"; break;
    // 四则运算
    case SourceKind::Add: os << " <- add"; break;
    case SourceKind::Sub: os << " <- sub"; break;
    case SourceKind::Mul: os << " <- mul"; break;
    case SourceKind::Div: os << " <- div"; break;
    case SourceKind::Rem: os << " <- rem"; break;
    // 比较运算
    case SourceKind::CmpEQ: os << " <- cmp_eq"; break;
    case SourceKind::CmpNE: os << " <- cmp_ne"; break;
    case SourceKind::CmpLT: os << " <- cmp_lt"; break;
    case SourceKind::CmpLE: os << " <- cmp_le"; break;
    case SourceKind::CmpGT: os << " <- cmp_gt"; break;
    case SourceKind::CmpGE: os << " <- cmp_ge"; break;
    // 选择运算
    case SourceKind::Select: os << " <- select"; break;
    // Load 运算
    case SourceKind::Load: os << " <- load"; break;
    case SourceKind::Computed: os << " <- computed"; break;
  }

  printOperationInfo(os, sourceOp, 0);

  if (elementExpr) {
    os << "\n";
    printIndent(os, 1);
    os << "element:\n";
    elementExpr->print(os, 2);
  }
}

void TensorSV::print(llvm::raw_ostream& os, unsigned indent) const {
  printIndent(os, indent);
  os << "Tensor[";
  for (size_t i = 0; i < shape.size(); ++i) {
    if (i > 0) os << "x";
    os << shape[i];
  }
  os << "]";

  switch (source) {
    case SourceKind::MakeRange: os << " <- make_range"; break;
    case SourceKind::Splat: os << " <- splat"; break;
    case SourceKind::ExpandDims: os << " <- expand_dims"; break;
    case SourceKind::Broadcast: os << " <- broadcast"; break;
    case SourceKind::Add: os << " <- add"; break;
    case SourceKind::Sub: os << " <- sub"; break;
    case SourceKind::Mul: os << " <- mul"; break;
    case SourceKind::Div: os << " <- div"; break;
    case SourceKind::Rem: os << " <- rem"; break;
    case SourceKind::CmpEQ: os << " <- cmp_eq"; break;
    case SourceKind::CmpNE: os << " <- cmp_ne"; break;
    case SourceKind::CmpLT: os << " <- cmp_lt"; break;
    case SourceKind::CmpLE: os << " <- cmp_le"; break;
    case SourceKind::CmpGT: os << " <- cmp_gt"; break;
    case SourceKind::CmpGE: os << " <- cmp_ge"; break;
    case SourceKind::Select: os << " <- select"; break;
    case SourceKind::Load: os << " <- load"; break;
    case SourceKind::Computed: os << " <- computed"; break;
  }

  printOperationInfo(os, sourceOp, indent);

  if (elementExpr) {
    os << "\n";
    printIndent(os, indent + 1);
    os << "element:\n";
    elementExpr->print(os, indent + 2);
  }
}

TensorSV::TensorSV(SourceKind src, ArrayRef<int64_t> s, Type elemType,
                   Operation* op)
    : SymValue(Kind::Tensor, op), source(src), elementType(elemType) {
  shape.append(s.begin(), s.end());
}

//===----------------------------------------------------------------------===//
// 辅助函数
//===----------------------------------------------------------------------===//


std::optional<int64_t> getConstantInt(const SymValue* sv) {
  if (auto* ci = dyn_cast<ScalarConstantIntSV>(sv)) {
    return ci->getInt();
  }
  return std::nullopt;
}

std::optional<double> getConstantFloat(const SymValue* sv) {
  if (auto* cf = dyn_cast<ScalarConstantFloatSV>(sv)) {
    return cf->getFloat();
  }
  return std::nullopt;
}

} // namespace ascend
} // namespace triton
} // namespace mlir
