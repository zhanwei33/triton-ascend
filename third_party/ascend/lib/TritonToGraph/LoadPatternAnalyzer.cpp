/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */

#include "TritonToGraph/LoadPatternAnalyzer.h"
#include "llvm/Support/Debug.h"
#include <optional>
#include "llvm/Support/Format.h"

#define DEBUG_TYPE "load-pattern-analyzer"

using namespace mlir;
using namespace mlir::triton;
using namespace mlir::triton::ascend;

//===----------------------------------------------------------------------===//
// Helper Functions
//===----------------------------------------------------------------------===//

/// dyn_cast helper for SymValue
namespace {
template<typename T>
T* dyn_cast_sv(SymValue* sv) {
  if (!sv) return nullptr;
  if (T::classof(sv)) return static_cast<T*>(sv);
  return nullptr;
}

template<typename T>
std::shared_ptr<T> dyn_cast_sv(std::shared_ptr<SymValue> sv) {
  if (!sv) return nullptr;
  if (T::classof(sv.get())) return std::static_pointer_cast<T>(sv);
  return nullptr;
}

/// Extract constant int from ScalarSV
std::optional<int64_t> getConstantInt(ScalarSV* sv) {
  if (auto* c = dyn_cast_sv<ScalarConstantIntSV>(sv)) {
    return c->getInt();
  }
  return std::nullopt;
}
} // anonymous namespace

//===----------------------------------------------------------------------===//
// TensorAccessInfo Methods
//===----------------------------------------------------------------------===//

int64_t TensorAccessInfo::getNumElements() const {
  if (shape.empty()) return 1;
  int64_t n = 1;
  for (auto s : shape) {
    if (s < 0) return -1; // Unknown shape
    n *= s;
  }
  return n;
}

int64_t TensorAccessInfo::getTotalBytes() const {
  int64_t elemSize = 0;
  if (elementType) {
    if (auto intType = dyn_cast<IntegerType>(elementType)) {
      elemSize = intType.getWidth() / 8;
    } else if (auto floatType = dyn_cast<FloatType>(elementType)) {
      elemSize = floatType.getWidth() / 8;
    }
  }
  int64_t numElems = getNumElements();
  if (numElems < 0) return -1;
  return numElems * elemSize;
}

void TensorAccessInfo::print(llvm::raw_ostream& os) const {
  os << "=== Tensor Access Info ===\n";
  os << "Base Pointer: " << basePtr << "\n";
  os << "Access Shape: [";
  for (size_t i = 0; i < shape.size(); ++i) {
    if (i > 0) os << "x";
    os << shape[i];
  }
  os << "]\n";

  os << "Strides: [";
  for (size_t i = 0; i < strides.size(); ++i) {
    if (i > 0) os << ", ";
    os << strides[i];
  }
  os << "]\n";

  os << "Block Shape: [";
  for (size_t i = 0; i < blockShape.size(); ++i) {
    if (i > 0) os << ", ";
    os << blockShape[i];
  }
  os << "]\n";

  os << "Element Type: ";
  if (elementType) {
    elementType.print(os);
  } else {
    os << "unknown";
  }
  os << "\n";

  os << "Contiguous Axis: " << contiguousAxis << "\n";
  os << "Row Contiguous: " << (isRowContiguous ? "yes" : "no") << "\n";
  os << "Col Contiguous: " << (isColContiguous ? "yes" : "no") << "\n";

  if (hasLengthCheck) {
    os << "Length Check: yes (bound=" << lengthBound << ")\n";
  }

  if (hasMask) {
    os << "Has Mask: yes\n";
  }

  if (hasPadding) {
    os << "Has Padding: yes\n";
  }

  if (isLoopDependent) {
    os << "Loop Dependent: yes\n";
  }

  if (isBlockPtr) {
    os << "Block Pointer: yes\n";
  }

  os << "Access Pattern: ";
  switch (pattern) {
    case AccessPattern::Unknown: os << "Unknown"; break;
    case AccessPattern::ScalarSequential: os << "ScalarSequential"; break;
    case AccessPattern::TensorContiguous: os << "TensorContiguous"; break;
    case AccessPattern::TensorStrided: os << "TensorStrided"; break;
    case AccessPattern::GatherContiguous: os << "GatherContiguous"; break;
    case AccessPattern::GatherStrided: os << "GatherStrided"; break;
    case AccessPattern::LoopDependent: os << "LoopDependent"; break;
  }
  os << "\n";
}

void TensorAccessInfo::printYAML(llvm::raw_ostream& os) const {
  os << "PtrInfo:\n";
  os << "  base_tensor: " << basePtr << "\n";
  os << "  base_shape: [";
  for (size_t i = 0; i < shape.size(); ++i) {
    if (i > 0) os << ", ";
    os << shape[i];
  }
  os << "]\n";

  if (baseOffset) {
    os << "  base_offset: ";
    baseOffset->print(os);
    os << "\n";
  }

  os << "  strides: [";
  for (size_t i = 0; i < strides.size(); ++i) {
    if (i > 0) os << ", ";
    os << strides[i];
  }
  os << "]\n";

  os << "  element_type: ";
  if (elementType) {
    elementType.print(os);
  }
  os << "\n";

  os << "  contiguous_axis: " << contiguousAxis << "\n";

  if (hasLengthCheck) {
    os << "  length_check:\n";
    os << "    bound: " << lengthBound << "\n";
    os << "    type: min_truncation\n";
  }

  if (hasMask) {
    os << "  mask: yes\n";
  }

  if (hasPadding) {
    os << "  padding: ";
    if (paddingValue) {
      paddingValue->print(os);
    }
    os << "\n";
  }

  os << "  pattern: ";
  switch (pattern) {
    case AccessPattern::Unknown: os << "Unknown"; break;
    case AccessPattern::ScalarSequential: os << "ScalarSequential"; break;
    case AccessPattern::TensorContiguous: os << "TensorContiguous"; break;
    case AccessPattern::TensorStrided: os << "TensorStrided"; break;
    case AccessPattern::GatherContiguous: os << "GatherContiguous"; break;
    case AccessPattern::GatherStrided: os << "GatherStrided"; break;
    case AccessPattern::LoopDependent: os << "LoopDependent"; break;
  }
  os << "\n";
}

StringRef TensorAccessInfo::getLayoutDescription() const {
  if (isRowContiguous) return "row_major_contiguous";
  if (isColContiguous) return "col_major_contiguous";
  if (contiguousAxis >= 0) return "partially_contiguous";
  return "strided";
}

//===----------------------------------------------------------------------===//
// LoadPatternAnalyzer Main Entry
//===----------------------------------------------------------------------===//

TensorAccessInfo LoadPatternAnalyzer::analyzeLoad(
    tt::LoadOp loadOp, const SymbolicExecutionState& state) {

  TensorAccessInfo info;

  // Get load pointer
  Value ptr = loadOp.getPtr();
  info.basePtr = ptr;
  info.basePtrType = ptr.getType();

  // Look up symbolic value for ptr
  std::shared_ptr<SymValue> ptrSym = state.getSymValue(ptr);

  if (!ptrSym) {
    LLVM_DEBUG(llvm::dbgs() << "No symbolic value for load ptr\n");
    return info;
  }

  // Dispatch based on pointer type
  if (auto tensorPtr = dyn_cast_sv<TensorPtrSV>(ptrSym)) {
    // make_tensor_ptr result
    info = analyzeTensorPtr(tensorPtr, loadOp);
    info.isBlockPtr = true;
  }
  else if (auto ptrExpr = dyn_cast_sv<PtrExprSV>(ptrSym)) {
    // tt.addptr result
    info = analyzePtrExpr(ptrExpr, loadOp);
  }
  else if (auto gmPtr = dyn_cast_sv<GmPtrSV>(ptrSym)) {
    // Kernel parameter pointer
    info = analyzeGmPtr(gmPtr, loadOp);
  }
  else if (auto scalarPtr = dyn_cast_sv<ScalarSV>(ptrSym)) {
    // Other scalar pointer (e.g., pure offset calculation)
    info.baseOffset = scalarPtr.get();
  }

  // Analyze mask and padding
  analyzeMask(loadOp, info, state);
  analyzePadding(loadOp, info, state);

  // Analyze loop dependency
  analyzeLoopDependency(loadOp, info, state);

  // Classify final access pattern
  classifyAccessPattern(info);

  return info;
}

//===----------------------------------------------------------------------===//
// Pointer Type Dispatch
//===----------------------------------------------------------------------===//

TensorAccessInfo LoadPatternAnalyzer::analyzeTensorPtr(
    std::shared_ptr<TensorPtrSV> tensorPtr,
    tt::LoadOp loadOp) {

  TensorAccessInfo info;
  info.tensorPtr = tensorPtr;

  // Extract shape (from symbolic expressions)
  auto shapeExprs = tensorPtr->getShape();
  info.shape.clear();
  for (auto& s : shapeExprs) {
    if (auto constInt = dyn_cast_sv<ScalarConstantIntSV>(s.get())) {
      info.shape.push_back(constInt->getInt());
    } else {
      info.shape.push_back(-1);  // Unknown dimension
    }
  }

  // Extract blockShape
  info.blockShape = SmallVector<int64_t>(tensorPtr->getBlockShape());

  // Extract element type
  info.elementType = tensorPtr->getPointeeType();

  // Analyze offsets (key for pattern recognition)
  analyzeTensorPtrOffsets(tensorPtr.get(), info);

  // Derive strides
  auto strideExprs = tensorPtr->getStrides();
  info.strides.clear();
  for (auto& s : strideExprs) {
    if (auto constInt = dyn_cast_sv<ScalarConstantIntSV>(s.get())) {
      info.strides.push_back(constInt->getInt());
    } else {
      info.strides.push_back(-1);
    }
  }

  // Analyze contiguity based on offsets and strides
  analyzeContiguity(info);

  // Detect min truncation pattern in offsets
  detectMinTruncationInOffset(tensorPtr.get(), info);

  return info;
}

TensorAccessInfo LoadPatternAnalyzer::analyzePtrExpr(
    std::shared_ptr<PtrExprSV> ptrExpr,
    tt::LoadOp loadOp) {

  TensorAccessInfo info;
  info.ptrExpr = ptrExpr;

  // Get pointee type
  info.elementType = ptrExpr->getPointeeType();

  // Analyze offset expression
  analyzePtrExprOffset(ptrExpr.get(), info);

  return info;
}

TensorAccessInfo LoadPatternAnalyzer::analyzeGmPtr(
    std::shared_ptr<GmPtrSV> gmPtr,
    tt::LoadOp loadOp) {

  TensorAccessInfo info;
  info.gmPtr = gmPtr;

  // Get pointee type
  info.elementType = gmPtr->getPointeeType();

  // GmPtr is a kernel parameter - typically scalar access or needs addptr
  info.shape.clear();
  info.strides.clear();

  return info;
}

//===----------------------------------------------------------------------===//
// Offset Analysis
//===----------------------------------------------------------------------===//

void LoadPatternAnalyzer::analyzeTensorPtrOffsets(
    TensorPtrSV* tensorPtr,
    TensorAccessInfo& info) {

  auto offsets = tensorPtr->getOffsets();

  // Store offset expressions for later analysis
  info.offsetExprs.clear();
  for (auto& off : offsets) {
    info.offsetExprs.push_back(off);
  }

  // Analyze each offset pattern
  for (auto& off : offsets) {
    OffsetPattern pattern = analyzeOffsetPattern(off.get());

    // Check for min truncation pattern
    if (pattern.kind == OffsetPatternKind::MinTruncation) {
      info.hasLengthCheck = true;
      if (pattern.constantValue > 0) {
        info.lengthBound = pattern.constantValue;
      }
      info.rangeValue = pattern.rangeExpr;
    }
  }
}

void LoadPatternAnalyzer::analyzePtrExprOffset(
    PtrExprSV* ptrExpr,
    TensorAccessInfo& info) {

  // Get the offset expression
  ScalarSV* offset = ptrExpr->getOffset();

  if (!offset) return;

  // Analyze the offset pattern
  OffsetPattern pattern = analyzeOffsetPattern(offset);

  // Store for later analysis
  info.baseOffset = offset;

  // Check for min truncation
  if (pattern.kind == OffsetPatternKind::MinTruncation) {
    info.hasLengthCheck = true;
    if (pattern.constantValue > 0) {
      info.lengthBound = pattern.constantValue;
    }
    info.rangeValue = pattern.rangeExpr;
  }
}

bool LoadPatternAnalyzer::isContiguousOffsetPattern(const OffsetPattern& pattern) {
  return pattern.isContiguous();
}

OffsetPattern LoadPatternAnalyzer::analyzeOffsetPattern(ScalarSV* offset) {
  OffsetPattern pattern;

  if (!offset) {
    pattern.kind = OffsetPatternKind::Unknown;
    return pattern;
  }

  // Check for constant integer
  if (auto constInt = dyn_cast_sv<ScalarConstantIntSV>(offset)) {
    pattern.kind = OffsetPatternKind::Constant;
    pattern.constantValue = constInt->getInt();
    return pattern;
  }

  // Check for ProgramID (pid.x/y/z)
  if (auto pid = dyn_cast_sv<ProgramIDSV>(offset)) {
    pattern.kind = OffsetPatternKind::ProgramID;
    pattern.axis = pid->getAxis();
    return pattern;
  }

  // Check for RangeExpr (arange)
  if (auto range = dyn_cast_sv<RangeExprSV>(offset)) {
    pattern.kind = OffsetPatternKind::Range;
    pattern.rangeStart = range->getStart();
    pattern.rangeEnd = range->getEnd();
    pattern.rangeExpr = range;
    return pattern;
  }

  // Check for AddExpr
  if (auto add = dyn_cast_sv<AddExprSV>(offset)) {
    auto lhsPattern = analyzeOffsetPattern(add->getLHS());
    auto rhsPattern = analyzeOffsetPattern(add->getRHS());

    // Check for linear pattern: base + stride * idx
    // or: pid * stride + range
    if ((lhsPattern.kind == OffsetPatternKind::ProgramID &&
         rhsPattern.kind == OffsetPatternKind::Range) ||
        (lhsPattern.kind == OffsetPatternKind::Range &&
         rhsPattern.kind == OffsetPatternKind::ProgramID)) {
      pattern.kind = OffsetPatternKind::Linear;
      pattern.base = add->getLHS();
      pattern.idx = add->getRHS();
      pattern.stride = 1; // Default, will be refined
    }
    else if (lhsPattern.kind == OffsetPatternKind::Constant &&
             rhsPattern.kind == OffsetPatternKind::Range) {
      // base + range pattern
      pattern.kind = OffsetPatternKind::Linear;
      pattern.constantValue = lhsPattern.constantValue;
      pattern.idx = add->getRHS();
      pattern.stride = 1;
    }
    else {
      pattern.kind = OffsetPatternKind::AddExpr;
    }

    return pattern;
  }

  // Check for MulExpr (stride * idx)
  if (auto mul = dyn_cast_sv<MulExprSV>(offset)) {
    auto lhsPattern = analyzeOffsetPattern(mul->getLHS());
    auto rhsPattern = analyzeOffsetPattern(mul->getRHS());

    // Check for stride * idx pattern
    if (lhsPattern.kind == OffsetPatternKind::Constant) {
      pattern.kind = OffsetPatternKind::Linear;
      pattern.stride = lhsPattern.constantValue;
      pattern.idx = mul->getRHS();
    }
    else if (rhsPattern.kind == OffsetPatternKind::Constant) {
      pattern.kind = OffsetPatternKind::Linear;
      pattern.stride = rhsPattern.constantValue;
      pattern.idx = mul->getLHS();
    }

    return pattern;
  }

  // Check for SelectExpr (min/max pattern)
  if (auto select = dyn_cast_sv<SelectExprSV>(offset)) {
    if (isMinSelectPattern(select)) {
      pattern.kind = OffsetPatternKind::MinTruncation;

      // Try to extract bound from the condition
      auto* cond = select->getCondition();
      if (cond) {
        if (auto boundConst = dyn_cast_sv<ScalarConstantIntSV>(cond->getRHS())) {
          pattern.constantValue = boundConst->getInt();
        }
        pattern.rangeExpr = cond->getLHS();
      }
    }
    return pattern;
  }

  // Default
  pattern.kind = OffsetPatternKind::Unknown;
  return pattern;
}

//===----------------------------------------------------------------------===//
// Min Truncation Detection
//===----------------------------------------------------------------------===//

bool LoadPatternAnalyzer::detectMinTruncationInOffset(
    ScalarSV* offset,
    int64_t& bound,
    ScalarSV*& range) {

  if (!offset) return false;

  // Check if this is a select expression
  if (auto select = dyn_cast_sv<SelectExprSV>(offset)) {
    if (isMinSelectPattern(select)) {
      auto* cond = select->getCondition();
      if (cond) {
        range = cond->getLHS();
        if (auto boundConst = dyn_cast_sv<ScalarConstantIntSV>(cond->getRHS())) {
          bound = boundConst->getInt();
          return true;
        }
      }
    }
  }

  // Recursively check sub-expressions
  if (auto add = dyn_cast_sv<AddExprSV>(offset)) {
    if (detectMinTruncationInOffset(add->getLHS(), bound, range)) return true;
    if (detectMinTruncationInOffset(add->getRHS(), bound, range)) return true;
  }
  if (auto mul = dyn_cast_sv<MulExprSV>(offset)) {
    if (detectMinTruncationInOffset(mul->getLHS(), bound, range)) return true;
    if (detectMinTruncationInOffset(mul->getRHS(), bound, range)) return true;
  }
  if (auto sub = dyn_cast_sv<SubExprSV>(offset)) {
    if (detectMinTruncationInOffset(sub->getLHS(), bound, range)) return true;
    if (detectMinTruncationInOffset(sub->getRHS(), bound, range)) return true;
  }

  return false;
}

void LoadPatternAnalyzer::detectMinTruncationInOffset(
    TensorPtrSV* tensorPtr,
    TensorAccessInfo& info) {

  if (!tensorPtr) return;

  // Check all offsets in the tensor pointer
  auto offsets = tensorPtr->getOffsets();

  for (auto& off : offsets) {
    int64_t bound = 0;
    ScalarSV* range = nullptr;

    if (detectMinTruncationInOffset(off.get(), bound, range)) {
      info.hasLengthCheck = true;
      info.lengthBound = bound;
      info.rangeValue = range;
      return;
    }
  }
}

bool LoadPatternAnalyzer::isMinSelectPattern(SelectExprSV* select) {
  if (!select) return false;

  // Use the built-in method first
  if (select->isMinPattern()) {
    return true;
  }

  // Manual check: select(cmp_lt(x, y), x, y)
  auto* cond = select->getCondition();
  if (!cond) return false;

  // Check condition is less-than comparison
  if (cond->getPred() != CmpExprSV::Pred::LT &&
      cond->getPred() != CmpExprSV::Pred::LE) {
    return false;
  }

  // Check: trueVal should be the LHS of comparison (the smaller value)
  if (select->getTrueVal() != cond->getLHS()) {
    return false;
  }

  // falseVal can be RHS of comparison or another constant
  return true;
}

bool LoadPatternAnalyzer::isMaxSelectPattern(SelectExprSV* select) {
  if (!select) return false;
  return select->isMaxPattern();
}

bool LoadPatternAnalyzer::isLengthCheckPattern(SelectExprSV* select) {
  if (!select) return false;
  return select->isLengthCheck();
}

//===----------------------------------------------------------------------===//
// Contiguity Analysis
//===----------------------------------------------------------------------===//

void LoadPatternAnalyzer::analyzeContiguity(TensorAccessInfo& info) {

  if (info.shape.empty() || info.strides.empty()) {
    info.contiguousAxis = -1;
    return;
  }

  // Check row-major contiguity (last dimension contiguous)
  info.isRowContiguous = isRowMajorContiguous(info.shape, info.strides);

  // Check column-major contiguity (first dimension contiguous)
  info.isColContiguous = isColMajorContiguous(info.shape, info.strides);

  // Determine contiguous axis
  if (info.isRowContiguous) {
    info.contiguousAxis = info.shape.size() - 1;
  } else if (info.isColContiguous) {
    info.contiguousAxis = 0;
  } else {
    // Check partial contiguity
    for (int i = info.shape.size() - 1; i >= 0; --i) {
      if (info.strides[i] == 1) {
        info.contiguousAxis = i;
        break;
      }
    }
  }
}

bool LoadPatternAnalyzer::isRowMajorContiguous(
    ArrayRef<int64_t> shape,
    ArrayRef<int64_t> strides) {

  if (shape.size() != strides.size()) return false;

  int64_t expectedStride = 1;
  for (int i = shape.size() - 1; i >= 0; --i) {
    if (strides[i] != expectedStride) {
      return false;
    }
    expectedStride *= shape[i];
  }
  return true;
}

bool LoadPatternAnalyzer::isColMajorContiguous(
    ArrayRef<int64_t> shape,
    ArrayRef<int64_t> strides) {

  if (shape.size() != strides.size()) return false;

  int64_t expectedStride = 1;
  for (size_t i = 0; i < shape.size(); ++i) {
    if (strides[i] != expectedStride) {
      return false;
    }
    expectedStride *= shape[i];
  }
  return true;
}

//===----------------------------------------------------------------------===//
// Stride Inference
//===----------------------------------------------------------------------===//

SmallVector<int64_t> LoadPatternAnalyzer::inferStridesFromOffsets(
    ArrayRef<std::shared_ptr<ScalarSV>> offsets,
    ArrayRef<int64_t> shape) {

  SmallVector<int64_t> strides(shape.size(), 1);

  if (shape.empty()) return strides;

  // Default row-major strides
  strides.back() = 1;
  for (int i = shape.size() - 2; i >= 0; --i) {
    strides[i] = strides[i + 1] * shape[i + 1];
  }

  // Try to infer from offset expressions
  for (size_t i = 0; i < offsets.size() && i < shape.size(); ++i) {
    auto strideOpt = extractStrideFromOffset(offsets[i].get());
    if (strideOpt && *strideOpt > 0) {
      strides[i] = *strideOpt;
    }
  }

  return strides;
}

std::optional<int64_t> LoadPatternAnalyzer::extractStrideFromOffset(ScalarSV* offset) {
  if (!offset) return std::nullopt;

  // Look for multiplication pattern: stride * idx
  if (auto mul = dyn_cast_sv<MulExprSV>(offset)) {
    // Check if one side is constant
    if (auto lhsConst = dyn_cast_sv<ScalarConstantIntSV>(mul->getLHS())) {
      return lhsConst->getInt();
    }
    if (auto rhsConst = dyn_cast_sv<ScalarConstantIntSV>(mul->getRHS())) {
      return rhsConst->getInt();
    }
  }

  // Look for range expression (stride = 1)
  if (auto range = dyn_cast_sv<RangeExprSV>(offset)) {
    return 1;
  }

  return std::nullopt;
}

//===----------------------------------------------------------------------===//
// Mask/Padding Analysis
//===----------------------------------------------------------------------===//

void LoadPatternAnalyzer::analyzeMask(
    tt::LoadOp loadOp,
    TensorAccessInfo& info,
    const SymbolicExecutionState& state) {

  // tt.load has optional mask and padding operands
  // Get operands - mask is typically the second-to-last operand if present
  unsigned numOperands = loadOp->getNumOperands();

  if (numOperands >= 2) {
    // Mask is usually operand 1 (after ptr)
    Value mask = loadOp->getOperand(1);
    Type maskType = mask.getType();

    // Check if it's actually a mask (i1 type or tensor<i1>)
    bool isMaskType = false;
    if (auto tensorType = dyn_cast<RankedTensorType>(maskType)) {
      if (auto intType = dyn_cast<IntegerType>(tensorType.getElementType())) {
        if (intType.getWidth() == 1) {
          isMaskType = true;
        }
      }
    } else if (auto intType = dyn_cast<IntegerType>(maskType)) {
      if (intType.getWidth() == 1) {
        isMaskType = true;
      }
    }

    if (isMaskType) {
      info.hasMask = true;
      info.maskValue = mask;

      // Try to get symbolic value for mask
      auto maskSym = state.getSymValue(mask);
      if (maskSym) {
        info.maskSymValue = maskSym.get();

        // Try to extract bound from mask
        int64_t bound = 0;
        ScalarSV* range = nullptr;
        if (extractBoundFromMask(maskSym.get(), bound, range)) {
          info.lengthBound = bound;
          info.hasLengthCheck = true;
          info.rangeValue = range;
        }
      }
    }
  }
}

void LoadPatternAnalyzer::analyzePadding(
    tt::LoadOp loadOp,
    TensorAccessInfo& info,
    const SymbolicExecutionState& state) {

  // Padding is typically the last operand
  unsigned numOperands = loadOp->getNumOperands();

  if (numOperands >= 3) {
    // Last operand is padding
    Value padding = loadOp->getOperand(numOperands - 1);

    info.hasPadding = true;

    auto paddingSym = state.getSymValue(padding);
    if (paddingSym) {
      info.paddingValue = paddingSym.get();
    }
  }
}

bool LoadPatternAnalyzer::extractBoundFromMask(
    SymValue* maskSym,
    int64_t& bound,
    ScalarSV*& range) {

  if (!maskSym) return false;

  // Check if it's a comparison expression
  if (auto cmp = dyn_cast_sv<CmpExprSV>(maskSym)) {
    return extractBoundFromCmp(cmp, bound, range);
  }

  // Check if it's a TensorSV with element expression
  if (auto tensor = dyn_cast_sv<TensorSV>(maskSym)) {
    auto* elemExpr = tensor->getElementExpr();
    if (elemExpr) {
      return extractBoundFromMask(elemExpr, bound, range);
    }
  }

  return false;
}

bool LoadPatternAnalyzer::extractBoundFromCmp(
    CmpExprSV* cmp,
    int64_t& bound,
    ScalarSV*& idx) {

  if (!cmp) return false;

  // Check for cmp_lt(idx, bound) or cmp_le(idx, bound)
  if (cmp->getPred() == CmpExprSV::Pred::LT ||
      cmp->getPred() == CmpExprSV::Pred::LE) {
    idx = cmp->getLHS();
    if (auto boundConst = dyn_cast_sv<ScalarConstantIntSV>(cmp->getRHS())) {
      bound = boundConst->getInt();
      return true;
    }
  }

  return false;
}

//===----------------------------------------------------------------------===//
// Loop Dependency Analysis
//===----------------------------------------------------------------------===//

bool LoadPatternAnalyzer::isLoopDependent(
    tt::LoadOp loadOp,
    const SymbolicExecutionState& state) {

  // Get the pointer value
  Value ptr = loadOp.getPtr();
  std::shared_ptr<SymValue> ptrSym = state.getSymValue(ptr);

  if (!ptrSym) return false;

  // Check if pointer contains InductionSV
  if (auto scalar = dyn_cast_sv<ScalarSV>(ptrSym)) {
    return containsInductionVar(scalar.get());
  }

  return false;
}

void LoadPatternAnalyzer::analyzeLoopDependency(
    tt::LoadOp loadOp,
    TensorAccessInfo& info,
    const SymbolicExecutionState& state) {

  if (isLoopDependent(loadOp, state)) {
    info.isLoopDependent = true;
  }
}

//===----------------------------------------------------------------------===//
// Helper: Check if expression contains Induction variable
//===----------------------------------------------------------------------===//

namespace {
bool containsInductionVar(ScalarSV* sv) {
  if (!sv) return false;

  if (dyn_cast_sv<InductionSV>(sv)) {
    return true;
  }

  // Recursively check sub-expressions
  if (auto add = dyn_cast_sv<AddExprSV>(sv)) {
    return containsInductionVar(add->getLHS()) ||
           containsInductionVar(add->getRHS());
  }
  if (auto mul = dyn_cast_sv<MulExprSV>(sv)) {
    return containsInductionVar(mul->getLHS()) ||
           containsInductionVar(mul->getRHS());
  }
  if (auto sub = dyn_cast_sv<SubExprSV>(sv)) {
    return containsInductionVar(sub->getLHS()) ||
           containsInductionVar(sub->getRHS());
  }
  if (auto div = dyn_cast_sv<DivExprSV>(sv)) {
    return containsInductionVar(div->getLHS()) ||
           containsInductionVar(div->getRHS());
  }
  if (auto rem = dyn_cast_sv<RemExprSV>(sv)) {
    return containsInductionVar(rem->getLHS()) ||
           containsInductionVar(rem->getRHS());
  }
  if (auto cmp = dyn_cast_sv<CmpExprSV>(sv)) {
    return containsInductionVar(cmp->getLHS()) ||
           containsInductionVar(cmp->getRHS());
  }
  if (auto select = dyn_cast_sv<SelectExprSV>(sv)) {
    return containsInductionVar(select->getTrueVal()) ||
           containsInductionVar(select->getFalseVal());
  }
  if (auto ptrExpr = dyn_cast_sv<PtrExprSV>(sv)) {
    return containsInductionVar(ptrExpr->getBasePtr()) ||
           containsInductionVar(ptrExpr->getOffset());
  }

  return false;
}
} // anonymous namespace

//===----------------------------------------------------------------------===//
// Access Pattern Classification
//===----------------------------------------------------------------------===//

void LoadPatternAnalyzer::classifyAccessPattern(TensorAccessInfo& info) {

  // 1. Check loop dependency first
  if (info.isLoopDependent) {
    info.pattern = TensorAccessInfo::AccessPattern::LoopDependent;
    return;
  }

  // 2. Check scalar access
  if (info.isScalarAccess()) {
    info.pattern = TensorAccessInfo::AccessPattern::ScalarSequential;
    return;
  }

  // 3. Classify based on contiguity
  if (info.contiguousAxis >= 0) {
    if (info.shape.size() == 1) {
      // 1D contiguous = Gather contiguous (128-element gather)
      info.pattern = TensorAccessInfo::AccessPattern::GatherContiguous;
    } else {
      info.pattern = TensorAccessInfo::AccessPattern::TensorContiguous;
    }
  } else {
    info.pattern = TensorAccessInfo::AccessPattern::TensorStrided;
  }

  // 4. Special: if has length check and 1D shape, might be varlen sequence
  if (info.hasLengthCheck && info.shape.size() == 1) {
    info.pattern = TensorAccessInfo::AccessPattern::ScalarSequential;
  }
}

//===----------------------------------------------------------------------===//
// Helper Methods
//===----------------------------------------------------------------------===//

int64_t LoadPatternAnalyzer::getElementTypeSize(Type type) const {
  if (auto intType = dyn_cast<IntegerType>(type)) {
    return intType.getWidth() / 8;
  }
  if (auto floatType = dyn_cast<FloatType>(type)) {
    return floatType.getWidth() / 8;
  }
  return 0;
}

int64_t LoadPatternAnalyzer::computeLinearIndex(
    ArrayRef<int64_t> indices,
    ArrayRef<int64_t> strides) const {

  assert(indices.size() == strides.size());

  int64_t linear = 0;
  for (size_t i = 0; i < indices.size(); ++i) {
    linear += indices[i] * strides[i];
  }
  return linear;
}

bool LoadPatternAnalyzer::containsProgramID(ScalarSV* sv) {
  if (!sv) return false;

  if (dyn_cast_sv<ProgramIDSV>(sv)) {
    return true;
  }

  // Recursively check sub-expressions
  if (auto add = dyn_cast_sv<AddExprSV>(sv)) {
    return containsProgramID(add->getLHS()) ||
           containsProgramID(add->getRHS());
  }
  if (auto mul = dyn_cast_sv<MulExprSV>(sv)) {
    return containsProgramID(mul->getLHS()) ||
           containsProgramID(mul->getRHS());
  }
  if (auto sub = dyn_cast_sv<SubExprSV>(sv)) {
    return containsProgramID(sub->getLHS()) ||
           containsProgramID(sub->getRHS());
  }
  if (auto div = dyn_cast_sv<DivExprSV>(sv)) {
    return containsProgramID(div->getLHS()) ||
           containsProgramID(div->getRHS());
  }

  return false;
}

bool LoadPatternAnalyzer::containsRangeExpr(ScalarSV* sv) {
  if (!sv) return false;

  if (dyn_cast_sv<RangeExprSV>(sv)) {
    return true;
  }

  // Recursively check sub-expressions
  if (auto add = dyn_cast_sv<AddExprSV>(sv)) {
    return containsRangeExpr(add->getLHS()) ||
           containsRangeExpr(add->getRHS());
  }
  if (auto mul = dyn_cast_sv<MulExprSV>(sv)) {
    return containsRangeExpr(mul->getLHS()) ||
           containsRangeExpr(mul->getRHS());
  }
  if (auto sub = dyn_cast_sv<SubExprSV>(sv)) {
    return containsRangeExpr(sub->getLHS()) ||
           containsRangeExpr(sub->getRHS());
  }

  return false;
}
