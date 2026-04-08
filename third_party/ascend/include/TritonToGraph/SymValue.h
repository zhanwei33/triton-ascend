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
#include "llvm/Support/Casting.h"
#include <memory>
#include <optional>

#include "triton/Dialect/Triton/IR/Types.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Operation.h"

namespace tt = mlir::triton;
namespace mlir {
namespace triton {
namespace cfg {
class Instruction;  // Forward declaration
}
namespace ascend {

using cfg::Instruction;  // Bring Instruction into ascend namespace

// Forward declaration for Operation
class Operation;

//===----------------------------------------------------------------------===//
// SymValue - 符号执行值基类
//===----------------------------------------------------------------------===//

class SymValue {
public:
  enum class Kind {
    // Scalar
    ScalarConstantInt,
    ScalarConstantFloat,
    AddExpr,
    SubExpr,
    MulExpr,
    DivExpr,
    RangeExpr,
    CmpExpr,
    SelectExpr,
    PtrExpr,
    TensorPtr,      // make_tensor_ptr 结果
    ProgramID,
    GmPtr,          // kernel 指针类型入参
    RemExpr,        // rem 操作产生的值
    Unknown,        // 未知值（如 load 结果）
    Induction,      // for 循环迭代变量
    IterArg,        // for 的 iter_arg
    Arg,            // kernel 非指针类型入参

    // Tensor
    Tensor,
  };

  SymValue(Kind k, Operation* op = nullptr) : kind(k), sourceOp(op) {}
  virtual ~SymValue() = default;

  Kind getKind() const { return kind; }

  /// 获取创建该 SymValue 的 Operation
  Operation* getOperation() const { return sourceOp; }

  /// 设置 Operation
  void setOperation(Operation* op) { sourceOp = op; }

  bool isScalar() const;
  bool isTensor() const;

  virtual void print(llvm::raw_ostream& os) const = 0;

  /// 带缩进的打印
  virtual void print(llvm::raw_ostream& os, unsigned indent) const {
    print(os);  // 默认实现
  }

protected:
  Kind kind;
  Operation* sourceOp;  // 指向创建该 SymValue 的 operation

  /// 获取 Operation 的单行字符串表示
  static std::string getOperationStr(Operation* op);

  /// 打印缩进空格
  static void printIndent(llvm::raw_ostream& os, unsigned indent);

  /// 打印 operation 信息（右对齐）
  static void printOperationInfo(llvm::raw_ostream& os, Operation* op,
                                  unsigned currentIndent = 0);
};

//===----------------------------------------------------------------------===//
// ScalarSV - 标量基类（含维度信息）
//
// 维度信息语义：
// - [-1]                : 未关联 Tensor
// - [0]                 : 关联 make_range 结果 (1D)
// - [0,1,2]             : 关联 splat 的 nD Tensor
// - [1] 或 [0]          : 关联 expand_dims 结果（被保持的维度）
// - [0,1]               : 关联 broadcast/代数运算的 2D Tensor
// - [0,1,2]             : 关联 3D Tensor 的元素
//===----------------------------------------------------------------------===//

class ScalarSV : public SymValue {
public:
  // 维度信息：该 Scalar 在哪些维度上存在
  SmallVector<int> dims;

  ScalarSV(Kind k, Operation* op = nullptr) : SymValue(k, op) {
    dims.push_back(-1);  // 默认未关联
  }

  explicit ScalarSV(Kind k, ArrayRef<int> d, Operation* op = nullptr)
      : SymValue(k, op) {
    dims.append(d.begin(), d.end());
  }

  // 获取数据类型
  virtual Type getDataType() const = 0;

  // 是否关联 Tensor
  bool isAssociated() const {
    return !(dims.size() == 1 && dims[0] == -1);
  }

  // 获取维度数
  int getRank() const {
    if (dims.size() == 1 && dims[0] == -1) return 0;
    return dims.size();
  }

  // 设置维度（用于 splat/expand_dims/broadcast 后更新）
  void setDims(ArrayRef<int> d) {
    dims.clear();
    dims.append(d.begin(), d.end());
  }

  /// 打印维度信息
  void printDims(llvm::raw_ostream& os) const;

  /// 带 operation 信息的打印接口
  void printWithOp(llvm::raw_ostream& os, unsigned indent,
                   const std::string& content) const;

  static bool classof(const SymValue* v) {
    return v->getKind() != Kind::Tensor;
  }
};

//===----------------------------------------------------------------------===//
// ScalarConstantSV - 常量基类
//===----------------------------------------------------------------------===//

class ScalarConstantSV : public ScalarSV {
public:
  static bool classof(const SymValue* v) {
    return v->getKind() == Kind::ScalarConstantInt ||
           v->getKind() == Kind::ScalarConstantFloat;
  }

protected:
  ScalarConstantSV(Kind k, Operation* op = nullptr) : ScalarSV(k, op) {}
  ScalarConstantSV(Kind k, ArrayRef<int> d, Operation* op = nullptr)
      : ScalarSV(k, d, op) {}
};

//===----------------------------------------------------------------------===//
// ScalarConstantIntSV - 整数常量
//===----------------------------------------------------------------------===//

class ScalarConstantIntSV : public ScalarConstantSV {
  llvm::APInt value;
  Type dataType;

public:
  explicit ScalarConstantIntSV(int64_t val, Type type, Operation* op = nullptr)
      : ScalarConstantSV(Kind::ScalarConstantInt, op),
        value(64, val, true), dataType(type) {}

  explicit ScalarConstantIntSV(int64_t val, Type type, ArrayRef<int> d,
                                Operation* op = nullptr)
      : ScalarConstantSV(Kind::ScalarConstantInt, d, op),
        value(64, val, true), dataType(type) {}

  int64_t getInt() const;
  const llvm::APInt& getAPInt() const { return value; }
  Type getDataType() const override { return dataType; }

  static bool classof(const SymValue* v) {
    return v->getKind() == Kind::ScalarConstantInt;
  }
  void print(llvm::raw_ostream& os) const override;
};

//===----------------------------------------------------------------------===//
// ScalarConstantFloatSV - 浮点常量
//===----------------------------------------------------------------------===//

class ScalarConstantFloatSV : public ScalarConstantSV {
  llvm::APFloat value;
  Type dataType;

public:
  explicit ScalarConstantFloatSV(double val, Type type, Operation* op = nullptr)
      : ScalarConstantSV(Kind::ScalarConstantFloat, op),
        value(val), dataType(type) {}

  explicit ScalarConstantFloatSV(double val, Type type, ArrayRef<int> d,
                                  Operation* op = nullptr)
      : ScalarConstantSV(Kind::ScalarConstantFloat, d, op),
        value(val), dataType(type) {}

  double getFloat() const;
  const llvm::APFloat& getAPFloat() const { return value; }
  Type getDataType() const override { return dataType; }

  static bool classof(const SymValue* v) {
    return v->getKind() == Kind::ScalarConstantFloat;
  }
  void print(llvm::raw_ostream& os) const override;
};

//===----------------------------------------------------------------------===//
// 四则运算表达式 (Add/Sub/Mul/Div)
//
// 注意：构造时 dims 默认为 [-1]，应在 symbolic execution 时设置
//===----------------------------------------------------------------------===//

class AddExprSV : public ScalarSV {
  std::shared_ptr<ScalarSV> lhs;
  std::shared_ptr<ScalarSV> rhs;
  Type dataType;

public:
  AddExprSV(std::shared_ptr<ScalarSV> l, std::shared_ptr<ScalarSV> r,
            Type type, Operation* op = nullptr);

  ScalarSV* getLHS() const { return lhs.get(); }
  ScalarSV* getRHS() const { return rhs.get(); }
  Type getDataType() const override { return dataType; }

  static bool classof(const SymValue* v) {
    return v->getKind() == Kind::AddExpr;
  }
  void print(llvm::raw_ostream& os) const override;
  void print(llvm::raw_ostream& os, unsigned indent) const override;
};

class SubExprSV : public ScalarSV {
  std::shared_ptr<ScalarSV> lhs;
  std::shared_ptr<ScalarSV> rhs;
  Type dataType;

public:
  SubExprSV(std::shared_ptr<ScalarSV> l, std::shared_ptr<ScalarSV> r,
            Type type, Operation* op = nullptr);

  ScalarSV* getLHS() const { return lhs.get(); }
  ScalarSV* getRHS() const { return rhs.get(); }
  Type getDataType() const override { return dataType; }

  static bool classof(const SymValue* v) {
    return v->getKind() == Kind::SubExpr;
  }
  void print(llvm::raw_ostream& os) const override;
  void print(llvm::raw_ostream& os, unsigned indent) const override;
};

class MulExprSV : public ScalarSV {
  std::shared_ptr<ScalarSV> lhs;
  std::shared_ptr<ScalarSV> rhs;
  Type dataType;

public:
  MulExprSV(std::shared_ptr<ScalarSV> l, std::shared_ptr<ScalarSV> r,
            Type type, Operation* op = nullptr);

  ScalarSV* getLHS() const { return lhs.get(); }
  ScalarSV* getRHS() const { return rhs.get(); }
  Type getDataType() const override { return dataType; }

  static bool classof(const SymValue* v) {
    return v->getKind() == Kind::MulExpr;
  }
  void print(llvm::raw_ostream& os) const override;
  void print(llvm::raw_ostream& os, unsigned indent) const override;
};

class DivExprSV : public ScalarSV {
  std::shared_ptr<ScalarSV> lhs;
  std::shared_ptr<ScalarSV> rhs;
  Type dataType;

public:
  DivExprSV(std::shared_ptr<ScalarSV> l, std::shared_ptr<ScalarSV> r,
            Type type, Operation* op = nullptr);

  ScalarSV* getLHS() const { return lhs.get(); }
  ScalarSV* getRHS() const { return rhs.get(); }
  Type getDataType() const override { return dataType; }

  static bool classof(const SymValue* v) {
    return v->getKind() == Kind::DivExpr;
  }
  void print(llvm::raw_ostream& os) const override;
  void print(llvm::raw_ostream& os, unsigned indent) const override;
};

//===----------------------------------------------------------------------===//
// RangeExprSV - make_range 产生的表达式 [start, end)
//===----------------------------------------------------------------------===//

class RangeExprSV : public ScalarSV {
  int64_t start;
  int64_t end;
  Type dataType;

public:
  RangeExprSV(int64_t s, int64_t e, Type type, Operation* op = nullptr);

  int64_t getStart() const { return start; }
  int64_t getEnd() const { return end; }
  int64_t getSize() const { return end - start; }
  Type getDataType() const override { return dataType; }

  static bool classof(const SymValue* v) {
    return v->getKind() == Kind::RangeExpr;
  }
  void print(llvm::raw_ostream& os) const override;
  void print(llvm::raw_ostream& os, unsigned indent) const override;
};

//===----------------------------------------------------------------------===//
// CmpExprSV - 比较表达式
//
// 注意：构造时 dims 默认为 [-1]，应在 symbolic execution 时设置
//===----------------------------------------------------------------------===//

class CmpExprSV : public ScalarSV {
public:
  enum class Pred { EQ, NE, LT, LE, GT, GE };

private:
  Pred pred;
  std::shared_ptr<ScalarSV> lhs;
  std::shared_ptr<ScalarSV> rhs;
  Type dataType;

public:
  CmpExprSV(Pred p, std::shared_ptr<ScalarSV> l,
            std::shared_ptr<ScalarSV> r, Type type, Operation* op = nullptr);

  Pred getPred() const { return pred; }
  ScalarSV* getLHS() const { return lhs.get(); }
  ScalarSV* getRHS() const { return rhs.get(); }
  Type getDataType() const override { return dataType; }

  /// 获取比较操作符字符串
  const char* getPredStr() const;

  static bool classof(const SymValue* v) {
    return v->getKind() == Kind::CmpExpr;
  }
  void print(llvm::raw_ostream& os) const override;
  void print(llvm::raw_ostream& os, unsigned indent) const override;
};

//===----------------------------------------------------------------------===//
// SelectExprSV - 选择表达式
//
// 注意：构造时 dims 默认为 [-1]，应在 symbolic execution 时设置
//===----------------------------------------------------------------------===//

class SelectExprSV : public ScalarSV {
  std::shared_ptr<CmpExprSV> condition;
  std::shared_ptr<ScalarSV> trueVal;
  std::shared_ptr<ScalarSV> falseVal;
  Type dataType;

public:
  SelectExprSV(std::shared_ptr<CmpExprSV> cond,
               std::shared_ptr<ScalarSV> t,
               std::shared_ptr<ScalarSV> f, Type type,
               Operation* op = nullptr);

  CmpExprSV* getCondition() const { return condition.get(); }
  ScalarSV* getTrueVal() const { return trueVal.get(); }
  ScalarSV* getFalseVal() const { return falseVal.get(); }
  Type getDataType() const override { return dataType; }

  bool isMinPattern() const;
  bool isMaxPattern() const;
  bool isLengthCheck() const;

  static bool classof(const SymValue* v) {
    return v->getKind() == Kind::SelectExpr;
  }
  void print(llvm::raw_ostream& os) const override;
  void print(llvm::raw_ostream& os, unsigned indent) const override;
};

//===----------------------------------------------------------------------===//
// PtrExprSV - 指针表达式 (addptr)
//
// 注意：构造时 dims 默认为 [-1]，应在 symbolic execution 时设置
//===----------------------------------------------------------------------===//

class PtrExprSV : public ScalarSV {
  std::shared_ptr<ScalarSV> basePtr;  // 基指针
  std::shared_ptr<ScalarSV> offset;   // 偏移量
  Type pointeeType;

public:
  PtrExprSV(std::shared_ptr<ScalarSV> base,
            std::shared_ptr<ScalarSV> off,
            Type pt, Operation* op = nullptr);

  ScalarSV* getBasePtr() const { return basePtr.get(); }
  ScalarSV* getOffset() const { return offset.get(); }
  Type getPointeeType() const { return pointeeType; }
  Type getDataType() const override {
    return tt::PointerType::get(pointeeType, 1);
  }

  // 计算完整偏移 (base + offset)
  std::shared_ptr<AddExprSV> computeTotalOffset() const;

  static bool classof(const SymValue* v) {
    return v->getKind() == Kind::PtrExpr;
  }
  void print(llvm::raw_ostream& os) const override;
  void print(llvm::raw_ostream& os, unsigned indent) const override;
};

//===----------------------------------------------------------------------===//
// TensorPtrSV - make_tensor_ptr 产生的块指针
// 保存 shape, strides, offsets, block shape 信息（使用 ScalarSV 支持符号表达式）
//===----------------------------------------------------------------------===//

class TensorPtrSV : public ScalarSV {
  SmallVector<std::shared_ptr<ScalarSV>> shape;      // 总形状
  SmallVector<std::shared_ptr<ScalarSV>> strides;    // 步长
  SmallVector<std::shared_ptr<ScalarSV>> offsets;    // 偏移量
  SmallVector<int64_t> blockShape;                   // 块形状
  Type pointeeType;                                  // 指向的元素类型

public:
  TensorPtrSV(ArrayRef<std::shared_ptr<ScalarSV>> s,
              ArrayRef<std::shared_ptr<ScalarSV>> st,
              ArrayRef<std::shared_ptr<ScalarSV>> off,
              ArrayRef<int64_t> bs,
              Type pt, Operation* op = nullptr)
      : ScalarSV(Kind::TensorPtr, op),
        pointeeType(pt) {
    shape.append(s.begin(), s.end());
    strides.append(st.begin(), st.end());
    offsets.append(off.begin(), off.end());
    blockShape.append(bs.begin(), bs.end());
  }

  ArrayRef<std::shared_ptr<ScalarSV>> getShape() const { return shape; }
  ArrayRef<std::shared_ptr<ScalarSV>> getStrides() const { return strides; }
  ArrayRef<std::shared_ptr<ScalarSV>> getOffsets() const { return offsets; }
  ArrayRef<int64_t> getBlockShape() const { return blockShape; }
  Type getPointeeType() const { return pointeeType; }
  Type getDataType() const override {
    return tt::PointerType::get(pointeeType, 1);
  }

  // 获取维度数（rank）
  int getRank() const { return shape.size(); }

  static bool classof(const SymValue* v) {
    return v->getKind() == Kind::TensorPtr;
  }
  void print(llvm::raw_ostream& os) const override;
  void print(llvm::raw_ostream& os, unsigned indent) const override;
};

//===----------------------------------------------------------------------===//
// ProgramIDSV - get_program_id
//===----------------------------------------------------------------------===//

class ProgramIDSV : public ScalarSV {
  int axis;  // 0=x, 1=y, 2=z
  Type dataType;

public:
  ProgramIDSV(int a, Type type, Operation* op = nullptr)
      : ScalarSV(Kind::ProgramID, op), axis(a), dataType(type) {}

  int getAxis() const { return axis; }
  Type getDataType() const override { return dataType; }

  static bool classof(const SymValue* v) {
    return v->getKind() == Kind::ProgramID;
  }
  void print(llvm::raw_ostream& os) const override;
  void print(llvm::raw_ostream& os, unsigned indent) const override;
};

//===----------------------------------------------------------------------===//
// GmPtrSV - kernel 指针类型入参 (Global Memory Pointer)
//===----------------------------------------------------------------------===//

class GmPtrSV : public ScalarSV {
  Value param;           // 关联的入参 Value
  Type pointeeType;

public:
  explicit GmPtrSV(Value p, Type pt, Operation* op = nullptr)
      : ScalarSV(Kind::GmPtr, op), param(p), pointeeType(pt) {}

  Value getParam() const { return param; }
  Type getPointeeType() const { return pointeeType; }
  Type getDataType() const override {
    return tt::PointerType::get(pointeeType, 1);
  }

  static bool classof(const SymValue* v) {
    return v->getKind() == Kind::GmPtr;
  }
  void print(llvm::raw_ostream& os) const override;
  void print(llvm::raw_ostream& os, unsigned indent) const override;
};

//===----------------------------------------------------------------------===//
// RemExprSV - rem (取模) 操作产生的表达式
//===----------------------------------------------------------------------===//

class RemExprSV : public ScalarSV {
  std::shared_ptr<ScalarSV> lhs;
  std::shared_ptr<ScalarSV> rhs;
  Type dataType;

public:
  RemExprSV(std::shared_ptr<ScalarSV> l, std::shared_ptr<ScalarSV> r,
            Type type, Operation* op = nullptr);

  ScalarSV* getLHS() const { return lhs.get(); }
  ScalarSV* getRHS() const { return rhs.get(); }
  Type getDataType() const override { return dataType; }

  static bool classof(const SymValue* v) {
    return v->getKind() == Kind::RemExpr;
  }
  void print(llvm::raw_ostream& os) const override;
  void print(llvm::raw_ostream& os, unsigned indent) const override;
};

//===----------------------------------------------------------------------===//
// UnknownSV - 未知 SymValue（如 load 产生的结果）
//===----------------------------------------------------------------------===//

class UnknownSV : public ScalarSV {
  Type dataType;

public:
  explicit UnknownSV(Type type, Operation* op = nullptr)
      : ScalarSV(Kind::Unknown, op), dataType(type) {}

  Type getDataType() const override { return dataType; }

  static bool classof(const SymValue* v) {
    return v->getKind() == Kind::Unknown;
  }
  void print(llvm::raw_ostream& os) const override;
  void print(llvm::raw_ostream& os, unsigned indent) const override;
};

//===----------------------------------------------------------------------===//
// InductionSV - for 循环迭代变量
//===----------------------------------------------------------------------===//

class InductionSV : public ScalarSV {
  Instruction* forInst;
  std::shared_ptr<ScalarSV> init;
  std::shared_ptr<ScalarSV> end;
  std::shared_ptr<ScalarSV> step;
  Type dataType;

public:
  InductionSV(Instruction* inst,
              std::shared_ptr<ScalarSV> i,
              std::shared_ptr<ScalarSV> e,
              std::shared_ptr<ScalarSV> s,
              Type type, Operation* op = nullptr)
      : ScalarSV(Kind::Induction, op),
        forInst(inst), init(i), end(e), step(s), dataType(type) {}

  Instruction* getForInst() const { return forInst; }
  ScalarSV* getInit() const { return init.get(); }
  ScalarSV* getEnd() const { return end.get(); }
  ScalarSV* getStep() const { return step.get(); }
  Type getDataType() const override { return dataType; }

  static bool classof(const SymValue* v) {
    return v->getKind() == Kind::Induction;
  }
  void print(llvm::raw_ostream& os) const override;
  void print(llvm::raw_ostream& os, unsigned indent) const override;
};

//===----------------------------------------------------------------------===//
// IterArgSV - for 的 iter_arg
//===----------------------------------------------------------------------===//

class IterArgSV : public ScalarSV {
  Instruction* forInst;
  Instruction* initInst;
  Instruction* yieldDefInst;
  Type dataType;

public:
  IterArgSV(Instruction* forI, Instruction* initI, Instruction* yieldI,
            Type type, Operation* op = nullptr)
      : ScalarSV(Kind::IterArg, op),
        forInst(forI), initInst(initI), yieldDefInst(yieldI), dataType(type) {}

  Instruction* getForInst() const { return forInst; }
  Instruction* getInitInst() const { return initInst; }
  Instruction* getYieldDefInst() const { return yieldDefInst; }
  Type getDataType() const override { return dataType; }

  static bool classof(const SymValue* v) {
    return v->getKind() == Kind::IterArg;
  }
  void print(llvm::raw_ostream& os) const override;
  void print(llvm::raw_ostream& os, unsigned indent) const override;
};

//===----------------------------------------------------------------------===//
// ArgSV - 非指针类型的 kernel 入参
//===----------------------------------------------------------------------===//
class ArgSV : public ScalarSV {
  unsigned argIndex;    // 参数索引
  Type dataType;        // 参数类型
  std::string name;     // 参数名（可选）

public:
  ArgSV(unsigned idx, Type type, Operation* op = nullptr,
        const std::string& n = "")
      : ScalarSV(Kind::Arg, op), argIndex(idx), dataType(type), name(n) {}

  unsigned getArgIndex() const { return argIndex; }
  Type getDataType() const override { return dataType; }
  const std::string& getName() const { return name; }

  bool hasName() const { return !name.empty(); }

  static bool classof(const SymValue* v) {
    return v->getKind() == Kind::Arg;
  }
  void print(llvm::raw_ostream& os) const override;
  void print(llvm::raw_ostream& os, unsigned indent) const override;
};

//===----------------------------------------------------------------------===//
// TensorSV - 张量 (含 Element ScalarSV)
//===----------------------------------------------------------------------===//

class TensorSV : public SymValue {
public:
  enum class SourceKind {
    MakeRange,
    Splat,
    ExpandDims,
    Broadcast,
    // 元素级运算类型
    Add,
    Sub,
    Mul,
    Div,
    Rem,
    CmpEQ, CmpNE, CmpLT, CmpLE, CmpGT, CmpGE,
    Select,
    Load,
    Computed,
  };

  // 构造函数（用于 make_shared）
  TensorSV(SourceKind src, ArrayRef<int64_t> s, Type elemType,
           Operation* op = nullptr);

private:
  SourceKind source;
  SmallVector<int64_t> shape;
  Type elementType;

public:
  std::shared_ptr<ScalarSV> elementExpr;  // Element ScalarSV (public for direct access)
  // 工厂方法
  static std::shared_ptr<TensorSV> createMakeRange(
      int64_t start, int64_t end, Type elemType, Operation* op = nullptr);

  static std::shared_ptr<TensorSV> createSplat(
      std::shared_ptr<ScalarSV> val,
      ArrayRef<int64_t> shape, Type elemType, Operation* op = nullptr);

  static std::shared_ptr<TensorSV> createExpandDims(
      const TensorSV* input, int axis, Operation* op = nullptr);

  static std::shared_ptr<TensorSV> createBroadcast(
      const TensorSV* input, ArrayRef<int64_t> shape, Operation* op = nullptr);

  static std::shared_ptr<TensorSV> createComputed(
      SourceKind op, const TensorSV* lhs, const TensorSV* rhs,
      Operation* mlirOp = nullptr);

  /// 创建 Select Tensor（arith.select 的 Tensor 版本）
  static std::shared_ptr<TensorSV> createSelect(
      const TensorSV* trueTensor,
      const TensorSV* falseTensor,
      std::shared_ptr<CmpExprSV> condition, Operation* op = nullptr);

  /// 创建 Load Tensor（elementExpr 为 UnknownSV）
  static std::shared_ptr<TensorSV> createLoad(
      ArrayRef<int64_t> shape, Type elemType, Operation* op = nullptr);

  SourceKind getSource() const { return source; }
  ArrayRef<int64_t> getShape() const { return shape; }
  Type getElementType() const { return elementType; }
  ScalarSV* getElementExpr() const { return elementExpr.get(); }

  // 更新 elementExpr 的 dims
  void updateElementDims();

  static bool classof(const SymValue* v) {
    return v->getKind() == Kind::Tensor;
  }
  void print(llvm::raw_ostream& os) const override;
  void print(llvm::raw_ostream& os, unsigned indent) const override;
};

//===----------------------------------------------------------------------===//
// 辅助函数
//===----------------------------------------------------------------------===//

// 类型检查
inline bool isScalar(const SymValue* v) { return v && v->isScalar(); }
inline bool isTensor(const SymValue* v) { return v && v->isTensor(); }

// 获取整数常量值
std::optional<int64_t> getConstantInt(const SymValue* sv);

// 获取浮点常量值
std::optional<double> getConstantFloat(const SymValue* sv);

} // namespace ascend
} // namespace triton
} // namespace mlir

#endif
