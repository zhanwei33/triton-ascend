/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */

#ifndef TRITON_TO_GRAPH_LOAD_PATTERN_ANALYZER_H
#define TRITON_TO_GRAPH_LOAD_PATTERN_ANALYZER_H

#include "TritonToGraph/SymValue.h"
#include "TritonToGraph/SymbolicExecution.h"
#include "triton/Dialect/Triton/IR/Dialect.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/raw_ostream.h"

namespace mlir {
namespace triton {
namespace ascend {

//===----------------------------------------------------------------------===//
// Offset Pattern Recognition
//===----------------------------------------------------------------------===//

enum class OffsetPatternKind {
  Unknown,
  Constant,           // Constant offset
  ProgramID,          // pid.x/y/z
  Range,              // arange(start, end)
  Linear,             // base + stride * idx
  AddExpr,            // offset1 + offset2
  MinTruncation,      // select(idx < bound, idx, 0)
  Broadcast           // splat value
};

struct OffsetPattern {
  OffsetPatternKind kind = OffsetPatternKind::Unknown;
  int64_t constantValue = 0;
  int axis = 0;                           // For ProgramID (0=x, 1=y, 2=z)
  int64_t rangeStart = 0, rangeEnd = 0;   // For Range
  ScalarSV* base = nullptr;              // For Linear
  int64_t stride = 0;                    // For Linear
  ScalarSV* idx = nullptr;               // For Linear/Range
  ScalarSV* rangeExpr = nullptr;         // The range expression itself

  bool isContiguous() const {
    return kind == OffsetPatternKind::Range ||
           (kind == OffsetPatternKind::Linear && stride == 1);
  }
};

//===----------------------------------------------------------------------===//
// Tensor Access Information
//===----------------------------------------------------------------------===//

struct TensorAccessInfo {
  //===---------------------------------------------------------------------===
  // Basic Information
  //===---------------------------------------------------------------------===
  Value basePtr;                        // Original base pointer
  Type basePtrType;                     // Base pointer type
  ScalarSV* baseOffset = nullptr;      // Base address offset

  //===---------------------------------------------------------------------===
  // Shape Information
  //===---------------------------------------------------------------------===
  SmallVector<int64_t> shape;           // Access tensor shape
  SmallVector<int64_t> strides;         // Dimension strides (element count)
  SmallVector<int64_t> blockShape;      // Block shape for block pointers
  Type elementType;                     // Element data type

  //===---------------------------------------------------------------------===
  // Pointer Information (new)
  //===---------------------------------------------------------------------===
  std::shared_ptr<TensorPtrSV> tensorPtr;  // make_tensor_ptr result
  std::shared_ptr<PtrExprSV> ptrExpr;      // addptr result
  std::shared_ptr<GmPtrSV> gmPtr;          // Kernel parameter pointer
  bool isBlockPtr = false;                 // Whether using block pointer
  SmallVector<std::shared_ptr<ScalarSV>> offsetExprs;  // Per-dimension offsets

  //===---------------------------------------------------------------------===
  // Contiguity Analysis
  //===---------------------------------------------------------------------===
  bool isRowContiguous = false;         // Row major contiguous
  bool isColContiguous = false;         // Column major contiguous
  int contiguousAxis = -1;              // Which axis is contiguous (-1 = none)

  //===---------------------------------------------------------------------===
  // Boundary Information (for min truncation detection)
  //===---------------------------------------------------------------------===
  bool hasLengthCheck = false;          // Has length check (min pattern)
  int64_t lengthBound = 0;              // Length bound value
  ScalarSV* rangeValue = nullptr;      // Range value for length check

  //===---------------------------------------------------------------------===
  // Mask Information
  //===---------------------------------------------------------------------===
  bool hasMask = false;                 // Has mask
  Value maskValue;                      // Mask value
  SymValue* maskSymValue = nullptr;    // Mask symbolic value

  //===---------------------------------------------------------------------===
  // Padding Information
  //===---------------------------------------------------------------------===
  bool hasPadding = false;              // Has padding value
  SymValue* paddingValue = nullptr;    // Padding symbolic value

  //===---------------------------------------------------------------------===
  // Loop Dependency
  //===---------------------------------------------------------------------===
  bool isLoopDependent = false;         // Depends on loop variable

  //===---------------------------------------------------------------------===
  // Access Pattern Classification
  //===---------------------------------------------------------------------===
  enum class AccessPattern {
    Unknown,           // Unknown
    ScalarSequential,  // Scalar sequential access (e.g., varlen sequence length)
    TensorContiguous,  // Tensor contiguous access
    TensorStrided,     // Tensor strided access
    GatherContiguous,  // Gather contiguous (128-element gather)
    GatherStrided,     // Gather strided
    LoopDependent,     // Loop-dependent access (e.g., K in for loop)
  };
  AccessPattern pattern = AccessPattern::Unknown;

  //===---------------------------------------------------------------------===
  // Constructors
  //===---------------------------------------------------------------------===
  TensorAccessInfo()
      : basePtrType(nullptr),
        elementType(nullptr),
        contiguousAxis(-1),
        hasLengthCheck(false),
        lengthBound(0),
        rangeValue(nullptr),
        hasMask(false),
        maskSymValue(nullptr),
        hasPadding(false),
        paddingValue(nullptr),
        isLoopDependent(false),
        pattern(AccessPattern::Unknown) {}

  //===---------------------------------------------------------------------===
  // Methods
  //===---------------------------------------------------------------------===

  /// Calculate total element count
  int64_t getNumElements() const;

  /// Calculate total bytes
  int64_t getTotalBytes() const;

  /// Get rank (number of dimensions)
  size_t getRank() const { return shape.size(); }

  /// Print analysis result
  void print(llvm::raw_ostream& os) const;

  /// Output as YAML format
  void printYAML(llvm::raw_ostream& os) const;

  /// Check if scalar access
  bool isScalarAccess() const { return shape.empty(); }

  /// Check if 2D access
  bool is2DAccess() const { return shape.size() == 2; }

  /// Get memory layout description
  StringRef getLayoutDescription() const;
};

//===----------------------------------------------------------------------===//
// Load Pattern Analyzer
//===----------------------------------------------------------------------===//

class LoadPatternAnalyzer {
public:
  LoadPatternAnalyzer() = default;

  //===---------------------------------------------------------------------===
  // Main Analysis Interface
  //===---------------------------------------------------------------------===

  /// Analyze load instruction access pattern
  /// @param loadOp Load operation to analyze
  /// @param state Symbolic execution state (contains all SymValues)
  /// @return TensorAccessInfo Complete access information
  TensorAccessInfo analyzeLoad(tt::LoadOp loadOp,
                               const SymbolicExecutionState& state);

  //===---------------------------------------------------------------------===
  // Offset Pattern Analysis (new)
  //===---------------------------------------------------------------------===

  /// Recursively analyze offset expression structure
  /// Recognizes patterns like: pid * stride + arange(0, N)
  OffsetPattern analyzeOffsetPattern(ScalarSV* offset);

  /// Check if offset pattern represents contiguous access
  bool isContiguousOffsetPattern(const OffsetPattern& pattern);

private:
  //===---------------------------------------------------------------------===
  // Pointer Type Dispatch (refactored)
  //===---------------------------------------------------------------------===

  /// Analyze TensorPtrSV (make_tensor_ptr result)
  TensorAccessInfo analyzeTensorPtr(
      std::shared_ptr<TensorPtrSV> tensorPtr,
      tt::LoadOp loadOp);

  /// Analyze PtrExprSV (addptr result)
  TensorAccessInfo analyzePtrExpr(
      std::shared_ptr<PtrExprSV> ptrExpr,
      tt::LoadOp loadOp);

  /// Analyze GmPtrSV (kernel parameter)
  TensorAccessInfo analyzeGmPtr(
      std::shared_ptr<GmPtrSV> gmPtr,
      tt::LoadOp loadOp);

  //===---------------------------------------------------------------------===
  // Offset Analysis (based on new ScalarSV hierarchy)
  //===---------------------------------------------------------------------===

  /// Analyze offsets from TensorPtrSV
  void analyzeTensorPtrOffsets(
      TensorPtrSV* tensorPtr,
      TensorAccessInfo& info);

  /// Analyze offset from PtrExprSV
  void analyzePtrExprOffset(
      PtrExprSV* ptrExpr,
      TensorAccessInfo& info);

  //===---------------------------------------------------------------------===
  // Pattern Recognition Helpers
  //===---------------------------------------------------------------------===

  /// Recognize min truncation pattern in offset
  /// Pattern: select(cmp_lt(idx, bound), idx, const)
  bool detectMinTruncationInOffset(
      ScalarSV* offset,
      int64_t& bound,
      ScalarSV*& range);

  /// Recognize min truncation pattern in TensorPtrSV offsets
  void detectMinTruncationInOffset(
      TensorPtrSV* tensorPtr,
      TensorAccessInfo& info);

  /// Check if SelectExprSV is min pattern
  bool isMinSelectPattern(SelectExprSV* select);

  /// Check if SelectExprSV is max pattern
  bool isMaxSelectPattern(SelectExprSV* select);

  /// Check if SelectExprSV is length check pattern
  bool isLengthCheckPattern(SelectExprSV* select);

  //===---------------------------------------------------------------------===
  // Contiguity Analysis
  //===---------------------------------------------------------------------===

  /// Analyze access contiguity
  void analyzeContiguity(TensorAccessInfo& info);

  /// Check row-major contiguity (last dimension contiguous)
  bool isRowMajorContiguous(ArrayRef<int64_t> shape,
                            ArrayRef<int64_t> strides);

  /// Check column-major contiguity (first dimension contiguous)
  bool isColMajorContiguous(ArrayRef<int64_t> shape,
                            ArrayRef<int64_t> strides);

  //===---------------------------------------------------------------------===
  // Stride Inference
  //===---------------------------------------------------------------------===

  /// Infer strides from offset expressions
  SmallVector<int64_t> inferStridesFromOffsets(
      ArrayRef<std::shared_ptr<ScalarSV>> offsets,
      ArrayRef<int64_t> shape);

  /// Try to extract constant stride from offset expression
  std::optional<int64_t> extractStrideFromOffset(ScalarSV* offset);

  //===---------------------------------------------------------------------===
  // Mask/Padding Analysis
  //===---------------------------------------------------------------------===

  /// Analyze load mask operand
  void analyzeMask(tt::LoadOp loadOp,
                   TensorAccessInfo& info,
                   const SymbolicExecutionState& state);

  /// Analyze load padding value
  void analyzePadding(tt::LoadOp loadOp,
                      TensorAccessInfo& info,
                      const SymbolicExecutionState& state);

  /// Extract bound from mask (CmpExprSV pattern)
  bool extractBoundFromMask(
      SymValue* maskSym,
      int64_t& bound,
      ScalarSV*& range);

  /// Extract bound from comparison expression
  bool extractBoundFromCmp(CmpExprSV* cmp, int64_t& bound, ScalarSV*& idx);

  //===---------------------------------------------------------------------===
  // Loop Dependency Analysis
  //===---------------------------------------------------------------------===

  /// Check if load is in a loop and depends on loop variable
  bool isLoopDependent(tt::LoadOp loadOp,
                       const SymbolicExecutionState& state);

  /// Analyze loop dependency details
  void analyzeLoopDependency(tt::LoadOp loadOp,
                             TensorAccessInfo& info,
                             const SymbolicExecutionState& state);

  //===---------------------------------------------------------------------===
  // Access Pattern Classification
  //===---------------------------------------------------------------------===

  /// Classify final access pattern
  void classifyAccessPattern(TensorAccessInfo& info);

  //===---------------------------------------------------------------------===
  // Helper Methods
  //===---------------------------------------------------------------------===

  /// Get element type size in bytes
  int64_t getElementTypeSize(Type type) const;

  /// Compute linear index from indices and strides
  int64_t computeLinearIndex(ArrayRef<int64_t> indices,
                            ArrayRef<int64_t> strides) const;

  /// Check if SymValue contains or is a ProgramID
  bool containsProgramID(ScalarSV* sv);

  /// Check if SymValue contains or is a RangeExpr
  bool containsRangeExpr(ScalarSV* sv);
};

} // namespace ascend
} // namespace triton
} // namespace mlir

#endif // TRITON_TO_GRAPH_LOAD_PATTERN_ANALYZER_H
