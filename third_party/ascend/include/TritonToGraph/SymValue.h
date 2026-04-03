/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */

#ifndef TRITON_TO_GRAPH_SYM_VALUE_H
#define TRITON_TO_GRAPH_SYM_VALUE_H

#include "mlir/IR/Value.h"
#include "mlir/IR/Types.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/Casting.h"

namespace mlir {
namespace triton {
namespace ascend {

//===----------------------------------------------------------------------===//
// SymValue - 符号执行值基类
// 使用MLIR风格的dyn_cast进行类型转换
//===----------------------------------------------------------------------===//

class SymValue {
public:
  enum class Kind {
    // 标量类型
    ScalarConstantInt,   // 整数常量
    ScalarConstantFloat, // 浮点常量
    ScalarExpr,          // 代数表达式
    ProgramID,           // get_program_id

    // Tensor类型
    TensorRange,         // make_range结果 (1D)
    TensorSplat,         // splat结果
    TensorExpr,          // tensor级算术运算

    // 指针类型
    PtrBase,             // addptr scalar
    PtrTensor,           // addptr tensor

    // 特殊
    Unknown,
  };

  SymValue(Kind k) : kind(k), elementType(nullptr) {}
  virtual ~SymValue() = default;

  Kind getKind() const { return kind; }
  Type getElementType() const { return elementType; }
  void setElementType(Type t) { elementType = t; }

  // 是否为常量
  bool isConstant() const {
    return kind == Kind::ScalarConstantInt ||
           kind == Kind::ScalarConstantFloat;
  }

  // 打印调试用
  virtual void print(llvm::raw_ostream& os) const = 0;

protected:
  Kind kind;
  Type elementType;  // 所有SymValue都记住元素类型
};

//===----------------------------------------------------------------------===//
// 标量常量基类（抽象）
//===----------------------------------------------------------------------===//

class ScalarConstantSV : public SymValue {
public:
  static bool classof(const SymValue* v) {
    return v->getKind() == Kind::ScalarConstantInt ||
           v->getKind() == Kind::ScalarConstantFloat;
  }

protected:
  ScalarConstantSV(Kind k) : SymValue(k) {}
};

//===----------------------------------------------------------------------===//
// 整数常量
//===----------------------------------------------------------------------===//

class ScalarConstantIntSV : public ScalarConstantSV {
  llvm::APInt value;

public:
  explicit ScalarConstantIntSV(int64_t val)
      : ScalarConstantSV(Kind::ScalarConstantInt),
        value(64, val, true) {}  // 64位有符号整数

  explicit ScalarConstantIntSV(const llvm::APInt& v)
      : ScalarConstantSV(Kind::ScalarConstantInt),
        value(v) {}

  int64_t getInt() const {
    // 确保value能被安全转换为int64_t
    if (value.getBitWidth() <= 64) {
      return value.getSExtValue();
    }
    // 如果位宽超过64，截取低64位
    return value.trunc(64).getSExtValue();
  }

  const llvm::APInt& getAPInt() const { return value; }

  static bool classof(const SymValue* v) {
    return v->getKind() == Kind::ScalarConstantInt;
  }

  void print(llvm::raw_ostream& os) const override {
    os << "const.i" << value.getBitWidth() << "(" << getInt() << ")";
  }
};

//===----------------------------------------------------------------------===//
// 浮点常量
//===----------------------------------------------------------------------===//

class ScalarConstantFloatSV : public ScalarConstantSV {
  llvm::APFloat value;

public:
  explicit ScalarConstantFloatSV(double val)
      : ScalarConstantSV(Kind::ScalarConstantFloat),
        value(val) {}

  explicit ScalarConstantFloatSV(const llvm::APFloat& v)
      : ScalarConstantSV(Kind::ScalarConstantFloat),
        value(v) {}

  double getFloat() const { return value.convertToDouble(); }

  const llvm::APFloat& getAPFloat() const { return value; }

  static bool classof(const SymValue* v) {
    return v->getKind() == Kind::ScalarConstantFloat;
  }

  void print(llvm::raw_ostream& os) const override {
    os << "const.f(" << getFloat() << ")";
  }
};

//===----------------------------------------------------------------------===//
// 代数表达式（支持代数变换）
//===----------------------------------------------------------------------===//

class ScalarExprSV : public SymValue {
public:
  enum class OpKind {
    Add, Sub, Mul, Div,
    CmpEQ, CmpNE, CmpLT, CmpLE, CmpGT, CmpGE,  // 比较运算
    Select,  // 选择运算（用于识别min截断）
  };

private:
  OpKind op;
  SymValue* lhs;  // 左操作数
  SymValue* rhs;  // 右操作数（Select时有特殊含义）
  // 对于Select，condition单独存储
  SymValue* condition;  // 仅用于Select

public:
  // 二元运算构造函数
  ScalarExprSV(OpKind o, SymValue* l, SymValue* r)
      : SymValue(Kind::ScalarExpr),
        op(o), lhs(l), rhs(r), condition(nullptr) {}

  // Select构造函数
  ScalarExprSV(SymValue* cond, SymValue* trueVal, SymValue* falseVal)
      : SymValue(Kind::ScalarExpr),
        op(OpKind::Select), lhs(trueVal), rhs(falseVal), condition(cond) {}

  OpKind getOp() const { return op; }
  SymValue* getLHS() const { return lhs; }
  SymValue* getRHS() const { return rhs; }
  SymValue* getCondition() const { return condition; }

  // 是否为比较操作
  bool isComparison() const {
    return op == OpKind::CmpEQ || op == OpKind::CmpNE ||
           op == OpKind::CmpLT || op == OpKind::CmpLE ||
           op == OpKind::CmpGT || op == OpKind::CmpGE;
  }

  // 是否为Select操作
  bool isSelect() const { return op == OpKind::Select; }

  // 代数变换方法
  // 1. 操作符前后是常量，可计算结果
  bool canFoldConstants() const;

  // 2. 合并常量
  ScalarConstantSV* foldConstants();

  // 3. 应用结合律: (a + b) + c = a + (b + c)
  ScalarExprSV* applyAssociative();

  // 4. 应用分配率: a * (b + c) = a * b + a * c
  ScalarExprSV* applyDistributive();

  // 5. 规范化（交换律调整位置，常量放右边）
  ScalarExprSV* canonicalize();

  // 6. 合并同类项: x + 2*x = 3*x
  ScalarExprSV* combineLikeTerms();

  // 7. 完整简化（应用所有变换）
  SymValue* simplify();

  static bool classof(const SymValue* v) {
    return v->getKind() == Kind::ScalarExpr;
  }

  void print(llvm::raw_ostream& os) const override;

  // 辅助方法：获取操作符字符串
  static const char* getOpStr(OpKind k);
};

//===----------------------------------------------------------------------===//
// Program ID
//===----------------------------------------------------------------------===//

class ProgramIDSV : public SymValue {
  int axis;  // x=0, y=1, z=2

public:
  explicit ProgramIDSV(int a)
      : SymValue(Kind::ProgramID), axis(a) {}

  int getAxis() const { return axis; }
  const char* getAxisName() const {
    return axis == 0 ? "x" : (axis == 1 ? "y" : "z");
  }

  static bool classof(const SymValue* v) {
    return v->getKind() == Kind::ProgramID;
  }

  void print(llvm::raw_ostream& os) const override {
    os << "pid." << getAxisName();
  }
};

//===----------------------------------------------------------------------===//
// make_range 结果: tensor<len x element_type>
//===----------------------------------------------------------------------===//

class TensorRangeSV : public SymValue {
  int64_t start, end;  // end是exclusive

public:
  TensorRangeSV(int64_t s, int64_t e)
      : SymValue(Kind::TensorRange), start(s), end(e) {
    assert(end > start && "make_range requires end > start");
  }

  int64_t getStart() const { return start; }
  int64_t getEnd() const { return end; }
  int64_t getLen() const { return end - start; }  // 隐含的shape

  // 获取shape（1D）
  SmallVector<int64_t> getShape() const {
    return {getLen()};
  }

  // 获取第i个元素的SymValue（这是一个表达式，不是存储的值）
  // 注意：这个返回的SymValue需要外部管理生命周期
  ScalarExprSV* getElementExpr(int64_t index) const;

  // 获取指定位置的常量值（如果可计算）
  int64_t getElementValue(int64_t index) const {
    return start + index;
  }

  static bool classof(const SymValue* v) {
    return v->getKind() == Kind::TensorRange;
  }

  void print(llvm::raw_ostream& os) const override {
    os << "range[" << start << ", " << end << ")";
  }
};

//===----------------------------------------------------------------------===//
// splat 结果
//===----------------------------------------------------------------------===//

class TensorSplatSV : public SymValue {
  SmallVector<int64_t> shape;
  SymValue* elementValue;  // 广播的源值

public:
  TensorSplatSV(ArrayRef<int64_t> s, SymValue* ev)
      : SymValue(Kind::TensorSplat),
        shape(s), elementValue(ev) {}

  ArrayRef<int64_t> getShape() const { return shape; }
  SymValue* getElementValue() const { return elementValue; }

  // 获取线性索引对应的元素（都指向同一个elementValue）
  SymValue* getElement(int64_t linearIndex) const {
    // splat的所有元素都相同，忽略索引
    (void)linearIndex;
    return elementValue;
  }

  // 多维索引转线性索引
  int64_t getLinearIndex(ArrayRef<int64_t> indices) const;

  // 线性索引转多维坐标
  SmallVector<int64_t> getMultiDimIndex(int64_t linearIdx) const;

  static bool classof(const SymValue* v) {
    return v->getKind() == Kind::TensorSplat;
  }

  void print(llvm::raw_ostream& os) const override {
    os << "splat(";
    elementValue->print(os);
    os << ") -> [";
    for (size_t i = 0; i < shape.size(); ++i) {
      if (i > 0) os << "x";
      os << shape[i];
    }
    os << "]";
  }
};

//===----------------------------------------------------------------------===//
// Tensor级算术运算
//===----------------------------------------------------------------------===//

class TensorExprSV : public SymValue {
public:
  enum class OpKind {
    Add, Sub, Mul, Div,
    CmpEQ, CmpNE, CmpLT, CmpLE, CmpGT, CmpGE,  // 比较运算
    Select,        // 选择运算
    ExpandDims,    // expand_dims
    Broadcast,     // broadcast
  };

private:
  OpKind op;
  SmallVector<int64_t> shape;
  SmallVector<SymValue*> operands;  // 操作数
  SmallVector<int64_t> broadcastDims;  // 用于Broadcast记录广播维度
  int64_t expandAxis;  // 用于ExpandDims

public:
  // 普通二元运算构造函数
  TensorExprSV(OpKind o, ArrayRef<int64_t> s,
               SymValue* lhs, SymValue* rhs)
      : SymValue(Kind::TensorExpr),
        op(o), shape(s), expandAxis(-1) {
    operands.push_back(lhs);
    operands.push_back(rhs);
  }

  // Select构造函数
  TensorExprSV(SymValue* cond, SymValue* trueVal, SymValue* falseVal,
               ArrayRef<int64_t> s)
      : SymValue(Kind::TensorExpr),
        op(OpKind::Select), shape(s), expandAxis(-1) {
    operands.push_back(cond);
    operands.push_back(trueVal);
    operands.push_back(falseVal);
  }

  // ExpandDims构造函数
  TensorExprSV(SymValue* input, int64_t axis, ArrayRef<int64_t> s)
      : SymValue(Kind::TensorExpr),
        op(OpKind::ExpandDims), shape(s), expandAxis(axis) {
    operands.push_back(input);
  }

  // Broadcast构造函数
  TensorExprSV(SymValue* input, ArrayRef<int64_t> s,
               ArrayRef<int64_t> bcastDims)
      : SymValue(Kind::TensorExpr),
        op(OpKind::Broadcast), shape(s),
        broadcastDims(bcastDims), expandAxis(-1) {
    operands.push_back(input);
  }

  OpKind getOp() const { return op; }
  ArrayRef<int64_t> getShape() const { return shape; }
  ArrayRef<SymValue*> getOperands() const { return operands; }

  // 获取指定位置的元素表达式
  // 这会递归地构建每个元素的SymValue
  SymValue* getElement(ArrayRef<int64_t> indices) const;

  // 识别是否为长度检测模式（用于生成min截断）
  // 返回true如果这是类似 select(cmp_lt(idx, bound), idx, 0) 的模式
  bool isLengthCheckPattern(SymValue*& range, int64_t& bound) const;

  // 识别是否为min模式
  bool isMinPattern() const;

  // 识别是否为max模式
  bool isMaxPattern() const;

  int64_t getExpandAxis() const { return expandAxis; }
  ArrayRef<int64_t> getBroadcastDims() const { return broadcastDims; }

  static bool classof(const SymValue* v) {
    return v->getKind() == Kind::TensorExpr;
  }

  void print(llvm::raw_ostream& os) const override;
};

//===----------------------------------------------------------------------===//
// 指针类型 - 标量指针（addptr scalar）
//===----------------------------------------------------------------------===//

class PtrBaseSV : public SymValue {
  Value basePtr;        // 原始base指针（MLIR Value）
  SymValue* offset;     // offset对应的SymValue
  Type pointeeType;     // 指向的元素类型

public:
  PtrBaseSV(Value bp, SymValue* off, Type pt)
      : SymValue(Kind::PtrBase),
        basePtr(bp), offset(off), pointeeType(pt) {}

  Value getBasePtr() const { return basePtr; }
  SymValue* getOffset() const { return offset; }
  Type getPointeeType() const { return pointeeType; }

  // 计算完整偏移量（简化后的表达式）
  SymValue* computeFullOffset() const { return offset; }

  static bool classof(const SymValue* v) {
    return v->getKind() == Kind::PtrBase;
  }

  void print(llvm::raw_ostream& os) const override {
    os << "ptr.base(";
    offset->print(os);
    os << ")";
  }
};

//===----------------------------------------------------------------------===//
// 指针类型 - Tensor指针（addptr tensor）
//===----------------------------------------------------------------------===//

class PtrTensorSV : public SymValue {
  Value basePtr;        // 原始base指针
  SmallVector<int64_t> shape;  // tensor形状
  // 每个元素的offset（linear index -> offset SymValue）
  DenseMap<uint64_t, SymValue*> elementOffsets;
  Type pointeeType;     // 指向的元素类型

public:
  PtrTensorSV(Value bp, ArrayRef<int64_t> s, Type pt)
      : SymValue(Kind::PtrTensor),
        basePtr(bp), shape(s), pointeeType(pt) {}

  Value getBasePtr() const { return basePtr; }
  ArrayRef<int64_t> getShape() const { return shape; }
  Type getPointeeType() const { return pointeeType; }

  // 获取元素数量
  int64_t getNumElements() const {
    int64_t n = 1;
    for (auto s : shape) n *= s;
    return n;
  }

  // 多维索引转线性索引
  int64_t getLinearIndex(ArrayRef<int64_t> indices) const;

  // 设置指定位置的offset
  void setElementOffset(ArrayRef<int64_t> indices, SymValue* offset);

  // 获取指定位置的offset
  SymValue* getElementOffset(ArrayRef<int64_t> indices) const;

  // 获取指定线性索引的offset
  SymValue* getElementOffset(int64_t linearIdx) const;

  // 获取所有offsets（用于分析）
  const DenseMap<uint64_t, SymValue*>& getAllOffsets() const {
    return elementOffsets;
  }

  // 计算stride信息（基于offsets推导）
  SmallVector<int64_t> inferStrides() const;

  static bool classof(const SymValue* v) {
    return v->getKind() == Kind::PtrTensor;
  }

  void print(llvm::raw_ostream& os) const override {
    os << "ptr.tensor[shape=";
    for (size_t i = 0; i < shape.size(); ++i) {
      if (i > 0) os << "x";
      os << shape[i];
    }
    os << ", offsets=" << elementOffsets.size() << "]";
  }
};

//===----------------------------------------------------------------------===//
// 辅助函数
//===----------------------------------------------------------------------===//

// 创建二元运算（自动处理常量合并）
ScalarExprSV* createBinaryExpr(ScalarExprSV::OpKind op,
                               SymValue* lhs, SymValue* rhs);

// 判断是否为常量
inline bool isConstant(SymValue* sv) {
  return sv && sv->isConstant();
}

// 获取整数常量值（如果不是整数常量返回nullopt）
llvm::Optional<int64_t> getConstantInt(SymValue* sv);

// 获取浮点常量值
llvm::Optional<double> getConstantFloat(SymValue* sv);

} // namespace ascend
} // namespace triton
} // namespace mlir

#endif // TRITON_TO_GRAPH_SYM_VALUE_H
