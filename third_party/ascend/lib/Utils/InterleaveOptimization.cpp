/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "ascend/include/Utils/InterleaveOptimization.h"
#include "ascend/include/Utils/Utils.h"
#include "mlir/Dialect/Bufferization/IR/Bufferization.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/Dialect/Utils/StaticValueUtils.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinTypeInterfaces.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "mlir/Interfaces/ViewLikeInterface.h"
#include "mlir/Support/LogicalResult.h"

#include "mlir/IR/Operation.h"
#include "triton/Dialect/Triton/IR/Dialect.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Debug.h"
#include <cassert>
#include <limits>
#include <optional>
#include <utility>

namespace mlir {
namespace triton {
// For origin MemRefType of ReinterpretCastOp under interleave state, here wanna
// adjust its shape info by expanding last dimension double.
static MemRefType expandInterleaveMemRefType(
    MemRefType originType,
    std::optional<OpFoldResult> castOffset = std::nullopt) {
  // Double the last dimension shape
  SmallVector<int64_t> shape(originType.getShape());
  shape.back() = shape.back() * 2;

  // Adjuest layout attribute
  StridedLayoutAttr originLayout =
      llvm::dyn_cast<StridedLayoutAttr>(originType.getLayout());
  // If offset is static, just reset it to 0.
  auto offset = originLayout.getOffset() == ShapedType::kDynamic
                    ? originLayout.getOffset()
                    : 0;
  // A reinterpret_cast result type must agree with its offset operand.  The
  // original interleaved view can carry layout offset 0 while the normalized
  // base is a non-zero static offset, so use the new cast offset whenever it
  // is available.
  if (castOffset) {
    if (auto staticOffset = getConstantIntValue(*castOffset))
      offset = staticOffset.value();
    else
      offset = ShapedType::kDynamic;
  }
  // Set last dimension stride to 1
  SmallVector<int64_t> stride(originLayout.getStrides());
  stride.back() = 1;

  return MemRefType::get(
      shape, originType.getElementType(),
      StridedLayoutAttr::get(originType.getContext(), offset, stride));
}

// The last dimension of an interleaved view has stride 2.  The address can be
// expressed as an arbitrary base plus a lane selector (0 for even, 1 for odd):
//
//   base + 2 * lane + {0, 1}
//
// The base is allowed to contain static and dynamic offsets.  In particular,
// a constant such as the RoPE region offset (+64) is part of the base, not an
// odd-lane marker.  Keep the analysis side-effect free so callers can safely
// return failure and use the normal lowering path.
struct DeinterleaveOffsetInfo {
  SmallVector<Value> baseTerms;
  int64_t baseConstant = 0;
  IndexMode indexMode = IndexMode::EVEN_MODE;
  // Reuse the original SSA base whenever the outermost +1 unambiguously
  // selects the odd lane. This preserves the existing offset def-use chain
  // instead of recreating an equivalent arithmetic expression during type
  // conversion.
  std::optional<OpFoldResult> materializedBaseOffset;
};

static bool addStaticOffset(int64_t offset, int64_t &sum) {
  if ((offset > 0 && sum > std::numeric_limits<int64_t>::max() - offset) ||
      (offset < 0 && sum < std::numeric_limits<int64_t>::min() - offset))
    return false;
  sum += offset;
  return true;
}

static bool collectDeinterleaveOffsetTerms(OpFoldResult offset,
                                           SmallVectorImpl<Value> &baseTerms,
                                           int64_t &staticOffset) {
  if (auto constant = getConstantIntValue(offset))
    return addStaticOffset(constant.value(), staticOffset);

  auto value = llvm::dyn_cast<Value>(offset);
  if (!value)
    return false;

  if (auto addOp = value.getDefiningOp<arith::AddIOp>()) {
    return collectDeinterleaveOffsetTerms(addOp.getLhs(), baseTerms,
                                          staticOffset) &&
           collectDeinterleaveOffsetTerms(addOp.getRhs(), baseTerms,
                                          staticOffset);
  }

  // The expression below this value is an opaque base term.  Do not recurse
  // into it: a constant in a multiply/addptr/base-address subgraph is not a
  // lane selector.
  baseTerms.push_back(value);
  return true;
}

static FailureOr<DeinterleaveOffsetInfo>
normalizeDeinterleaveOffset(OpFoldResult originOffset) {
  DeinterleaveOffsetInfo result;
  int64_t staticOffset = 0;
  if (!collectDeinterleaveOffsetTerms(originOffset, result.baseTerms,
                                      staticOffset))
    return failure();

  for (Value term : result.baseTerms) {
    if (!isa<IndexType>(term.getType()))
      return failure();
  }

  // Keep the base pair-aligned relative to the original view.  This also
  // supports folded constants, e.g. +65 becomes base +64 and ODD_MODE.
  int64_t laneOffset = staticOffset % 2;
  if (laneOffset < 0)
    laneOffset += 2;
  result.baseConstant = staticOffset - laneOffset;
  result.indexMode =
      laneOffset == 0 ? IndexMode::EVEN_MODE : IndexMode::ODD_MODE;

  if (auto value = llvm::dyn_cast<Value>(originOffset)) {
    Value base = value;
    if (result.indexMode == IndexMode::ODD_MODE) {
      auto addOp = value.getDefiningOp<arith::AddIOp>();
      if (addOp) {
        if (auto lhsConstant = getConstantIntValue(addOp.getLhs());
            lhsConstant && lhsConstant.value() == 1) {
          base = addOp.getRhs();
          result.materializedBaseOffset = base;
        } else if (auto rhsConstant = getConstantIntValue(addOp.getRhs());
                   rhsConstant && rhsConstant.value() == 1) {
          base = addOp.getLhs();
          result.materializedBaseOffset = base;
        }
      }
    } else {
      result.materializedBaseOffset = base;
    }
  }
  return result;
}

// Conversion may duplicate a pure index expression while lowering two Triton
// loads from the same program ID.  The resulting Values are distinct, e.g.
// `%pid0 * 2` and `%pid1 * 2`, even though both casts originate from the same
// scalar argument.  Permit that narrow structural equivalence here, but do
// not try to prove arbitrary operation equivalence: unsupported expressions
// stay distinct and therefore retain generic load lowering.
static bool areEquivalentDeinterleaveBaseTerm(Value lhs, Value rhs,
                                              unsigned depth = 0) {
  constexpr unsigned kMaxStructuralDepth = 16;
  if (lhs == rhs)
    return true;
  if (depth == kMaxStructuralDepth || lhs.getType() != rhs.getType())
    return false;

  auto lhsConstant = getConstantIntValue(lhs);
  auto rhsConstant = getConstantIntValue(rhs);
  if (lhsConstant || rhsConstant)
    return lhsConstant && rhsConstant &&
           lhsConstant.value() == rhsConstant.value();

  if (auto lhsCast = lhs.getDefiningOp<arith::IndexCastOp>()) {
    auto rhsCast = rhs.getDefiningOp<arith::IndexCastOp>();
    return rhsCast && areEquivalentDeinterleaveBaseTerm(
                          lhsCast.getIn(), rhsCast.getIn(), depth + 1);
  }
  if (auto lhsCast = lhs.getDefiningOp<arith::IndexCastUIOp>()) {
    auto rhsCast = rhs.getDefiningOp<arith::IndexCastUIOp>();
    return rhsCast && areEquivalentDeinterleaveBaseTerm(
                          lhsCast.getIn(), rhsCast.getIn(), depth + 1);
  }

  auto areEquivalentCommutativeBinary = [&](auto lhsOp, auto rhsOp) {
    if (!rhsOp || lhsOp->getAttrs() != rhsOp->getAttrs())
      return false;
    return (areEquivalentDeinterleaveBaseTerm(lhsOp.getLhs(), rhsOp.getLhs(),
                                              depth + 1) &&
            areEquivalentDeinterleaveBaseTerm(lhsOp.getRhs(), rhsOp.getRhs(),
                                              depth + 1)) ||
           (areEquivalentDeinterleaveBaseTerm(lhsOp.getLhs(), rhsOp.getRhs(),
                                              depth + 1) &&
            areEquivalentDeinterleaveBaseTerm(lhsOp.getRhs(), rhsOp.getLhs(),
                                              depth + 1));
  };
  if (auto lhsAdd = lhs.getDefiningOp<arith::AddIOp>())
    return areEquivalentCommutativeBinary(lhsAdd,
                                          rhs.getDefiningOp<arith::AddIOp>());
  if (auto lhsMul = lhs.getDefiningOp<arith::MulIOp>())
    return areEquivalentCommutativeBinary(lhsMul,
                                          rhs.getDefiningOp<arith::MulIOp>());
  return false;
}

static bool hasEquivalentDeinterleaveBase(const DeinterleaveOffsetInfo &lhs,
                                          const DeinterleaveOffsetInfo &rhs) {
  if (lhs.baseConstant != rhs.baseConstant ||
      lhs.baseTerms.size() != rhs.baseTerms.size())
    return false;

  // Addition is commutative.  Compare the opaque terms as a multiset so that
  // equivalent add trees with a different operand order remain optimizable.
  SmallVector<bool> matched(rhs.baseTerms.size(), false);
  for (Value lhsTerm : lhs.baseTerms) {
    bool found = false;
    for (auto [index, rhsTerm] : llvm::enumerate(rhs.baseTerms)) {
      if (!matched[index] &&
          areEquivalentDeinterleaveBaseTerm(lhsTerm, rhsTerm)) {
        matched[index] = true;
        found = true;
        break;
      }
    }
    if (!found)
      return false;
  }
  return true;
}

static OpFoldResult
materializeDeinterleaveBaseOffset(const DeinterleaveOffsetInfo &offsetInfo,
                                  OpBuilder &builder, Location loc) {
  if (offsetInfo.materializedBaseOffset)
    return *offsetInfo.materializedBaseOffset;
  if (offsetInfo.baseTerms.empty())
    return builder.getIndexAttr(offsetInfo.baseConstant);

  Value base = offsetInfo.baseTerms.front();
  for (Value term : llvm::drop_begin(offsetInfo.baseTerms))
    base = builder.create<arith::AddIOp>(loc, base, term);

  if (offsetInfo.baseConstant != 0) {
    Value constant = builder.create<arith::ConstantOp>(
        loc, builder.getIndexAttr(offsetInfo.baseConstant));
    base = builder.create<arith::AddIOp>(loc, base, constant);
  }
  return base;
}

struct DeinterleaveLoadCandidate {
  bufferization::ToTensorOp toTensor;
  memref::AllocOp alloc;
  memref::CopyOp copy;
  memref::ReinterpretCastOp reinterpretCast;
  DeinterleaveOffsetInfo offsetInfo;
};

// The generic unmasked load lowering has exactly this local chain:
//
//   reinterpret_cast(stride=2) -> memref.copy -> alloc -> to_tensor
//
// Do not recognize looser forms here.  In particular, a shared alloc, a fill,
// or a subview belongs to another lowering contract and must stay generic.
static FailureOr<DeinterleaveLoadCandidate>
parseDeinterleaveLoadCandidate(bufferization::ToTensorOp toTensor) {
  Value allocValue = toTensor->getOperand(0);
  auto alloc = allocValue.getDefiningOp<memref::AllocOp>();
  if (!alloc)
    return failure();

  memref::CopyOp copy = nullptr;
  for (Operation *user : allocValue.getUsers()) {
    if (user == toTensor.getOperation())
      continue;
    auto candidateCopy = dyn_cast<memref::CopyOp>(user);
    if (!candidateCopy || candidateCopy.getTarget() != allocValue || copy)
      return failure();
    copy = candidateCopy;
  }
  if (!copy)
    return failure();

  auto reinterpretCast =
      copy.getSource().getDefiningOp<memref::ReinterpretCastOp>();
  if (!reinterpretCast || copy.getSource() != reinterpretCast.getResult())
    return failure();

  MemRefType sourceType = reinterpretCast.getType();
  if (sourceType.getRank() == 0 ||
      sourceType.getShape().back() == ShapedType::kDynamic ||
      sourceType.getShape().back() % 2 != 0)
    return failure();
  auto stridesAndOffset = sourceType.getStridesAndOffset();
  if (stridesAndOffset.first.back() != 2)
    return failure();

  auto offsetInfo =
      normalizeDeinterleaveOffset(reinterpretCast.getConstifiedMixedOffset());
  if (failed(offsetInfo))
    return failure();

  return DeinterleaveLoadCandidate{toTensor, alloc, copy, reinterpretCast,
                                   std::move(*offsetInfo)};
}

static bool isBetweenCopiesMemorySafe(Operation *first, Operation *second) {
  if (first->getBlock() != second->getBlock() ||
      !first->isBeforeInBlock(second))
    return false;

  bool isBetween = false;
  for (Operation &op : *first->getBlock()) {
    if (&op == first) {
      isBetween = true;
      continue;
    }
    if (&op == second)
      return true;
    if (!isBetween)
      continue;

    // The second generic load allocates its private destination between the
    // two copies.  Allocation and to_tensor do not alter the source window;
    // every other memory-affecting or unknown operation is a barrier.
    if (isa<memref::AllocOp, bufferization::ToTensorOp>(&op))
      continue;
    if (!isMemoryEffectFree(&op))
      return false;
  }
  return false;
}

static bool arePairableDeinterleaveLoads(DeinterleaveLoadCandidate &first,
                                         DeinterleaveLoadCandidate &second) {
  if (first.reinterpretCast.getViewSource() !=
          second.reinterpretCast.getViewSource() ||
      first.toTensor.getResult().getType() !=
          second.toTensor.getResult().getType() ||
      !isEqualConstantIntOrValueArray(
          first.reinterpretCast.getConstifiedMixedSizes(),
          second.reinterpretCast.getConstifiedMixedSizes()) ||
      !isEqualConstantIntOrValueArray(
          first.reinterpretCast.getConstifiedMixedStrides(),
          second.reinterpretCast.getConstifiedMixedStrides()) ||
      !hasEquivalentDeinterleaveBase(first.offsetInfo, second.offsetInfo) ||
      first.offsetInfo.indexMode == second.offsetInfo.indexMode)
    return false;

  return isBetweenCopiesMemorySafe(first.copy, second.copy) ||
         isBetweenCopiesMemorySafe(second.copy, first.copy);
}

static Value createDeinterleaveSlice(Value source, MemRefType sourceType,
                                     IndexMode indexMode, OpBuilder &builder,
                                     Location loc) {
  SmallVector<OpFoldResult> offsets(sourceType.getRank(),
                                    builder.getIndexAttr(0));
  SmallVector<OpFoldResult> strides(sourceType.getRank(),
                                    builder.getIndexAttr(1));
  SmallVector<OpFoldResult> sizes = llvm::to_vector(
      llvm::map_range(sourceType.getShape(), [&](int64_t dim) -> OpFoldResult {
        return builder.getIndexAttr(dim);
      }));
  offsets.back() =
      builder.getIndexAttr(indexMode == IndexMode::ODD_MODE ? 1 : 0);
  strides.back() = builder.getIndexAttr(2);
  sizes.back() = builder.getIndexAttr(sourceType.getShape().back() / 2);
  return builder
      .create<tensor::ExtractSliceOp>(loc, source, offsets, sizes, strides)
      .getResult();
}

static LogicalResult
optimizeDeinterleaveLoadPair(DeinterleaveLoadCandidate &lhs,
                             DeinterleaveLoadCandidate &rhs) {
  if (!arePairableDeinterleaveLoads(lhs, rhs))
    return failure();

  DeinterleaveLoadCandidate *first = &lhs;
  DeinterleaveLoadCandidate *second = &rhs;
  if (second->copy->isBeforeInBlock(first->copy))
    std::swap(first, second);

  OpBuilder builder(first->copy);
  Location loc = first->copy.getLoc();
  auto castSizes = first->reinterpretCast.getConstifiedMixedSizes();
  auto castStrides = first->reinterpretCast.getConstifiedMixedStrides();
  auto lastDim = getConstantIntValue(castSizes.back());
  if (!lastDim)
    return failure();
  castSizes.back() = builder.getIndexAttr(lastDim.value() * 2);
  castStrides.back() = builder.getIndexAttr(1);

  OpFoldResult baseOffset =
      materializeDeinterleaveBaseOffset(first->offsetInfo, builder, loc);
  MemRefType expandedType =
      expandInterleaveMemRefType(first->reinterpretCast.getType(), baseOffset);
  auto expandedCast = builder.create<memref::ReinterpretCastOp>(
      loc, expandedType, first->reinterpretCast.getViewSource(), baseOffset,
      castSizes, castStrides);
  auto expandedAlloc = builder.create<memref::AllocOp>(
      loc,
      MemRefType::get(expandedType.getShape(), expandedType.getElementType()));
  builder.create<memref::CopyOp>(loc, expandedCast, expandedAlloc);
  auto expandedTensor = builder.create<bufferization::ToTensorOp>(
      loc,
      RankedTensorType::get(expandedType.getShape(),
                            expandedType.getElementType()),
      expandedAlloc, true /* restrict */, true /* writable */);

  Value firstSlice = createDeinterleaveSlice(
      expandedTensor, expandedType, first->offsetInfo.indexMode, builder, loc);
  Value secondSlice = createDeinterleaveSlice(
      expandedTensor, expandedType, second->offsetInfo.indexMode, builder, loc);
  first->toTensor.getResult().replaceAllUsesWith(firstSlice);
  second->toTensor.getResult().replaceAllUsesWith(secondSlice);

  first->toTensor->erase();
  second->toTensor->erase();
  first->copy->erase();
  second->copy->erase();
  first->alloc->erase();
  second->alloc->erase();

  if (first->reinterpretCast == second->reinterpretCast) {
    if (first->reinterpretCast->use_empty())
      first->reinterpretCast->erase();
  } else {
    if (first->reinterpretCast->use_empty())
      first->reinterpretCast->erase();
    if (second->reinterpretCast->use_empty())
      second->reinterpretCast->erase();
  }
  return success();
}

void DeinterleaveLoadPairOptimization(Operation *root) {
  llvm::DenseMap<Value, SmallVector<DeinterleaveLoadCandidate>> candidates;
  root->walk([&](bufferization::ToTensorOp toTensor) {
    auto candidate = parseDeinterleaveLoadCandidate(toTensor);
    if (succeeded(candidate)) {
      candidates[candidate->reinterpretCast.getViewSource()].push_back(
          std::move(*candidate));
    }
  });

  for (auto &entry : candidates) {
    auto &candidateVec = entry.second;
    SmallVector<bool> consumed(candidateVec.size(), false);
    for (size_t first = 0; first < candidateVec.size(); ++first) {
      if (consumed[first])
        continue;
      for (size_t second = first + 1; second < candidateVec.size(); ++second) {
        if (consumed[second])
          continue;
        if (succeeded(optimizeDeinterleaveLoadPair(candidateVec[first],
                                                   candidateVec[second]))) {
          consumed[first] = true;
          consumed[second] = true;
          break;
        }
      }
    }
  }
}

LogicalResult
InterleaveStatusOptimization(SmallVector<Operation *> materializeVec) {
  OpBuilder builder(materializeVec[1]);
  auto loc = materializeVec[1]->getLoc();

  auto firstReinterpretCastOp =
      llvm::dyn_cast<bufferization::MaterializeInDestinationOp>(
          materializeVec[0])
          .getDest()
          .getDefiningOp<memref::ReinterpretCastOp>();
  auto secondReinterpretCastOp =
      llvm::dyn_cast<bufferization::MaterializeInDestinationOp>(
          materializeVec[1])
          .getDest()
          .getDefiningOp<memref::ReinterpretCastOp>();

  assert(firstReinterpretCastOp && secondReinterpretCastOp);
  // Judge whether two `ReinterpretCastOp` shape satisfy interleave state
  // a. both size are equal
  if (!isEqualConstantIntOrValueArray(
          firstReinterpretCastOp.getConstifiedMixedSizes(),
          secondReinterpretCastOp.getConstifiedMixedSizes())) {
    return failure();
  }
  // b. both strides are equal
  if (!isEqualConstantIntOrValueArray(
          firstReinterpretCastOp.getConstifiedMixedStrides(),
          secondReinterpretCastOp.getConstifiedMixedStrides())) {
    return failure();
  }
  // c. both offsets should satisfy tricky rule
  auto firstOriginCastOffset =
      firstReinterpretCastOp.getConstifiedMixedOffset();
  auto secondOriginCastOffset =
      secondReinterpretCastOp.getConstifiedMixedOffset();
  auto firstOffsetInfo = normalizeDeinterleaveOffset(firstOriginCastOffset);
  auto secondOffsetInfo = normalizeDeinterleaveOffset(secondOriginCastOffset);
  if (failed(firstOffsetInfo) || failed(secondOffsetInfo) ||
      !hasEquivalentDeinterleaveBase(*firstOffsetInfo, *secondOffsetInfo) ||
      firstOffsetInfo->indexMode == secondOffsetInfo->indexMode)
    return failure();

  std::pair<IndexMode, IndexMode> indexModeRecord = {
      firstOffsetInfo->indexMode, secondOffsetInfo->indexMode};
  OpFoldResult newCastOffset =
      materializeDeinterleaveBaseOffset(*firstOffsetInfo, builder, loc);

  // Create new op
  // 1. Get new destination memref type
  auto dstType = expandInterleaveMemRefType(firstReinterpretCastOp.getType(),
                                            newCastOffset);

  // 2. New tensor::EmptyOp
  auto emptyTensor = builder.create<tensor::EmptyOp>(loc, dstType.getShape(),
                                                     dstType.getElementType());

  // 3. New insert_slice from materialization source into new empty tensor
  SmallVector<OpFoldResult> insertOffsets(dstType.getRank(),
                                          builder.getIndexAttr(0));
  SmallVector<OpFoldResult> insertStrides(dstType.getRank(),
                                          builder.getIndexAttr(1));
  SmallVector<OpFoldResult> insertSizes = llvm::to_vector(
      llvm::map_range(dstType.getShape(), [&](int64_t dim) -> OpFoldResult {
        return builder.getIndexAttr(dim);
      }));
  insertStrides.back() = builder.getIndexAttr(2);
  insertSizes.back() = builder.getIndexAttr(dstType.getShape().back() / 2);
  if (indexModeRecord.first == IndexMode::ODD_MODE) {
    insertOffsets.back() = builder.getIndexAttr(1);
  } else {
    insertOffsets.back() = builder.getIndexAttr(0);
  }
  auto insertFirst = builder.create<tensor::InsertSliceOp>(
      loc,
      llvm::dyn_cast<bufferization::MaterializeInDestinationOp>(
          materializeVec[0])
          .getSource(),
      emptyTensor.getResult(), insertOffsets, insertSizes, insertStrides);

  if (indexModeRecord.second == IndexMode::ODD_MODE) {
    insertOffsets.back() = builder.getIndexAttr(1);
  } else {
    insertOffsets.back() = builder.getIndexAttr(0);
  }
  auto insertSecond = builder.create<tensor::InsertSliceOp>(
      loc,
      llvm::dyn_cast<bufferization::MaterializeInDestinationOp>(
          materializeVec[1])
          .getSource(),
      insertFirst.getResult(), insertOffsets, insertSizes, insertStrides);

  // 4. Reinterpret_cast block arg
  auto newCastSize = firstReinterpretCastOp.getConstifiedMixedSizes();
  auto newCastStride = firstReinterpretCastOp.getConstifiedMixedStrides();
  newCastSize.back() = builder.getIndexAttr(dstType.getShape().back());
  newCastStride.back() = builder.getIndexAttr(1);
  auto newCastOp = builder.create<memref::ReinterpretCastOp>(
      loc, dstType, firstReinterpretCastOp.getViewSource(), newCastOffset,
      newCastSize, newCastStride);

  // 5. Create new bufferization::MaterializeInDestinationOp
  auto newStoreOp = builder.create<bufferization::MaterializeInDestinationOp>(
      loc, insertSecond.getResult(), newCastOp.getResult());
  // Setting writable is necessary as dst is memref type
  newStoreOp.setWritable(true);

  // 6. Erase origin materialization
  materializeVec[0]->erase();
  materializeVec[1]->erase();

  return success();
}

LogicalResult
InterleaveStatusWithMaskOptimization(SmallVector<Operation *> materializeVec) {
  OpBuilder builder(materializeVec[1]);

  auto firstSubviewOpOfReCast =
      llvm::dyn_cast<bufferization::MaterializeInDestinationOp>(
          materializeVec[0])
          .getDest()
          .getDefiningOp<memref::SubViewOp>();
  auto firstSrcExtractSlice =
      llvm::dyn_cast<bufferization::MaterializeInDestinationOp>(
          materializeVec[0])
          .getSource()
          .getDefiningOp<tensor::ExtractSliceOp>();
  auto firstReinterpretCastOp = firstSubviewOpOfReCast.getSource()
                                    .getDefiningOp<memref::ReinterpretCastOp>();

  auto secondSubviewOpOfReCast =
      llvm::dyn_cast<bufferization::MaterializeInDestinationOp>(
          materializeVec[1])
          .getDest()
          .getDefiningOp<memref::SubViewOp>();
  auto secondSrcExtractSlice =
      llvm::dyn_cast<bufferization::MaterializeInDestinationOp>(
          materializeVec[1])
          .getSource()
          .getDefiningOp<tensor::ExtractSliceOp>();
  auto secondReinterpretCastOp =
      secondSubviewOpOfReCast.getSource()
          .getDefiningOp<memref::ReinterpretCastOp>();

  // 1. Both source shapes of subview and extract_slice are equal
  if (firstSubviewOpOfReCast.getSourceType().getShape() !=
      firstSrcExtractSlice.getSourceType().getShape())
    return failure();
  if (secondSubviewOpOfReCast.getSourceType().getShape() !=
      secondSrcExtractSlice.getSourceType().getShape())
    return failure();
  if (firstSubviewOpOfReCast.getSourceType().getShape() !=
      secondSubviewOpOfReCast.getSourceType().getShape())
    return failure();

  // 2. both mask state are equal
  std::function<bool(OpFoldResult, OpFoldResult)> cmpFunc =
      mlir::isEqualConstantIntOrValue;
  if (!mlir::detail::sameOffsetsSizesAndStrides(firstSubviewOpOfReCast,
                                                firstSrcExtractSlice, cmpFunc))
    return failure();
  if (!mlir::detail::sameOffsetsSizesAndStrides(secondSubviewOpOfReCast,
                                                secondSrcExtractSlice, cmpFunc))
    return failure();
  if (!mlir::detail::sameOffsetsSizesAndStrides(
          firstSubviewOpOfReCast, secondSubviewOpOfReCast, cmpFunc))
    return failure();

  // 3. Still judge whether two `ReinterpretCastOp` shape satisfy request
  // a. both size are equal
  if (!isEqualConstantIntOrValueArray(
          firstReinterpretCastOp.getConstifiedMixedSizes(),
          secondReinterpretCastOp.getConstifiedMixedSizes()))
    return failure();
  // b. both strides are equal
  if (!isEqualConstantIntOrValueArray(
          firstReinterpretCastOp.getConstifiedMixedStrides(),
          secondReinterpretCastOp.getConstifiedMixedStrides()))
    return failure();
  // c. both offsets should satisfy tricky rule
  auto firstOriginCastOffset =
      firstReinterpretCastOp.getConstifiedMixedOffset();
  auto secondOriginCastOffset =
      secondReinterpretCastOp.getConstifiedMixedOffset();
  auto firstOffsetInfo = normalizeDeinterleaveOffset(firstOriginCastOffset);
  auto secondOffsetInfo = normalizeDeinterleaveOffset(secondOriginCastOffset);
  if (failed(firstOffsetInfo) || failed(secondOffsetInfo) ||
      !hasEquivalentDeinterleaveBase(*firstOffsetInfo, *secondOffsetInfo) ||
      firstOffsetInfo->indexMode == secondOffsetInfo->indexMode)
    return failure();

  std::pair<IndexMode, IndexMode> indexModeRecord = {
      firstOffsetInfo->indexMode, secondOffsetInfo->indexMode};
  OpFoldResult newCastOffset = materializeDeinterleaveBaseOffset(
      *firstOffsetInfo, builder, materializeVec[1]->getLoc());
  auto loc = materializeVec[1]->getLoc();

  // Create new op
  // 1. Get new destination memref type
  auto dstType = expandInterleaveMemRefType(firstReinterpretCastOp.getType(),
                                            newCastOffset);

  // 2. New tensor::EmptyOp
  auto emptyTensor = builder.create<tensor::EmptyOp>(loc, dstType.getShape(),
                                                     dstType.getElementType());

  // 3. New insert_slice from extract_slice source into new empty tensor
  SmallVector<OpFoldResult> insertOffsets(dstType.getRank(),
                                          builder.getIndexAttr(0));
  SmallVector<OpFoldResult> insertStrides(dstType.getRank(),
                                          builder.getIndexAttr(1));
  SmallVector<OpFoldResult> insertSizes = llvm::to_vector(
      llvm::map_range(dstType.getShape(), [&](int64_t dim) -> OpFoldResult {
        return builder.getIndexAttr(dim);
      }));
  insertStrides.back() = builder.getIndexAttr(2);
  insertSizes.back() = builder.getIndexAttr(dstType.getShape().back() / 2);
  if (indexModeRecord.first == IndexMode::ODD_MODE) {
    insertOffsets.back() = builder.getIndexAttr(1);
  } else {
    insertOffsets.back() = builder.getIndexAttr(0);
  }
  auto insertFirst = builder.create<tensor::InsertSliceOp>(
      loc, firstSrcExtractSlice.getSource(), emptyTensor.getResult(),
      insertOffsets, insertSizes, insertStrides);

  if (indexModeRecord.second == IndexMode::ODD_MODE) {
    insertOffsets.back() = builder.getIndexAttr(1);
  } else {
    insertOffsets.back() = builder.getIndexAttr(0);
  }
  auto insertSecond = builder.create<tensor::InsertSliceOp>(
      loc, secondSrcExtractSlice.getSource(), insertFirst.getResult(),
      insertOffsets, insertSizes, insertStrides);

  // 4. To enable store with mask, create new extract_slice
  SmallVector<OpFoldResult> extractOffsets =
      firstSrcExtractSlice.getMixedOffsets();
  SmallVector<OpFoldResult> extractStrides =
      firstSrcExtractSlice.getMixedStrides();
  SmallVector<OpFoldResult> extractSizes = firstSrcExtractSlice.getMixedSizes();
  if (!llvm::isa<Attribute>(extractSizes.back())) {
    return failure();
  }
  extractSizes.back() = builder.getIndexAttr(
      getConstantIntValue(extractSizes.back()).value() * 2);
  auto newSrcExtractSlice = builder.create<tensor::ExtractSliceOp>(
      loc, insertSecond.getResult(), extractOffsets, extractSizes,
      extractStrides);

  // 5. Reinterpret_cast block arg
  auto newCastSize = firstReinterpretCastOp.getConstifiedMixedSizes();
  auto newCastStride = firstReinterpretCastOp.getConstifiedMixedStrides();
  newCastSize.back() = builder.getIndexAttr(dstType.getShape().back());
  newCastStride.back() = builder.getIndexAttr(1);
  auto newCastOp = builder.create<memref::ReinterpretCastOp>(
      loc, dstType, firstReinterpretCastOp.getViewSource(), newCastOffset,
      newCastSize, newCastStride);

  // 6. Create new memref::SubViewOp of above new reinterpret_cast
  // Here could reuse shape info of new extract_slice
  auto dstSubviewType = memref::SubViewOp::inferResultType(
      dstType, extractOffsets, extractSizes, extractStrides);
  auto newSubviewOpOfReCast = builder.create<memref::SubViewOp>(
      loc, llvm::cast<MemRefType>(dstSubviewType), newCastOp, extractOffsets,
      extractSizes, extractStrides);

  // 7. Create new bufferization::MaterializeInDestinationOp
  auto newStoreOp = builder.create<bufferization::MaterializeInDestinationOp>(
      loc, newSrcExtractSlice.getResult(), newSubviewOpOfReCast.getResult());
  // Setting writable is necessary as dst is memref type
  newStoreOp.setWritable(true);

  // 8. Erase origin operation
  materializeVec[0]->erase();
  materializeVec[1]->erase();
  if (firstSubviewOpOfReCast->use_empty()) {
    firstSubviewOpOfReCast->erase();
  }
  if (firstSrcExtractSlice->use_empty()) {
    firstSrcExtractSlice->erase();
  }
  if (secondSubviewOpOfReCast->use_empty()) {
    secondSubviewOpOfReCast->erase();
  }
  if (secondSrcExtractSlice->use_empty()) {
    secondSrcExtractSlice->erase();
  }

  return success();
}

} // namespace triton
} // namespace mlir
