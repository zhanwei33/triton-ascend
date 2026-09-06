/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to do so, subject to the
 * following conditions:
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

#include "TritonToGraph/EntryArgPointerAliasAnalysis.h"
#include "TritonToGraph/GraphOptimizationRule.h"
#include "TritonToGraph/ProgramAxisDependenceAnalysis.h"
#include "TritonToGraph/ProgramGridSpecialization.h"
#include "TritonToGraph/ProgramGridTransform.h"
#include "TritonToGraph/ResourceCostModel.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Interfaces/CallInterfaces.h"
#include "triton/Dialect/Triton/IR/Dialect.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Debug.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

#define DEBUG_TYPE "graph-optimize"

using namespace mlir;
using namespace triton;
using namespace cfg;

namespace {

constexpr llvm::StringLiteral kIndependentAxisTensorizeMarkerAttr =
    "hacc.independent_axis_tensorize";
constexpr llvm::StringLiteral kCoalesceFactorAttr = "hacc.coalesce_factor";
constexpr llvm::StringLiteral kCoalesceAxisAttr = "hacc.coalesce_axis";
constexpr llvm::StringLiteral kCoalesceGridCeilDivAttr =
    "hacc.coalesce_grid_ceil_div";
constexpr llvm::StringLiteral kMergeSplitKernelName =
    "_merge_split_states_kernel";
constexpr llvm::StringLiteral kNormRopeKernelName = "_indexer_norm_rope_kernel";

// A small fixed candidate set is intentional for the MVP. Each factor is
// still scored by the shared resource model; the rule never guesses a factor
// from a target name or a post-transform grid extent.
constexpr std::array<unsigned, 3> kTensorizeFactors = {2, 4, 8};

enum class TensorizeForm : uint8_t {
  MergeSplit,
  NormRope,
};

struct IATCandidate {
  triton::FuncOp function;
  Operation *anchor = nullptr;
  TensorizeForm form = TensorizeForm::MergeSplit;
  int32_t axis = 1;
  // The split and D extents make the merge form's lane placement explicit:
  // [S, D] becomes [S, F, D], while values already reduced over S become
  // [F, D].  A single global insertion index cannot represent both shapes.
  int64_t splitExtent = 0;
  int64_t dimExtent = 0;
  int64_t logicalExtent = 0;
  unsigned factor = 1;
  CandidateEvaluation evaluation;
};

struct TensorizeReductionShape {
  int64_t splitExtent = 0;
  int64_t dimExtent = 0;
};

struct MappedValue {
  Value value;
  bool tensorized = false;
  // Position of the newly introduced F dimension in `value` when tensorized.
  // Scalars tensorize to tensor<F>, so their lane axis is always zero.
  int64_t laneAxis = 0;
};

bool multiplyNoOverflow(uint64_t lhs, uint64_t rhs, uint64_t &result) {
  return !__builtin_mul_overflow(lhs, rhs, &result);
}

bool getGridTaskProduct(const std::array<int64_t, 3> &grid, uint64_t &product) {
  product = 1;
  for (int64_t extent : grid) {
    if (extent < 1 ||
        !multiplyNoOverflow(product, static_cast<uint64_t>(extent), product))
      return false;
  }
  return true;
}

std::optional<TensorizeForm> getTensorizeForm(triton::FuncOp function) {
  const StringRef name = function.getName();
  if (name == kMergeSplitKernelName)
    return TensorizeForm::MergeSplit;
  if (name == kNormRopeKernelName)
    return TensorizeForm::NormRope;
  return std::nullopt;
}

std::optional<triton::GetProgramIdOp> findOnlyProgramId(triton::FuncOp function,
                                                        int32_t axis) {
  std::optional<triton::GetProgramIdOp> result;
  unsigned count = 0;
  function.walk([&](triton::GetProgramIdOp pid) {
    if (pid.getAxisAsInt() != axis)
      return;
    ++count;
    if (count == 1)
      result = pid;
  });
  return count == 1 ? result : std::nullopt;
}

std::optional<TensorizeReductionShape>
getExpectedReductionShape(triton::FuncOp function, TensorizeForm form) {
  std::optional<TensorizeReductionShape> matched;
  function.walk([&](triton::ReduceOp reduce) {
    if (matched || reduce.getSrcs().size() != 1 ||
        reduce.getResults().size() != 1 || reduce.getAxis() != 0)
      return;
    auto source =
        dyn_cast<RankedTensorType>(reduce.getSrcs().front().getType());
    if (!source || !source.hasStaticShape())
      return;
    Type result = reduce.getResults().front().getType();
    if (form == TensorizeForm::MergeSplit) {
      auto rankedResult = dyn_cast<RankedTensorType>(result);
      if (source.getRank() == 2 && rankedResult &&
          rankedResult.getRank() == 1 && rankedResult.hasStaticShape() &&
          source.getShape()[1] == rankedResult.getShape()[0]) {
        matched =
            TensorizeReductionShape{source.getShape()[0], source.getShape()[1]};
      }
      return;
    }
    if (source.getRank() == 1 && !isa<RankedTensorType>(result))
      matched = TensorizeReductionShape{/*splitExtent=*/0,
                                        /*dimExtent=*/source.getShape()[0]};
  });
  return matched;
}

bool hasSupportedControlFlow(triton::FuncOp function) {
  Region &body = function.getBody();
  if (!body.hasOneBlock())
    return false;
  for (Operation &operation : body.front()) {
    if (isa<CallOpInterface>(operation))
      return false;
    StringRef name = operation.getName().getStringRef();
    if (name.contains("atomic") || name.contains("barrier") ||
        name == "cf.br" || name == "cf.cond_br" || name.starts_with("scf."))
      return false;
    if (operation.getNumRegions() != 0 &&
        !isa<triton::ReduceOp, triton::ScanOp>(operation))
      return false;
  }
  return true;
}

std::optional<ProgramGridSpecialization>
getGridSpecialization(triton::FuncOp function) {
  Attribute attribute =
      function->getAttr(mlir::triton::cfg::kProgramGridSpecializationAttr);
  if (!attribute) {
    if (ModuleOp module = function->getParentOfType<ModuleOp>())
      attribute =
          module->getAttr(mlir::triton::cfg::kProgramGridSpecializationAttr);
  }
  if (!attribute)
    return std::nullopt;
  FailureOr<ProgramGridSpecialization> parsed =
      parseProgramGridSpecialization(attribute);
  if (failed(parsed))
    return std::nullopt;
  return *parsed;
}

bool isIATRuleEnabled(const ProgramGridSpecialization &specialization) {
  return (specialization.ruleMask &
          getGraphOptimizationRuleMask(
              GraphOptimizationRuleId::IndependentAxisTensorize)) != 0;
}

bool hasConflictingLaunchContract(ModuleOp module) {
  return module->hasAttr(kIndependentAxisTensorizeMarkerAttr) ||
         module->hasAttr(kProgramGridTransformsAttr) ||
         module->hasAttr(kCoalesceFactorAttr) ||
         module->hasAttr(kCoalesceAxisAttr) ||
         module->hasAttr(kCoalesceGridCeilDivAttr);
}

// Every transformed store has its own selected-axis disjointness proof from
// ProgramAxisDependenceAnalysis.  A write/read relation still needs a pointer
// root proof: an in-place or unknown-alias read could observe another lane's
// write even when each store interval is individually disjoint.
bool hasDisjointWriteReadRoots(
    const ProgramAxisDependence &dependence,
    const EntryArgPointerAliasAnalysis &entryPointerAliases) {
  for (const StoreAddressDependence &store : dependence.stores) {
    auto storeOp = dyn_cast_or_null<triton::StoreOp>(store.store);
    if (!storeOp)
      return false;
    for (Operation *operation : dependence.dependenceClosure) {
      auto load = dyn_cast<triton::LoadOp>(operation);
      if (!load)
        continue;
      if (entryPointerAliases.classify(storeOp.getPtr(), load.getPtr()) !=
          EntryArgPointerRelation::DistinctEntryRoots)
        return false;
    }
  }
  return true;
}

CandidateCost buildResourceCandidate(const IATCandidate &candidate,
                                     const ProgramAxisDependence &dependence,
                                     const LiveByteEstimate &liveBytes,
                                     uint64_t tasksBefore,
                                     uint64_t tasksAfter) {
  CandidateCost cost;
  cost.plan.tensorizeFactor = candidate.factor;
  cost.plan.blockT = 1;
  cost.plan.staticAxisFusionFactor = 1;
  cost.plan.stableId =
      (llvm::Twine(candidate.form == TensorizeForm::MergeSplit ? "iat.merge"
                                                               : "iat.norm") +
       ".f" + llvm::Twine(candidate.factor))
          .str();
  cost.logicalTasksBefore = tasksBefore;
  cost.logicalTasksAfter = tasksAfter;
  // IAT is ordinary ceil-div mapping: actual programs equal logical groups.
  // It deliberately does not ask the launcher to cap persistent coverage.
  cost.actualProgramsBefore = tasksBefore;
  cost.actualProgramsAfter = tasksAfter;
  cost.launchesBefore = 1;
  cost.launchesAfter = 1;
  cost.storeCountBefore = dependence.stores.size();
  cost.storeCountAfter = dependence.stores.size();
  cost.addressCalculationsBefore = dependence.dependenceClosure.size();
  cost.addressCalculationsAfter = dependence.dependenceClosure.size();
  cost.workPerProgramBefore = 1;
  cost.workPerProgramAfter = candidate.factor;
  cost.persistent = false;

  if (!liveBytes.known) {
    cost.hasDynamicShape =
        liveBytes.reason == ResourceCostRejectReason::DynamicShape;
    cost.hasUnknownResource = !cost.hasDynamicShape;
    return cost;
  }

  uint64_t estimated = 0;
  if (!multiplyNoOverflow(liveBytes.peakLiveBytes, candidate.factor,
                          estimated)) {
    cost.hasUnknownResource = true;
    return cost;
  }
  cost.hasPeakLiveBytes = true;
  cost.baselinePeakLiveBytes = liveBytes.peakLiveBytes;
  cost.estimatedPeakLiveBytes = estimated;
  return cost;
}

std::optional<IATCandidate> analyzeCandidate(GraphOptimizationContext &context,
                                             bool emitRejectRemark) {
  triton::FuncOp function = context.getFunction();
  ModuleOp module = function->getParentOfType<ModuleOp>();
  if (!module || hasConflictingLaunchContract(module) ||
      !hasSupportedControlFlow(function))
    return std::nullopt;

  std::optional<TensorizeForm> form = getTensorizeForm(function);
  if (!form)
    return std::nullopt;
  std::optional<TensorizeReductionShape> reductionShape =
      getExpectedReductionShape(function, *form);
  if (!reductionShape)
    return std::nullopt;

  std::optional<ProgramGridSpecialization> specialization =
      getGridSpecialization(function);
  if (!specialization || !isIATRuleEnabled(*specialization))
    return std::nullopt;

  constexpr int32_t kTargetAxis = 1;
  // The Ascend Norm+RoPE IAT kernel has a bounded validated logical-launch
  // range.  A transformed grid above this boundary reaches a vector-core
  // runtime failure (the first observed failing shape is 32768 x 16 ->
  // 32768 x 2 = 65536 logical programs), while 16384 x 16 -> 16384 x 2 is
  // valid.  Fail closed until that runtime ABI can represent larger IAT grids.
  constexpr uint64_t kNormRopeMaxTensorizedLaunchPrograms = 32768;
  const int64_t logicalExtent = specialization->grid[kTargetAxis];
  if (logicalExtent < 1)
    return std::nullopt;
  // The norm K form has a single head and must remain a stable no-op. Merge
  // intentionally still permits H=1 so tail-mask tests cover F-1.
  if (*form == TensorizeForm::NormRope && logicalExtent == 1)
    return std::nullopt;

  const ProgramAxisDependence &dependence =
      context.getProgramAxisDependenceAnalysis().get(kTargetAxis);
  if (!dependence.isIndependentAxisTransformCandidate() ||
      !hasDisjointWriteReadRoots(dependence,
                                 context.getEntryArgPointerAliasAnalysis()))
    return std::nullopt;
  std::optional<triton::GetProgramIdOp> targetPid =
      findOnlyProgramId(function, kTargetAxis);
  if (!targetPid)
    return std::nullopt;

  uint64_t tasksBefore = 0;
  if (!getGridTaskProduct(specialization->grid, tasksBefore))
    return std::nullopt;

  const LiveByteEstimate &liveBytes =
      context.getResourceCostAnalysis().getLiveByteEstimate();
  SmallVector<CandidateEvaluation, 4> evaluations;
  for (unsigned factor : kTensorizeFactors) {
    std::array<int64_t, 3> transformedGrid = specialization->grid;
    transformedGrid[kTargetAxis] =
        logicalExtent / factor + (logicalExtent % factor != 0);
    uint64_t tasksAfter = 0;
    if (!getGridTaskProduct(transformedGrid, tasksAfter))
      continue;
    if (*form == TensorizeForm::NormRope &&
        tasksAfter > kNormRopeMaxTensorizedLaunchPrograms)
      continue;

    IATCandidate prototype;
    prototype.function = function;
    prototype.anchor = targetPid->getOperation();
    prototype.form = *form;
    prototype.axis = kTargetAxis;
    prototype.splitExtent = reductionShape->splitExtent;
    prototype.dimExtent = reductionShape->dimExtent;
    prototype.logicalExtent = logicalExtent;
    prototype.factor = factor;
    evaluations.push_back(
        context.getResourceCostAnalysis().evaluate(buildResourceCandidate(
            prototype, dependence, liveBytes, tasksBefore, tasksAfter)));
  }
  if (evaluations.empty())
    return std::nullopt;

  sortCandidateEvaluations(evaluations);
  if (!evaluations.front().accepted) {
    if (emitRejectRemark)
      emitCandidateRemark(targetPid->getOperation(), evaluations.front());
    return std::nullopt;
  }

  IATCandidate candidate;
  candidate.function = function;
  candidate.anchor = targetPid->getOperation();
  candidate.form = *form;
  candidate.axis = kTargetAxis;
  candidate.splitExtent = reductionShape->splitExtent;
  candidate.dimExtent = reductionShape->dimExtent;
  candidate.logicalExtent = logicalExtent;
  candidate.factor =
      static_cast<unsigned>(evaluations.front().candidate.plan.tensorizeFactor);
  candidate.evaluation = std::move(evaluations.front());
  return candidate;
}

bool sameCandidate(const IATCandidate &lhs, const IATCandidate &rhs) {
  return lhs.function == rhs.function && lhs.form == rhs.form &&
         lhs.axis == rhs.axis && lhs.splitExtent == rhs.splitExtent &&
         lhs.dimExtent == rhs.dimExtent &&
         lhs.logicalExtent == rhs.logicalExtent && lhs.factor == rhs.factor;
}

// Keep the lane next to the logical dimension which carries the data, rather
// than applying a fixed insertion point to every value.  In merge, S-shaped
// values carry the split reduction and become [S, F, ...]; D-shaped values
// live after that reduction and become [F, D].  Norm has only the D dimension
// and always puts F first.
int64_t getPreferredLaneAxis(Type originalType, const IATCandidate &candidate) {
  auto tensor = dyn_cast<RankedTensorType>(originalType);
  if (!tensor || candidate.form == TensorizeForm::NormRope)
    return 0;
  const int64_t rank = tensor.getRank();
  if (rank == 0)
    return 0;
  if (rank == 1)
    return tensor.getShape().front() == candidate.splitExtent ? 1 : 0;
  // Scalars broadcast over a leading split dimension retain that dimension on
  // the left of F as well (for example tensor<1xD> -> tensor<1xF xD>).
  if (tensor.getShape().front() == candidate.splitExtent ||
      tensor.getShape().front() == 1)
    return 1;
  return 0;
}

std::optional<RankedTensorType>
getLiftedTensorType(Type originalType, const IATCandidate &candidate,
                    int64_t laneAxis) {
  Type elementType = originalType;
  SmallVector<int64_t> shape;
  if (auto tensor = dyn_cast<RankedTensorType>(originalType)) {
    if (laneAxis < 0 || laneAxis > tensor.getRank())
      return std::nullopt;
    shape.assign(tensor.getShape().begin(), tensor.getShape().end());
    elementType = tensor.getElementType();
  } else if (laneAxis != 0) {
    return std::nullopt;
  }
  shape.insert(shape.begin() + laneAxis, candidate.factor);
  return RankedTensorType::get(shape, elementType);
}

std::optional<RankedTensorType>
getSameShapeTensorType(Type originalType, ArrayRef<int64_t> shape) {
  Type elementType = originalType;
  if (auto tensor = dyn_cast<RankedTensorType>(originalType))
    elementType = tensor.getElementType();
  return RankedTensorType::get(shape, elementType);
}

bool hasSameShape(RankedTensorType lhs, RankedTensorType rhs) {
  return lhs.getRank() == rhs.getRank() &&
         llvm::equal(lhs.getShape(), rhs.getShape());
}

Value makeZero(IRRewriter &rewriter, Location loc, Type type) {
  auto tensor = dyn_cast<RankedTensorType>(type);
  Type elementType = tensor ? tensor.getElementType() : type;
  if (auto integer = dyn_cast<IntegerType>(elementType)) {
    TypedAttr zero = IntegerAttr::get(integer, 0);
    return tensor ? rewriter.create<arith::ConstantOp>(
                        loc, type, DenseElementsAttr::get(tensor, zero))
                  : rewriter.create<arith::ConstantOp>(loc, type, zero);
  }
  if (auto floating = dyn_cast<FloatType>(elementType)) {
    TypedAttr zero = FloatAttr::get(floating, 0.0);
    return tensor ? rewriter.create<arith::ConstantOp>(
                        loc, type, DenseElementsAttr::get(tensor, zero))
                  : rewriter.create<arith::ConstantOp>(loc, type, zero);
  }
  return Value();
}

bool rebuildTensorizedFunction(triton::FuncOp function,
                               const IATCandidate &candidate,
                               IRRewriter &rewriter) {
  if (!hasSupportedControlFlow(function))
    return false;
  std::optional<triton::GetProgramIdOp> targetPid =
      findOnlyProgramId(function, candidate.axis);
  if (!targetPid)
    return false;

  ProgramAxisDependenceAnalysis analysis(function);
  const ProgramAxisDependence &dependence = analysis.get(candidate.axis);
  if (!dependence.isIndependentAxisTransformCandidate())
    return false;
  DenseSet<Operation *> closure;
  for (Operation *operation : dependence.dependenceClosure)
    closure.insert(operation);

  Block &block = function.getBody().front();
  SmallVector<Operation *> originals;
  originals.reserve(block.getOperations().size());
  for (Operation &operation : block)
    originals.push_back(&operation);

  DenseMap<Value, MappedValue> values;
  auto lookup = [&](Value value) -> MappedValue {
    auto it = values.find(value);
    return it == values.end() ? MappedValue{value, false, 0} : it->second;
  };

  auto broadcastTo = [&](Location loc, Value value,
                         RankedTensorType target) -> Value {
    auto source = dyn_cast<RankedTensorType>(value.getType());
    if (!source || source.getElementType() != target.getElementType() ||
        source.getRank() != target.getRank())
      return Value();
    for (auto [sourceDim, targetDim] :
         llvm::zip(source.getShape(), target.getShape()))
      if (sourceDim != targetDim && sourceDim != 1)
        return Value();
    return hasSameShape(source, target)
               ? value
               : rewriter.create<triton::BroadcastOp>(loc, target, value);
  };

  // Embed an already tensorized value in a target shape without moving data:
  // only singleton dimensions are inserted and all non-singleton dimensions
  // must preserve their relative order.  This rejects instead of guessing a
  // transpose when an unfamiliar shape relation reaches the MVP.
  auto alignTensorized = [&](Location loc, MappedValue mapped,
                             RankedTensorType target,
                             int64_t targetLaneAxis) -> Value {
    auto source = dyn_cast<RankedTensorType>(mapped.value.getType());
    if (!source || mapped.laneAxis < 0 || mapped.laneAxis >= source.getRank() ||
        targetLaneAxis < 0 || targetLaneAxis >= target.getRank() ||
        source.getElementType() != target.getElementType())
      return Value();
    const int64_t sourceLaneAxis = mapped.laneAxis;
    const int64_t sourceTail = source.getRank() - sourceLaneAxis - 1;
    const int64_t targetTail = target.getRank() - targetLaneAxis - 1;
    if (sourceLaneAxis > targetLaneAxis || sourceTail > targetTail ||
        source.getShape()[sourceLaneAxis] != candidate.factor ||
        target.getShape()[targetLaneAxis] != candidate.factor)
      return Value();

    SmallVector<int64_t> sourceToTarget(source.getRank(), -1);
    for (int64_t index = 0; index < sourceLaneAxis; ++index)
      sourceToTarget[index] = index;
    sourceToTarget[sourceLaneAxis] = targetLaneAxis;
    for (int64_t index = 0; index < sourceTail; ++index)
      sourceToTarget[sourceLaneAxis + 1 + index] =
          target.getRank() - sourceTail + index;

    SmallVector<bool> targetMapped(target.getRank(), false);
    for (int64_t sourceIndex = 0; sourceIndex < source.getRank();
         ++sourceIndex) {
      const int64_t targetIndex = sourceToTarget[sourceIndex];
      if (targetIndex < 0 || targetIndex >= target.getRank() ||
          targetMapped[targetIndex])
        return Value();
      targetMapped[targetIndex] = true;
      const int64_t sourceDim = source.getShape()[sourceIndex];
      const int64_t targetDim = target.getShape()[targetIndex];
      if (sourceDim != targetDim && sourceDim != 1)
        return Value();
    }

    Value current = mapped.value;
    int64_t currentAxis = 0;
    for (int64_t targetIndex = 0; targetIndex < target.getRank();
         ++targetIndex) {
      if (targetMapped[targetIndex]) {
        ++currentAxis;
        continue;
      }
      current =
          rewriter.create<triton::ExpandDimsOp>(loc, current, currentAxis);
      ++currentAxis;
    }
    return broadcastTo(loc, current, target);
  };

  auto promoteUniform = [&](Location loc, Value value, Type oldType,
                            RankedTensorType target,
                            int64_t targetLaneAxis) -> Value {
    if (targetLaneAxis < 0 || targetLaneAxis >= target.getRank())
      return Value();
    if (auto oldTensor = dyn_cast<RankedTensorType>(oldType)) {
      if (oldTensor.getRank() != target.getRank() - 1 ||
          oldTensor.getElementType() != target.getElementType())
        return Value();
      SmallVector<int64_t> shape(target.getShape().begin(),
                                 target.getShape().end());
      shape.erase(shape.begin() + targetLaneAxis);
      auto unlaned = RankedTensorType::get(shape, target.getElementType());
      Value base = broadcastTo(loc, value, unlaned);
      if (!base)
        return Value();
      Value expanded =
          rewriter.create<triton::ExpandDimsOp>(loc, base, targetLaneAxis);
      return broadcastTo(loc, expanded, target);
    }
    return rewriter.create<triton::SplatOp>(loc, target, value);
  };

  auto liftOperandTo = [&](Value value, RankedTensorType target,
                           int64_t targetLaneAxis) -> Value {
    MappedValue mapped = lookup(value);
    return mapped.tensorized
               ? alignTensorized(value.getLoc(), mapped, target, targetLaneAxis)
               : promoteUniform(value.getLoc(), mapped.value, value.getType(),
                                target, targetLaneAxis);
  };

  auto copyMissingAttrs = [](Operation *from, Operation *to) {
    for (NamedAttribute attribute : from->getAttrs())
      if (!to->hasAttr(attribute.getName()))
        to->setAttr(attribute.getName(), attribute.getValue());
  };

  Value laneMask;
  auto maskForPointerType = [&](Location loc, RankedTensorType pointerType,
                                int64_t laneAxis) -> Value {
    if (!laneMask)
      return Value();
    auto maskType =
        RankedTensorType::get(pointerType.getShape(), rewriter.getI1Type());
    return alignTensorized(loc, MappedValue{laneMask, true, 0}, maskType,
                           laneAxis);
  };

  auto createUnchanged = [&](Operation *operation) -> bool {
    SmallVector<Value> operands;
    operands.reserve(operation->getNumOperands());
    for (Value operand : operation->getOperands()) {
      Value mapped = lookup(operand).value;
      if (!mapped)
        return false;
      operands.push_back(mapped);
    }
    Operation *replacement = rewriter.create(
        operation->getLoc(), operation->getName().getIdentifier(), operands,
        operation->getResultTypes(), operation->getAttrs());
    if (replacement->getNumResults() != operation->getNumResults())
      return false;
    for (auto [oldResult, newResult] :
         llvm::zip(operation->getResults(), replacement->getResults()))
      values[oldResult] = {newResult, false, 0};
    return true;
  };

  auto originalTypesHaveSameShape = [](Type lhs, Type rhs) {
    auto left = dyn_cast<RankedTensorType>(lhs);
    auto right = dyn_cast<RankedTensorType>(rhs);
    if (!left || !right)
      return !left && !right;
    return hasSameShape(left, right);
  };

  auto createElementwise = [&](Operation *operation) -> bool {
    if (operation->getNumResults() == 0)
      return false;
    const bool isReshape = isa<triton::ReshapeOp>(operation);
    if (!operation->hasTrait<OpTrait::Elementwise>() && !isReshape)
      return false;
    if (isReshape &&
        (!originalTypesHaveSameShape(operation->getOperand(0).getType(),
                                     operation->getResult(0).getType())))
      return false;

    int64_t laneAxis =
        getPreferredLaneAxis(operation->getResult(0).getType(), candidate);
    for (Value operand : operation->getOperands()) {
      MappedValue mapped = lookup(operand);
      if (!mapped.tensorized ||
          !originalTypesHaveSameShape(operand.getType(),
                                      operation->getResult(0).getType()))
        continue;
      laneAxis = mapped.laneAxis;
      break;
    }
    std::optional<RankedTensorType> firstResultType = getLiftedTensorType(
        operation->getResult(0).getType(), candidate, laneAxis);
    if (!firstResultType)
      return false;

    SmallVector<Type> resultTypes;
    resultTypes.reserve(operation->getNumResults());
    for (Type resultType : operation->getResultTypes()) {
      std::optional<RankedTensorType> lifted =
          getLiftedTensorType(resultType, candidate, laneAxis);
      if (!lifted || !hasSameShape(*lifted, *firstResultType))
        return false;
      resultTypes.push_back(*lifted);
    }

    SmallVector<Value> operands;
    operands.reserve(operation->getNumOperands());
    for (Value operand : operation->getOperands()) {
      std::optional<RankedTensorType> operandType = getSameShapeTensorType(
          operand.getType(), firstResultType->getShape());
      if (!operandType)
        return false;
      Value lifted = liftOperandTo(operand, *operandType, laneAxis);
      if (!lifted)
        return false;
      operands.push_back(lifted);
    }

    Operation *replacement = rewriter.create(
        operation->getLoc(), operation->getName().getIdentifier(), operands,
        resultTypes, operation->getAttrs());
    if (replacement->getNumResults() != operation->getNumResults())
      return false;
    for (auto [oldResult, newResult] :
         llvm::zip(operation->getResults(), replacement->getResults()))
      values[oldResult] = {newResult, true, laneAxis};
    return true;
  };

  for (Operation *operation : originals) {
    rewriter.setInsertionPoint(operation);

    if (operation == targetPid->getOperation()) {
      Operation *scalarPid = rewriter.clone(*operation);
      if (scalarPid->getNumResults() != 1)
        return false;
      Value scalar = scalarPid->getResult(0);
      auto integer = dyn_cast<IntegerType>(scalar.getType());
      if (!integer || integer.getWidth() != 32)
        return false;
      Value factor = rewriter.create<arith::ConstantIntOp>(
          operation->getLoc(), candidate.factor, integer.getWidth());
      Value base =
          rewriter.create<arith::MulIOp>(operation->getLoc(), scalar, factor);
      auto laneType =
          RankedTensorType::get({candidate.factor}, scalar.getType());
      Value lane = rewriter.create<triton::MakeRangeOp>(
          operation->getLoc(), laneType, 0, candidate.factor);
      Value baseSplat =
          rewriter.create<triton::SplatOp>(operation->getLoc(), laneType, base);
      Value logicalIds =
          rewriter.create<arith::AddIOp>(operation->getLoc(), baseSplat, lane);
      Value extent = rewriter.create<arith::ConstantIntOp>(
          operation->getLoc(), candidate.logicalExtent, integer.getWidth());
      Value extentSplat = rewriter.create<triton::SplatOp>(operation->getLoc(),
                                                           laneType, extent);
      laneMask = rewriter.create<arith::CmpIOp>(operation->getLoc(),
                                                arith::CmpIPredicate::slt,
                                                logicalIds, extentSplat);
      values[operation->getResult(0)] = {logicalIds, true, 0};
      continue;
    }

    bool tensorized = closure.contains(operation);
    for (Value operand : operation->getOperands())
      tensorized |= lookup(operand).tensorized;

    if (auto splat = dyn_cast<triton::SplatOp>(operation)) {
      if (!tensorized) {
        if (!createUnchanged(operation))
          return false;
        continue;
      }
      const int64_t laneAxis = getPreferredLaneAxis(splat.getType(), candidate);
      std::optional<RankedTensorType> resultType =
          getLiftedTensorType(splat.getType(), candidate, laneAxis);
      if (!resultType)
        return false;
      Value result = liftOperandTo(splat.getSrc(), *resultType, laneAxis);
      if (!result)
        return false;
      values[splat.getResult()] = {result, true, laneAxis};
      continue;
    }

    if (auto expand = dyn_cast<triton::ExpandDimsOp>(operation)) {
      if (!tensorized) {
        if (!createUnchanged(operation))
          return false;
        continue;
      }
      MappedValue sourceMapped = lookup(expand.getSrc());
      const int64_t oldLaneAxis =
          sourceMapped.tensorized
              ? sourceMapped.laneAxis
              : getPreferredLaneAxis(expand.getSrc().getType(), candidate);
      std::optional<RankedTensorType> sourceType = getLiftedTensorType(
          expand.getSrc().getType(), candidate, oldLaneAxis);
      if (!sourceType)
        return false;
      Value source = liftOperandTo(expand.getSrc(), *sourceType, oldLaneAxis);
      if (!source)
        return false;
      const int64_t newAxis =
          expand.getAxis() + (expand.getAxis() >= oldLaneAxis ? 1 : 0);
      const int64_t resultLaneAxis =
          oldLaneAxis + (expand.getAxis() < oldLaneAxis ? 1 : 0);
      Value result = rewriter.create<triton::ExpandDimsOp>(operation->getLoc(),
                                                           source, newAxis);
      std::optional<RankedTensorType> expected =
          getLiftedTensorType(expand.getType(), candidate, resultLaneAxis);
      if (!expected || result.getType() != *expected)
        return false;
      values[expand.getResult()] = {result, true, resultLaneAxis};
      continue;
    }

    if (auto broadcast = dyn_cast<triton::BroadcastOp>(operation)) {
      if (!tensorized) {
        if (!createUnchanged(operation))
          return false;
        continue;
      }
      MappedValue sourceMapped = lookup(broadcast.getSrc());
      const int64_t laneAxis =
          sourceMapped.tensorized
              ? sourceMapped.laneAxis
              : getPreferredLaneAxis(broadcast.getSrc().getType(), candidate);
      std::optional<RankedTensorType> resultType =
          getLiftedTensorType(broadcast.getType(), candidate, laneAxis);
      if (!resultType)
        return false;
      Value result = liftOperandTo(broadcast.getSrc(), *resultType, laneAxis);
      if (!result)
        return false;
      values[broadcast.getResult()] = {result, true, laneAxis};
      continue;
    }

    if (auto reduce = dyn_cast<triton::ReduceOp>(operation)) {
      if (!tensorized) {
        if (!createUnchanged(operation))
          return false;
        continue;
      }
      if (reduce.getSrcs().empty())
        return false;
      auto oldInputType =
          dyn_cast<RankedTensorType>(reduce.getSrcs().front().getType());
      if (!oldInputType)
        return false;
      MappedValue firstSource = lookup(reduce.getSrcs().front());
      const int64_t laneAxis =
          firstSource.tensorized
              ? firstSource.laneAxis
              : getPreferredLaneAxis(oldInputType, candidate);
      std::optional<RankedTensorType> firstSourceType =
          getLiftedTensorType(oldInputType, candidate, laneAxis);
      if (!firstSourceType)
        return false;
      SmallVector<Value> sources;
      for (Value source : reduce.getSrcs()) {
        std::optional<RankedTensorType> sourceType =
            getLiftedTensorType(source.getType(), candidate, laneAxis);
        if (!sourceType || !hasSameShape(*sourceType, *firstSourceType))
          return false;
        Value lifted = liftOperandTo(source, *sourceType, laneAxis);
        if (!lifted)
          return false;
        sources.push_back(lifted);
      }
      const int64_t axis =
          reduce.getAxis() + (reduce.getAxis() >= laneAxis ? 1 : 0);
      const int64_t resultLaneAxis = laneAxis - (axis < laneAxis ? 1 : 0);
      auto replacement =
          rewriter.create<triton::ReduceOp>(operation->getLoc(), sources, axis);
      rewriter.cloneRegionBefore(reduce.getCombineOp(),
                                 replacement.getCombineOp(),
                                 replacement.getCombineOp().end());
      copyMissingAttrs(operation, replacement.getOperation());
      for (auto [oldResult, newResult] :
           llvm::zip(reduce.getResults(), replacement.getResults())) {
        std::optional<RankedTensorType> expected =
            getLiftedTensorType(oldResult.getType(), candidate, resultLaneAxis);
        if (!expected || newResult.getType() != *expected)
          return false;
        values[oldResult] = {newResult, true, resultLaneAxis};
      }
      continue;
    }

    if (auto scan = dyn_cast<triton::ScanOp>(operation)) {
      if (!tensorized) {
        if (!createUnchanged(operation))
          return false;
        continue;
      }
      if (scan.getSrcs().empty())
        return false;
      auto oldInputType =
          dyn_cast<RankedTensorType>(scan.getSrcs().front().getType());
      if (!oldInputType)
        return false;
      MappedValue firstSource = lookup(scan.getSrcs().front());
      const int64_t laneAxis =
          firstSource.tensorized
              ? firstSource.laneAxis
              : getPreferredLaneAxis(oldInputType, candidate);
      std::optional<RankedTensorType> firstSourceType =
          getLiftedTensorType(oldInputType, candidate, laneAxis);
      if (!firstSourceType)
        return false;
      SmallVector<Value> sources;
      for (Value source : scan.getSrcs()) {
        std::optional<RankedTensorType> sourceType =
            getLiftedTensorType(source.getType(), candidate, laneAxis);
        if (!sourceType || !hasSameShape(*sourceType, *firstSourceType))
          return false;
        Value lifted = liftOperandTo(source, *sourceType, laneAxis);
        if (!lifted)
          return false;
        sources.push_back(lifted);
      }
      const int64_t axis =
          scan.getAxis() + (scan.getAxis() >= laneAxis ? 1 : 0);
      auto replacement = rewriter.create<triton::ScanOp>(
          operation->getLoc(), sources, static_cast<uint32_t>(axis),
          scan.getReverse());
      rewriter.cloneRegionBefore(scan.getCombineOp(),
                                 replacement.getCombineOp(),
                                 replacement.getCombineOp().end());
      copyMissingAttrs(operation, replacement.getOperation());
      for (auto [oldResult, newResult] :
           llvm::zip(scan.getResults(), replacement.getResults())) {
        std::optional<RankedTensorType> expected =
            getLiftedTensorType(oldResult.getType(), candidate, laneAxis);
        if (!expected || newResult.getType() != *expected)
          return false;
        values[oldResult] = {newResult, true, laneAxis};
      }
      continue;
    }

    if (auto load = dyn_cast<triton::LoadOp>(operation)) {
      if (!tensorized) {
        if (!createUnchanged(operation))
          return false;
        continue;
      }
      MappedValue pointerMapped = lookup(load.getPtr());
      const int64_t laneAxis =
          pointerMapped.tensorized
              ? pointerMapped.laneAxis
              : getPreferredLaneAxis(load.getPtr().getType(), candidate);
      std::optional<RankedTensorType> pointerType =
          getLiftedTensorType(load.getPtr().getType(), candidate, laneAxis);
      if (!pointerType)
        return false;
      Value pointer = liftOperandTo(load.getPtr(), *pointerType, laneAxis);
      std::optional<RankedTensorType> maskType =
          getSameShapeTensorType(rewriter.getI1Type(), pointerType->getShape());
      Value mask = load.getMask()
                       ? liftOperandTo(load.getMask(), *maskType, laneAxis)
                       : Value();
      Value laneMaskForLoad =
          maskForPointerType(operation->getLoc(), *pointerType, laneAxis);
      if (laneMaskForLoad)
        mask = mask ? rewriter.create<arith::AndIOp>(operation->getLoc(), mask,
                                                     laneMaskForLoad)
                    : laneMaskForLoad;
      std::optional<RankedTensorType> otherType = getSameShapeTensorType(
          load.getResult().getType(), pointerType->getShape());
      Value other = load.getOther()
                        ? liftOperandTo(load.getOther(), *otherType, laneAxis)
                        : makeZero(rewriter, operation->getLoc(), *otherType);
      if (!pointer || !maskType || !otherType || !other)
        return false;
      auto replacement = rewriter.create<triton::LoadOp>(
          operation->getLoc(), pointer, mask, other, load.getBoundaryCheck(),
          load.getPadding(), load.getCache(), load.getEvict(),
          load.getIsVolatile());
      copyMissingAttrs(operation, replacement.getOperation());
      if (replacement.getResult().getType() != *otherType)
        return false;
      values[load.getResult()] = {replacement.getResult(), true, laneAxis};
      continue;
    }

    if (auto store = dyn_cast<triton::StoreOp>(operation)) {
      if (!tensorized) {
        if (!createUnchanged(operation))
          return false;
        continue;
      }
      MappedValue pointerMapped = lookup(store.getPtr());
      const int64_t laneAxis =
          pointerMapped.tensorized
              ? pointerMapped.laneAxis
              : getPreferredLaneAxis(store.getPtr().getType(), candidate);
      std::optional<RankedTensorType> pointerType =
          getLiftedTensorType(store.getPtr().getType(), candidate, laneAxis);
      if (!pointerType)
        return false;
      Value pointer = liftOperandTo(store.getPtr(), *pointerType, laneAxis);
      std::optional<RankedTensorType> valueType = getSameShapeTensorType(
          store.getValue().getType(), pointerType->getShape());
      if (!valueType)
        return false;
      Value value = liftOperandTo(store.getValue(), *valueType, laneAxis);
      auto maskType =
          RankedTensorType::get(pointerType->getShape(), rewriter.getI1Type());
      Value mask = store.getMask()
                       ? liftOperandTo(store.getMask(), maskType, laneAxis)
                       : Value();
      Value laneMaskForStore =
          maskForPointerType(operation->getLoc(), *pointerType, laneAxis);
      if (laneMaskForStore)
        mask = mask ? rewriter.create<arith::AndIOp>(operation->getLoc(), mask,
                                                     laneMaskForStore)
                    : laneMaskForStore;
      if (!pointer || !value || !laneMaskForStore)
        return false;
      auto replacement = rewriter.create<triton::StoreOp>(
          operation->getLoc(), pointer, value, mask, store.getBoundaryCheck(),
          store.getCache(), store.getEvict());
      copyMissingAttrs(operation, replacement.getOperation());
      continue;
    }

    if (isa<triton::ReturnOp>(operation)) {
      // The target kernels have no return values. Returning a tensorized value
      // would require changing the callable ABI, so reject it.
      for (Value operand : operation->getOperands())
        if (lookup(operand).tensorized)
          return false;
      if (!createUnchanged(operation))
        return false;
      continue;
    }

    if (!tensorized) {
      if (!createUnchanged(operation))
        return false;
      continue;
    }
    if (!createElementwise(operation))
      return false;
  }

  if (!laneMask)
    return false;
  for (Operation *operation : llvm::reverse(originals)) {
    operation->dropAllUses();
    rewriter.eraseOp(operation);
  }
  return true;
}

class IndependentAxisTensorizePlan final : public RewritePlan {
public:
  IndependentAxisTensorizePlan(IATCandidate candidate, unsigned epoch)
      : candidate(std::move(candidate)), epoch(epoch) {}

  GraphOptimizationRuleId getRuleId() const override {
    return GraphOptimizationRuleId::IndependentAxisTensorize;
  }
  unsigned getBenefit() const override {
    return candidate.evaluation.benefitScore > 0
               ? static_cast<unsigned>(candidate.evaluation.benefitScore)
               : 1;
  }
  Operation *getAnchor() const override { return candidate.anchor; }
  unsigned getCreationEpoch() const override { return epoch; }

  LogicalResult revalidate(GraphOptimizationContext &context) const override {
    if (context.getFunction() != candidate.function)
      return failure();
    std::optional<IATCandidate> current = analyzeCandidate(context, false);
    return current && sameCandidate(candidate, *current) ? success()
                                                         : failure();
  }

  LogicalResult apply(IRRewriter &rewriter) override {
    (void)rewriter;
    ModuleOp module = candidate.function->getParentOfType<ModuleOp>();
    if (!module || hasConflictingLaunchContract(module))
      return failure();

    // Materialize in a detached module. All late failures (unsupported tensor
    // relation, verifier failure, or contract serialization failure) therefore
    // leave the source function and its metadata untouched.
    ModuleOp sandbox = ModuleOp::create(candidate.function.getLoc());
    sandbox.getBody()->push_back(candidate.function->clone());
    auto clonedFunction = dyn_cast<triton::FuncOp>(&sandbox.getBody()->front());
    if (!clonedFunction)
      return failure();

    IRRewriter sandboxRewriter(sandbox.getContext());
    if (!rebuildTensorizedFunction(clonedFunction, candidate, sandboxRewriter))
      return failure();

    ProgramGridTransformContract contract;
    contract.transforms.push_back(ProgramGridTransform{
        0, candidate.axis, candidate.factor, candidate.logicalExtent,
        /*persistentCoverage=*/false,
        /*gridStrideAbiVerified=*/false});
    if (failed(setProgramGridTransformContract(sandbox, contract)))
      return failure();
    sandbox->setAttr(kIndependentAxisTensorizeMarkerAttr,
                     UnitAttr::get(sandbox.getContext()));
    if (failed(mlir::verify(sandbox.getOperation())))
      return failure();

    // Both operations below are non-failing after sandbox validation: the
    // function body transfer has no allocation path and the generic attribute
    // was created in the same MLIRContext.
    candidate.function->getRegion(0).takeBody(clonedFunction->getRegion(0));
    module->setAttr(kProgramGridTransformsAttr,
                    sandbox->getAttr(kProgramGridTransformsAttr));
    module->setAttr(kIndependentAxisTensorizeMarkerAttr,
                    UnitAttr::get(module.getContext()));
    return success();
  }

private:
  IATCandidate candidate;
  unsigned epoch;
};

class IndependentAxisTensorizeRule final : public GraphOptimizationRule {
public:
  explicit IndependentAxisTensorizeRule(bool enabledForCompileMode)
      : enabledForCompileMode(enabledForCompileMode) {}

  GraphOptimizationRuleId getId() const override {
    return GraphOptimizationRuleId::IndependentAxisTensorize;
  }

  AnalysisRequirement getAnalysisRequirements() const override {
    return AnalysisRequirement::EntryArgPointerAlias |
           AnalysisRequirement::ProgramAxisDependence |
           AnalysisRequirement::ResourceCost;
  }

  LogicalResult findCandidates(
      GraphOptimizationContext &context,
      SmallVectorImpl<std::unique_ptr<RewritePlan>> &plans) override {
    if (!enabledForCompileMode)
      return success();
    std::optional<IATCandidate> candidate = analyzeCandidate(context, true);
    if (!candidate)
      return success();

    emitCandidateRemark(candidate->anchor, candidate->evaluation);
    LLVM_DEBUG(llvm::dbgs()
               << "[" DEBUG_TYPE "] matched graph optimization rule "
               << static_cast<unsigned>(getId()) << " ("
               << getGraphOptimizationRuleName(getId()) << ") in @"
               << candidate->function.getName() << ": axis=" << candidate->axis
               << " factor=" << candidate->factor
               << " logical_extent=" << candidate->logicalExtent << "\n");
    plans.push_back(std::make_unique<IndependentAxisTensorizePlan>(
        std::move(*candidate), context.getEpoch()));
    return success();
  }

private:
  bool enabledForCompileMode;
};

} // namespace

std::unique_ptr<GraphOptimizationRule> cfg::createIndependentAxisTensorizeRule(
    const IndependentAxisTensorizeRuleOptions &options) {
  return std::make_unique<IndependentAxisTensorizeRule>(
      options.enabledForCompileMode);
}
