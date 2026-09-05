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
  int64_t laneInsertAxis = 0;
  int64_t logicalExtent = 0;
  unsigned factor = 1;
  CandidateEvaluation evaluation;
};

struct MappedValue {
  Value value;
  bool tensorized = false;
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

bool hasExpectedReductionShape(triton::FuncOp function, TensorizeForm form) {
  bool matched = false;
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
      matched = source.getRank() == 2 && rankedResult &&
                rankedResult.getRank() == 1 && rankedResult.hasStaticShape();
      return;
    }
    matched = source.getRank() == 1 && !isa<RankedTensorType>(result);
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
  if (!form || !hasExpectedReductionShape(function, *form))
    return std::nullopt;

  std::optional<ProgramGridSpecialization> specialization =
      getGridSpecialization(function);
  if (!specialization || !isIATRuleEnabled(*specialization))
    return std::nullopt;

  constexpr int32_t kTargetAxis = 1;
  const int64_t logicalExtent = specialization->grid[kTargetAxis];
  if (logicalExtent < 1)
    return std::nullopt;
  // The norm K form has a single head and must remain a stable no-op. Merge
  // intentionally still permits H=1 so tail-mask tests cover F-1.
  if (*form == TensorizeForm::NormRope && logicalExtent == 1)
    return std::nullopt;

  const ProgramAxisDependence &dependence =
      context.getProgramAxisDependenceAnalysis().get(kTargetAxis);
  if (!dependence.isIndependentAxisTransformCandidate())
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

    IATCandidate prototype;
    prototype.function = function;
    prototype.anchor = targetPid->getOperation();
    prototype.form = *form;
    prototype.axis = kTargetAxis;
    prototype.laneInsertAxis = *form == TensorizeForm::MergeSplit ? 1 : 0;
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
  candidate.laneInsertAxis = *form == TensorizeForm::MergeSplit ? 1 : 0;
  candidate.logicalExtent = logicalExtent;
  candidate.factor =
      static_cast<unsigned>(evaluations.front().candidate.plan.tensorizeFactor);
  candidate.evaluation = std::move(evaluations.front());
  return candidate;
}

bool sameCandidate(const IATCandidate &lhs, const IATCandidate &rhs) {
  return lhs.function == rhs.function && lhs.form == rhs.form &&
         lhs.axis == rhs.axis && lhs.laneInsertAxis == rhs.laneInsertAxis &&
         lhs.logicalExtent == rhs.logicalExtent && lhs.factor == rhs.factor;
}

Type getLiftedType(Type type, const IATCandidate &candidate) {
  if (auto tensor = dyn_cast<RankedTensorType>(type)) {
    SmallVector<int64_t> shape(tensor.getShape().begin(),
                               tensor.getShape().end());
    const int64_t laneAxis =
        std::min<int64_t>(candidate.laneInsertAxis, shape.size());
    shape.insert(shape.begin() + laneAxis, candidate.factor);
    return RankedTensorType::get(shape, tensor.getElementType());
  }
  return RankedTensorType::get({candidate.factor}, type);
}

int64_t getLaneAxisForType(Type originalType, const IATCandidate &candidate) {
  if (auto tensor = dyn_cast<RankedTensorType>(originalType))
    return std::min<int64_t>(candidate.laneInsertAxis, tensor.getRank());
  return 0;
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
    return it == values.end() ? MappedValue{value, false} : it->second;
  };

  auto promoteUniform = [&](Value value, Type oldType) -> Value {
    Type liftedType = getLiftedType(oldType, candidate);
    auto liftedTensor = dyn_cast<RankedTensorType>(liftedType);
    if (!liftedTensor)
      return Value();
    if (isa<RankedTensorType>(oldType)) {
      const int64_t laneAxis = getLaneAxisForType(oldType, candidate);
      Value expanded = rewriter.create<triton::ExpandDimsOp>(value.getLoc(),
                                                             value, laneAxis);
      return rewriter.create<triton::BroadcastOp>(value.getLoc(), liftedTensor,
                                                  expanded);
    }
    return rewriter.create<triton::SplatOp>(value.getLoc(), liftedTensor,
                                            value);
  };

  auto liftOperand = [&](Value value) -> Value {
    MappedValue mapped = lookup(value);
    return mapped.tensorized ? mapped.value
                             : promoteUniform(mapped.value, value.getType());
  };

  auto copyMissingAttrs = [](Operation *from, Operation *to) {
    for (NamedAttribute attribute : from->getAttrs())
      if (!to->hasAttr(attribute.getName()))
        to->setAttr(attribute.getName(), attribute.getValue());
  };

  Value laneMask;
  auto maskForPointerType = [&](Location loc, Type pointerType) -> Value {
    auto pointerTensor = dyn_cast<RankedTensorType>(pointerType);
    if (!pointerTensor || !laneMask)
      return Value();
    const int64_t rank = pointerTensor.getRank();
    if (rank < 1)
      return Value();
    const int64_t laneAxis =
        std::min<int64_t>(candidate.laneInsertAxis, rank - 1);
    Value current = laneMask;
    for (int64_t index = 0; index < laneAxis; ++index)
      current = rewriter.create<triton::ExpandDimsOp>(loc, current, 0);
    while (cast<RankedTensorType>(current.getType()).getRank() < rank) {
      current = rewriter.create<triton::ExpandDimsOp>(
          loc, current, cast<RankedTensorType>(current.getType()).getRank());
    }
    auto maskType =
        RankedTensorType::get(pointerTensor.getShape(), rewriter.getI1Type());
    return rewriter.create<triton::BroadcastOp>(loc, maskType, current);
  };

  auto createGeneric = [&](Operation *operation, bool tensorized) -> bool {
    SmallVector<Value> operands;
    operands.reserve(operation->getNumOperands());
    for (Value operand : operation->getOperands()) {
      Value mapped = tensorized ? liftOperand(operand) : lookup(operand).value;
      if (!mapped)
        return false;
      operands.push_back(mapped);
    }
    SmallVector<Type> resultTypes;
    resultTypes.reserve(operation->getNumResults());
    for (Type type : operation->getResultTypes())
      resultTypes.push_back(tensorized ? getLiftedType(type, candidate) : type);
    Operation *replacement = rewriter.create(
        operation->getLoc(), operation->getName().getIdentifier(), operands,
        resultTypes, operation->getAttrs());
    if (replacement->getNumResults() != operation->getNumResults())
      return false;
    for (auto [oldResult, newResult] :
         llvm::zip(operation->getResults(), replacement->getResults()))
      values[oldResult] = {newResult, tensorized};
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
      values[operation->getResult(0)] = {logicalIds, true};
      continue;
    }

    bool tensorized = closure.contains(operation);
    for (Value operand : operation->getOperands())
      tensorized |= lookup(operand).tensorized;

    if (auto splat = dyn_cast<triton::SplatOp>(operation)) {
      if (!tensorized) {
        if (!createGeneric(operation, false))
          return false;
        continue;
      }
      Value source = liftOperand(splat.getSrc());
      auto sourceType = dyn_cast<RankedTensorType>(source.getType());
      auto resultType =
          dyn_cast<RankedTensorType>(getLiftedType(splat.getType(), candidate));
      if (!sourceType || !resultType || sourceType.getRank() != 1 ||
          sourceType.getShape().front() != candidate.factor)
        return false;
      Value current = source;
      const int64_t laneAxis =
          std::min<int64_t>(candidate.laneInsertAxis, resultType.getRank() - 1);
      for (int64_t index = 0; index < laneAxis; ++index)
        current = rewriter.create<triton::ExpandDimsOp>(operation->getLoc(),
                                                        current, 0);
      while (cast<RankedTensorType>(current.getType()).getRank() <
             resultType.getRank()) {
        current = rewriter.create<triton::ExpandDimsOp>(
            operation->getLoc(), current,
            cast<RankedTensorType>(current.getType()).getRank());
      }
      values[splat.getResult()] = {
          rewriter.create<triton::BroadcastOp>(operation->getLoc(), resultType,
                                               current),
          true};
      continue;
    }

    if (auto expand = dyn_cast<triton::ExpandDimsOp>(operation)) {
      if (!tensorized) {
        if (!createGeneric(operation, false))
          return false;
        continue;
      }
      Value source = liftOperand(expand.getSrc());
      const int64_t oldLaneAxis =
          getLaneAxisForType(expand.getSrc().getType(), candidate);
      const int64_t newAxis =
          expand.getAxis() + (expand.getAxis() >= oldLaneAxis ? 1 : 0);
      Value result = rewriter.create<triton::ExpandDimsOp>(operation->getLoc(),
                                                           source, newAxis);
      if (result.getType() != getLiftedType(expand.getType(), candidate))
        return false;
      values[expand.getResult()] = {result, true};
      continue;
    }

    if (auto broadcast = dyn_cast<triton::BroadcastOp>(operation)) {
      if (!tensorized) {
        if (!createGeneric(operation, false))
          return false;
        continue;
      }
      Value source = liftOperand(broadcast.getSrc());
      auto resultType = dyn_cast<RankedTensorType>(
          getLiftedType(broadcast.getType(), candidate));
      if (!resultType)
        return false;
      values[broadcast.getResult()] = {
          rewriter.create<triton::BroadcastOp>(operation->getLoc(), resultType,
                                               source),
          true};
      continue;
    }

    if (auto reduce = dyn_cast<triton::ReduceOp>(operation)) {
      if (!tensorized) {
        if (!createGeneric(operation, false))
          return false;
        continue;
      }
      if (reduce.getSrcs().empty())
        return false;
      auto oldInputType =
          dyn_cast<RankedTensorType>(reduce.getSrcs().front().getType());
      if (!oldInputType)
        return false;
      SmallVector<Value> sources;
      for (Value source : reduce.getSrcs()) {
        Value lifted = liftOperand(source);
        if (!lifted)
          return false;
        sources.push_back(lifted);
      }
      const int64_t oldLaneAxis = getLaneAxisForType(oldInputType, candidate);
      const int64_t axis =
          reduce.getAxis() + (reduce.getAxis() >= oldLaneAxis ? 1 : 0);
      auto replacement =
          rewriter.create<triton::ReduceOp>(operation->getLoc(), sources, axis);
      rewriter.cloneRegionBefore(reduce.getCombineOp(),
                                 replacement.getCombineOp(),
                                 replacement.getCombineOp().end());
      copyMissingAttrs(operation, replacement.getOperation());
      for (auto [oldResult, newResult] :
           llvm::zip(reduce.getResults(), replacement.getResults())) {
        if (newResult.getType() !=
            getLiftedType(oldResult.getType(), candidate))
          return false;
        values[oldResult] = {newResult, true};
      }
      continue;
    }

    if (auto scan = dyn_cast<triton::ScanOp>(operation)) {
      if (!tensorized) {
        if (!createGeneric(operation, false))
          return false;
        continue;
      }
      if (scan.getSrcs().empty())
        return false;
      auto oldInputType =
          dyn_cast<RankedTensorType>(scan.getSrcs().front().getType());
      if (!oldInputType)
        return false;
      SmallVector<Value> sources;
      for (Value source : scan.getSrcs()) {
        Value lifted = liftOperand(source);
        if (!lifted)
          return false;
        sources.push_back(lifted);
      }
      const int64_t oldLaneAxis = getLaneAxisForType(oldInputType, candidate);
      const int64_t axis =
          scan.getAxis() + (scan.getAxis() >= oldLaneAxis ? 1 : 0);
      auto replacement = rewriter.create<triton::ScanOp>(
          operation->getLoc(), sources, static_cast<uint32_t>(axis),
          scan.getReverse());
      rewriter.cloneRegionBefore(scan.getCombineOp(),
                                 replacement.getCombineOp(),
                                 replacement.getCombineOp().end());
      copyMissingAttrs(operation, replacement.getOperation());
      for (auto [oldResult, newResult] :
           llvm::zip(scan.getResults(), replacement.getResults())) {
        if (newResult.getType() !=
            getLiftedType(oldResult.getType(), candidate))
          return false;
        values[oldResult] = {newResult, true};
      }
      continue;
    }

    if (auto load = dyn_cast<triton::LoadOp>(operation)) {
      if (!tensorized) {
        if (!createGeneric(operation, false))
          return false;
        continue;
      }
      Value pointer = liftOperand(load.getPtr());
      Value mask = load.getMask() ? liftOperand(load.getMask()) : Value();
      Value laneMaskForLoad =
          maskForPointerType(operation->getLoc(), pointer.getType());
      if (laneMaskForLoad)
        mask = mask ? rewriter.create<arith::AndIOp>(operation->getLoc(), mask,
                                                     laneMaskForLoad)
                    : laneMaskForLoad;
      Value other =
          load.getOther()
              ? liftOperand(load.getOther())
              : makeZero(rewriter, operation->getLoc(),
                         getLiftedType(load.getResult().getType(), candidate));
      if (!pointer || !other)
        return false;
      auto replacement = rewriter.create<triton::LoadOp>(
          operation->getLoc(), pointer, mask, other, load.getBoundaryCheck(),
          load.getPadding(), load.getCache(), load.getEvict(),
          load.getIsVolatile());
      copyMissingAttrs(operation, replacement.getOperation());
      if (replacement.getResult().getType() !=
          getLiftedType(load.getResult().getType(), candidate))
        return false;
      values[load.getResult()] = {replacement.getResult(), true};
      continue;
    }

    if (auto store = dyn_cast<triton::StoreOp>(operation)) {
      if (!tensorized) {
        if (!createGeneric(operation, false))
          return false;
        continue;
      }
      Value pointer = liftOperand(store.getPtr());
      Value value = liftOperand(store.getValue());
      Value mask = store.getMask() ? liftOperand(store.getMask()) : Value();
      Value laneMaskForStore =
          maskForPointerType(operation->getLoc(), pointer.getType());
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
      if (!createGeneric(operation, false))
        return false;
      continue;
    }

    if (!createGeneric(operation, tensorized))
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
  GraphOptimizationRuleId getId() const override {
    return GraphOptimizationRuleId::IndependentAxisTensorize;
  }

  AnalysisRequirement getAnalysisRequirements() const override {
    return AnalysisRequirement::ProgramAxisDependence |
           AnalysisRequirement::ResourceCost;
  }

  LogicalResult findCandidates(
      GraphOptimizationContext &context,
      SmallVectorImpl<std::unique_ptr<RewritePlan>> &plans) override {
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
};

} // namespace

std::unique_ptr<GraphOptimizationRule> cfg::createIndependentAxisTensorizeRule(
    const IndependentAxisTensorizeRuleOptions &options) {
  static_cast<void>(options);
  return std::make_unique<IndependentAxisTensorizeRule>();
}
