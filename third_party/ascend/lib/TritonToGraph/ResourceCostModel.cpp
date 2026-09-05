/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
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

#include "TritonToGraph/ResourceCostModel.h"

#include "mlir/IR/BuiltinTypes.h"
#include "triton/Dialect/Triton/IR/Types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/raw_ostream.h"

#if TRITON_ASCEND_HAS_INPROC_COSTMODEL
#include "AscendModel/HardwareConfig.h"
#endif

#include <algorithm>
#include <limits>
#include <tuple>
#include <utility>

using namespace mlir;
using namespace triton;
using namespace cfg;

namespace {

bool checkedAdd(uint64_t lhs, uint64_t rhs, uint64_t &result) {
  if (lhs > std::numeric_limits<uint64_t>::max() - rhs)
    return false;
  result = lhs + rhs;
  return true;
}

bool checkedMul(uint64_t lhs, uint64_t rhs, uint64_t &result) {
  if (lhs != 0 && rhs > std::numeric_limits<uint64_t>::max() / lhs)
    return false;
  result = lhs * rhs;
  return true;
}

bool checkedScale(uint64_t value, uint64_t weight, uint64_t &result) {
  return checkedMul(value, weight, result);
}

bool checkedAccumulate(uint64_t value, uint64_t &total) {
  uint64_t next = 0;
  if (!checkedAdd(total, value, next))
    return false;
  total = next;
  return true;
}

bool isTensorValue(Value value) {
  return value && isa<RankedTensorType, UnrankedTensorType>(value.getType());
}

std::optional<unsigned> getElementBitWidth(Type elementType) {
  if (auto integer = dyn_cast<IntegerType>(elementType))
    return integer.getWidth();
  if (auto floating = dyn_cast<FloatType>(elementType))
    return floating.getWidth();
  if (isa<IndexType>(elementType))
    return 64;
  if (isa<triton::PointerType>(elementType))
    return 64;
  return std::nullopt;
}

Value findCanonicalValue(Value value, const llvm::DenseMap<Value, Value> &map) {
  Value current = value;
  while (true) {
    auto it = map.find(current);
    if (it == map.end() || it->second == current)
      return current;
    current = it->second;
  }
}

uint64_t
getDefinitionOrder(Value value,
                   const llvm::DenseMap<Operation *, uint64_t> &order) {
  if (auto result = dyn_cast<OpResult>(value)) {
    auto it = order.find(result.getOwner());
    return it == order.end() ? 0 : it->second;
  }
  if (auto argument = dyn_cast<BlockArgument>(value)) {
    Operation *parent = argument.getOwner()->getParentOp();
    auto it = order.find(parent);
    return it == order.end() ? 0 : it->second;
  }
  return 0;
}

bool isDefinedInside(Operation *valueOwner, Operation *containingOp) {
  return valueOwner && containingOp &&
         (valueOwner == containingOp || containingOp->isAncestor(valueOwner));
}

bool isDefinedInside(Value value, Operation *containingOp) {
  if (auto result = dyn_cast<OpResult>(value))
    return isDefinedInside(result.getOwner(), containingOp);
  if (auto argument = dyn_cast<BlockArgument>(value))
    return isDefinedInside(argument.getOwner()->getParentOp(), containingOp);
  return false;
}

bool isForLikeLoop(Operation *operation) {
  return operation && operation->getName().getStringRef() == "scf.for" &&
         operation->getNumRegions() == 1 && !operation->getRegion(0).empty();
}

void addCanonicalFactors(llvm::ArrayRef<unsigned> input,
                         llvm::SmallVectorImpl<unsigned> &output) {
  for (unsigned value : input) {
    if (value != 0)
      output.push_back(value);
  }
  if (output.empty())
    output.push_back(1);
  llvm::sort(output);
  output.erase(std::unique(output.begin(), output.end()), output.end());
}

bool addWeightedChange(uint64_t before, uint64_t after, uint64_t weight,
                       uint64_t &gain, uint64_t &penalty) {
  const uint64_t difference = before >= after ? before - after : after - before;
  uint64_t weighted = 0;
  if (!checkedScale(difference, weight, weighted))
    return false;
  return before >= after ? checkedAccumulate(weighted, gain)
                         : checkedAccumulate(weighted, penalty);
}

CandidateEvaluation reject(const CandidateCost &candidate,
                           ResourceCostRejectReason reason,
                           uint64_t safeUBBudget = 0,
                           uint64_t requiredParallelPrograms = 0) {
  CandidateEvaluation evaluation;
  evaluation.candidate = candidate;
  evaluation.reason = reason;
  evaluation.safeUBBudgetBytes = safeUBBudget;
  evaluation.requiredParallelPrograms = requiredParallelPrograms;
  evaluation.remark = formatCandidateRemark(evaluation);
  return evaluation;
}

} // namespace

const char *
cfg::getResourceCostRejectReasonName(ResourceCostRejectReason reason) {
  switch (reason) {
  case ResourceCostRejectReason::None:
    return "none";
  case ResourceCostRejectReason::UnknownResource:
    return "unknown_resource";
  case ResourceCostRejectReason::DynamicShape:
    return "dynamic_shape";
  case ResourceCostRejectReason::UnknownElementType:
    return "unknown_element_type";
  case ResourceCostRejectReason::Overflow:
    return "overflow";
  case ResourceCostRejectReason::UBOverflow:
    return "ub_overflow";
  case ResourceCostRejectReason::InsufficientParallelism:
    return "insufficient_parallelism";
  case ResourceCostRejectReason::InvalidCandidate:
    return "invalid_candidate";
  }
  return "unknown_resource";
}

bool ResourceSnapshot::isKnown() const {
  return ubCapacityBytes != 0 && reservedUBBytes <= ubCapacityBytes &&
         deviceCoreCount != 0 && minProgramsPerCore != 0 &&
         ubSafetyPercent > 0 && ubSafetyPercent <= 100;
}

std::optional<uint64_t> ResourceSnapshot::getSafeUBBudget() const {
  if (!isKnown())
    return std::nullopt;

  const uint64_t available = ubCapacityBytes - reservedUBBytes;
  const uint64_t wholePercent = available / 100;
  const uint64_t remainder = available % 100;
  uint64_t budget = 0;
  if (!checkedMul(wholePercent, ubSafetyPercent, budget))
    return std::nullopt;
  uint64_t remainderBudget = 0;
  if (!checkedMul(remainder, ubSafetyPercent, remainderBudget))
    return std::nullopt;
  if (!checkedAdd(budget, remainderBudget / 100, budget))
    return std::nullopt;
  return budget;
}

ResourceSnapshot ResourceSnapshot::fromExplicit(uint64_t ubCapacity,
                                                unsigned coreCount,
                                                unsigned minPrograms,
                                                unsigned safetyPercent,
                                                uint64_t reservedUB) {
  ResourceSnapshot result;
  result.ubCapacityBytes = ubCapacity;
  result.reservedUBBytes = reservedUB;
  result.deviceCoreCount = coreCount;
  result.minProgramsPerCore = minPrograms;
  result.ubSafetyPercent = safetyPercent;
  return result;
}

ResourceSnapshot ResourceSnapshot::fromHardwareConfig(
    const ascend::HardwareConfig &hardware, unsigned minPrograms,
    unsigned safetyPercent, uint64_t reservedUB) {
#if TRITON_ASCEND_HAS_INPROC_COSTMODEL
  const ascend::MemorySpace *ub = hardware.getMemorySpace("ub");
  const int vectorCores = hardware.getNumAIVCores();
  if (!ub || ub->sizeBytes == 0 ||
      ub->sizeBytes > std::numeric_limits<uint64_t>::max() || vectorCores <= 0)
    return {};
  return fromExplicit(static_cast<uint64_t>(ub->sizeBytes),
                      static_cast<unsigned>(vectorCores), minPrograms,
                      safetyPercent, reservedUB);
#else
  (void)hardware;
  (void)minPrograms;
  (void)safetyPercent;
  (void)reservedUB;
  return {};
#endif
}

std::optional<uint64_t> cfg::getStaticTensorBytes(Type type) {
  auto tensor = dyn_cast<RankedTensorType>(type);
  if (!tensor || !tensor.hasStaticShape())
    return std::nullopt;

  std::optional<unsigned> elementBits =
      getElementBitWidth(tensor.getElementType());
  if (!elementBits || *elementBits == 0)
    return std::nullopt;

  uint64_t elements = 1;
  for (int64_t dimension : tensor.getShape()) {
    if (dimension < 0)
      return std::nullopt;
    if (!checkedMul(elements, static_cast<uint64_t>(dimension), elements))
      return std::nullopt;
  }

  uint64_t totalBits = 0;
  if (!checkedMul(elements, *elementBits, totalBits))
    return std::nullopt;
  if (totalBits > std::numeric_limits<uint64_t>::max() - 7)
    return std::nullopt;
  return (totalBits + 7) / 8;
}

LiveByteEstimate cfg::estimatePeakLiveBytes(Operation *root) {
  LiveByteEstimate estimate;
  if (!root)
    return estimate;

  llvm::DenseMap<Operation *, uint64_t> order;
  llvm::SmallVector<Operation *, 64> operations;
  root->walk<WalkOrder::PreOrder>([&](Operation *operation) {
    order[operation] = operations.size();
    operations.push_back(operation);
  });

  llvm::DenseMap<Value, Value> canonicalValues;
  for (Operation *operation : operations) {
    if (!isForLikeLoop(operation))
      continue;

    Block &body = operation->getRegion(0).front();
    const unsigned initCount =
        operation->getNumOperands() < 3 ? 0 : operation->getNumOperands() - 3;
    if (body.getNumArguments() < initCount + 1 ||
        operation->getNumResults() != initCount)
      continue;

    for (unsigned index = 0; index < initCount; ++index)
      canonicalValues[body.getArgument(index + 1)] =
          findCanonicalValue(operation->getOperand(3 + index), canonicalValues);

    Operation *terminator = body.getTerminator();
    if (!terminator || terminator->getName().getStringRef() != "scf.yield" ||
        terminator->getNumOperands() != initCount)
      continue;
    for (unsigned index = 0; index < initCount; ++index)
      canonicalValues[operation->getResult(index)] =
          findCanonicalValue(terminator->getOperand(index), canonicalValues);
  }

  struct IntervalState {
    Value value;
    uint64_t begin = 0;
    uint64_t end = 0;
    uint64_t bytes = 0;
  };
  llvm::DenseMap<Value, IntervalState> intervals;

  auto addValue = [&](Value value) {
    if (!isTensorValue(value))
      return;
    Value canonical = findCanonicalValue(value, canonicalValues);
    if (intervals.count(canonical))
      return;

    std::optional<uint64_t> bytes = getStaticTensorBytes(canonical.getType());
    if (!bytes) {
      estimate.reason = isa<UnrankedTensorType>(canonical.getType()) ||
                                (isa<RankedTensorType>(canonical.getType()) &&
                                 !cast<RankedTensorType>(canonical.getType())
                                      .hasStaticShape())
                            ? ResourceCostRejectReason::DynamicShape
                            : ResourceCostRejectReason::UnknownElementType;
      return;
    }
    const uint64_t begin = getDefinitionOrder(canonical, order);
    intervals.try_emplace(canonical,
                          IntervalState{canonical, begin, begin, *bytes});
  };

  for (Operation *operation : operations) {
    for (Value value : operation->getResults())
      addValue(value);
    for (Region &region : operation->getRegions()) {
      for (Block &block : region)
        for (BlockArgument argument : block.getArguments())
          addValue(argument);
    }
  }

  if (estimate.reason == ResourceCostRejectReason::DynamicShape ||
      estimate.reason == ResourceCostRejectReason::UnknownElementType)
    return estimate;

  for (Operation *operation : operations) {
    const uint64_t useOrder = order.lookup(operation);
    for (Value operand : operation->getOperands()) {
      Value canonical = findCanonicalValue(operand, canonicalValues);
      auto it = intervals.find(canonical);
      if (it != intervals.end())
        it->second.end = std::max(it->second.end, useOrder);
    }
  }

  // Values captured from an enclosing scope are required by every dynamic loop
  // iteration.  Extending them to the end of the body prevents a lexical
  // single-iteration walk from dropping an invariant between iterations.
  for (Operation *loop : operations) {
    if (!isForLikeLoop(loop))
      continue;

    uint64_t loopEnd = order.lookup(loop);
    loop->getRegion(0).walk<WalkOrder::PreOrder>([&](Operation *nested) {
      loopEnd = std::max(loopEnd, order.lookup(nested));
    });
    loop->getRegion(0).walk<WalkOrder::PreOrder>([&](Operation *nested) {
      for (Value operand : nested->getOperands()) {
        Value canonical = findCanonicalValue(operand, canonicalValues);
        auto it = intervals.find(canonical);
        if (it != intervals.end() && !isDefinedInside(canonical, loop))
          it->second.end = std::max(it->second.end, loopEnd);
      }
    });
  }

  struct Event {
    uint64_t position;
    uint64_t bytes;
    bool starts;
  };
  llvm::SmallVector<Event, 32> events;
  events.reserve(intervals.size() * 2);
  for (const auto &entry : intervals) {
    const IntervalState &state = entry.second;
    estimate.intervals.push_back(
        LiveTensorInterval{state.value, state.begin, state.end, state.bytes});
    events.push_back(Event{state.begin, state.bytes, true});
    if (state.end == std::numeric_limits<uint64_t>::max()) {
      estimate.overflow = true;
      estimate.reason = ResourceCostRejectReason::Overflow;
      return estimate;
    }
    events.push_back(Event{state.end + 1, state.bytes, false});
  }

  llvm::sort(events, [](const Event &lhs, const Event &rhs) {
    if (lhs.position != rhs.position)
      return lhs.position < rhs.position;
    return !lhs.starts && rhs.starts;
  });

  uint64_t liveBytes = 0;
  for (const Event &event : events) {
    if (event.starts) {
      if (!checkedAdd(liveBytes, event.bytes, liveBytes)) {
        estimate.overflow = true;
        estimate.reason = ResourceCostRejectReason::Overflow;
        return estimate;
      }
      estimate.peakLiveBytes = std::max(estimate.peakLiveBytes, liveBytes);
      continue;
    }
    if (event.bytes > liveBytes) {
      estimate.overflow = true;
      estimate.reason = ResourceCostRejectReason::Overflow;
      return estimate;
    }
    liveBytes -= event.bytes;
  }

  estimate.known = true;
  estimate.reason = ResourceCostRejectReason::None;
  return estimate;
}

CandidateEvaluation
cfg::evaluateCandidateCost(const ResourceSnapshot &resources,
                           const CandidateCost &candidate,
                           const CostModelWeights &weights) {
  if (!resources.isKnown())
    return reject(candidate, ResourceCostRejectReason::UnknownResource);

  std::optional<uint64_t> safeBudget = resources.getSafeUBBudget();
  if (!safeBudget)
    return reject(candidate, ResourceCostRejectReason::Overflow);

  uint64_t requiredParallelPrograms = 0;
  if (!checkedMul(resources.deviceCoreCount, resources.minProgramsPerCore,
                  requiredParallelPrograms))
    return reject(candidate, ResourceCostRejectReason::Overflow, *safeBudget);

  if (candidate.hasDynamicShape)
    return reject(candidate, ResourceCostRejectReason::DynamicShape,
                  *safeBudget, requiredParallelPrograms);
  if (candidate.hasUnknownResource || !candidate.hasPeakLiveBytes)
    return reject(candidate, ResourceCostRejectReason::UnknownResource,
                  *safeBudget, requiredParallelPrograms);
  if (candidate.plan.tensorizeFactor == 0 || candidate.plan.blockT == 0 ||
      candidate.plan.staticAxisFusionFactor == 0 ||
      candidate.logicalTasksBefore == 0 || candidate.logicalTasksAfter == 0 ||
      candidate.actualProgramsBefore == 0 || candidate.actualProgramsAfter == 0)
    return reject(candidate, ResourceCostRejectReason::InvalidCandidate,
                  *safeBudget, requiredParallelPrograms);
  if (candidate.persistent &&
      candidate.actualProgramsAfter > candidate.logicalTasksAfter)
    return reject(candidate, ResourceCostRejectReason::InvalidCandidate,
                  *safeBudget, requiredParallelPrograms);
  if (candidate.estimatedPeakLiveBytes > *safeBudget)
    return reject(candidate, ResourceCostRejectReason::UBOverflow, *safeBudget,
                  requiredParallelPrograms);
  if (candidate.actualProgramsAfter < requiredParallelPrograms)
    return reject(candidate, ResourceCostRejectReason::InsufficientParallelism,
                  *safeBudget, requiredParallelPrograms);

  uint64_t workBefore = candidate.workPerProgramBefore;
  if (workBefore == 0)
    workBefore =
        candidate.logicalTasksBefore / candidate.actualProgramsBefore +
        (candidate.logicalTasksBefore % candidate.actualProgramsBefore != 0);
  uint64_t workAfter = candidate.workPerProgramAfter;
  if (workAfter == 0)
    workAfter =
        candidate.logicalTasksAfter / candidate.actualProgramsAfter +
        (candidate.logicalTasksAfter % candidate.actualProgramsAfter != 0);
  if (workBefore == 0 || workAfter == 0)
    return reject(candidate, ResourceCostRejectReason::InvalidCandidate,
                  *safeBudget, requiredParallelPrograms);

  uint64_t gains = 0;
  uint64_t penalties = 0;
  if (!addWeightedChange(candidate.actualProgramsBefore,
                         candidate.actualProgramsAfter,
                         weights.programReduction, gains, penalties) ||
      !addWeightedChange(candidate.launchesBefore, candidate.launchesAfter,
                         weights.launchReduction, gains, penalties) ||
      !addWeightedChange(candidate.gmReadBytesBefore,
                         candidate.gmReadBytesAfter, weights.gmByte, gains,
                         penalties) ||
      !addWeightedChange(candidate.gmWriteBytesBefore,
                         candidate.gmWriteBytesAfter, weights.gmByte, gains,
                         penalties) ||
      !addWeightedChange(candidate.storeCountBefore, candidate.storeCountAfter,
                         weights.store, gains, penalties) ||
      !addWeightedChange(candidate.addressCalculationsBefore,
                         candidate.addressCalculationsAfter,
                         weights.addressCalculation, gains, penalties) ||
      !addWeightedChange(candidate.baselinePeakLiveBytes,
                         candidate.estimatedPeakLiveBytes, weights.liveByte,
                         gains, penalties) ||
      !addWeightedChange(workBefore, workAfter, weights.workItem, gains,
                         penalties) ||
      gains > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
      penalties > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
    return reject(candidate, ResourceCostRejectReason::Overflow, *safeBudget,
                  requiredParallelPrograms);

  CandidateEvaluation evaluation;
  evaluation.candidate = candidate;
  evaluation.accepted = true;
  evaluation.reason = ResourceCostRejectReason::None;
  evaluation.benefitScore =
      static_cast<int64_t>(gains) - static_cast<int64_t>(penalties);
  evaluation.safeUBBudgetBytes = *safeBudget;
  evaluation.requiredParallelPrograms = requiredParallelPrograms;
  evaluation.effectiveWorkPerProgram = workAfter;
  evaluation.remark = formatCandidateRemark(evaluation);
  return evaluation;
}

llvm::SmallVector<CandidatePlan>
cfg::enumerateCandidatePlans(llvm::ArrayRef<unsigned> tensorizeFactors,
                             llvm::ArrayRef<unsigned> blockTFactors,
                             llvm::ArrayRef<unsigned> staticAxisFusionFactors,
                             llvm::StringRef stableIdPrefix) {
  llvm::SmallVector<unsigned> tensorize;
  llvm::SmallVector<unsigned> blockT;
  llvm::SmallVector<unsigned> staticFusion;
  addCanonicalFactors(tensorizeFactors, tensorize);
  addCanonicalFactors(blockTFactors, blockT);
  addCanonicalFactors(staticAxisFusionFactors, staticFusion);

  llvm::SmallVector<CandidatePlan> plans;
  uint64_t ordinal = 0;
  for (unsigned tensorizeFactor : tensorize) {
    for (unsigned tokenFactor : blockT) {
      for (unsigned staticFactor : staticFusion) {
        CandidatePlan plan;
        plan.tensorizeFactor = tensorizeFactor;
        plan.blockT = tokenFactor;
        plan.staticAxisFusionFactor = staticFactor;
        plan.stableId =
            (llvm::Twine(stableIdPrefix) + ".t" + llvm::Twine(tensorizeFactor) +
             ".b" + llvm::Twine(tokenFactor) + ".f" + llvm::Twine(staticFactor))
                .str();
        plan.sourceOrdinal = ordinal++;
        plans.push_back(std::move(plan));
      }
    }
  }
  return plans;
}

void cfg::sortCandidateEvaluations(
    llvm::MutableArrayRef<CandidateEvaluation> values) {
  std::stable_sort(
      values.begin(), values.end(),
      [](const CandidateEvaluation &lhs, const CandidateEvaluation &rhs) {
        if (lhs.accepted != rhs.accepted)
          return lhs.accepted;
        if (lhs.benefitScore != rhs.benefitScore)
          return lhs.benefitScore > rhs.benefitScore;
        const CandidatePlan &left = lhs.candidate.plan;
        const CandidatePlan &right = rhs.candidate.plan;
        return std::tie(left.tensorizeFactor, left.blockT,
                        left.staticAxisFusionFactor, left.stableId,
                        left.sourceOrdinal) <
               std::tie(right.tensorizeFactor, right.blockT,
                        right.staticAxisFusionFactor, right.stableId,
                        right.sourceOrdinal);
      });
}

std::string cfg::formatCandidateRemark(const CandidateEvaluation &evaluation) {
  const CandidateCost &candidate = evaluation.candidate;
  std::string remark;
  llvm::raw_string_ostream stream(remark);
  stream << "resource-cost candidate=" << candidate.plan.stableId
         << " accepted=" << (evaluation.accepted ? "true" : "false")
         << " reason=" << getResourceCostRejectReasonName(evaluation.reason)
         << " score=" << evaluation.benefitScore
         << " tensorize_factor=" << candidate.plan.tensorizeFactor
         << " block_t=" << candidate.plan.blockT
         << " static_axis_fusion_factor="
         << candidate.plan.staticAxisFusionFactor
         << " logical_tasks=" << candidate.logicalTasksBefore << "->"
         << candidate.logicalTasksAfter
         << " actual_programs=" << candidate.actualProgramsBefore << "->"
         << candidate.actualProgramsAfter
         << " required_programs=" << evaluation.requiredParallelPrograms
         << " gm_read_bytes=" << candidate.gmReadBytesBefore << "->"
         << candidate.gmReadBytesAfter
         << " gm_write_bytes=" << candidate.gmWriteBytesBefore << "->"
         << candidate.gmWriteBytesAfter
         << " stores=" << candidate.storeCountBefore << "->"
         << candidate.storeCountAfter
         << " address_calculations=" << candidate.addressCalculationsBefore
         << "->" << candidate.addressCalculationsAfter
         << " peak_live_bytes=" << candidate.baselinePeakLiveBytes << "->"
         << candidate.estimatedPeakLiveBytes
         << " ub_budget_bytes=" << evaluation.safeUBBudgetBytes
         << " work_per_program=" << evaluation.effectiveWorkPerProgram
         << " persistent=" << (candidate.persistent ? "true" : "false");
  return std::move(stream.str());
}

void cfg::emitCandidateRemark(Operation *anchor,
                              const CandidateEvaluation &evaluation) {
  if (anchor)
    anchor->emitRemark() << evaluation.remark;
}

std::string
cfg::formatProfilerCalibrationRecord(const ProfilerCalibrationRecord &record) {
  std::string formatted;
  llvm::raw_string_ostream stream(formatted);
  stream << "resource-cost-calibration schema=v1 kernel=" << record.kernelName
         << " profiler_artifact=" << record.profilerArtifact
         << " samples=" << record.sampleCount
         << " median_ns=" << record.medianNanoseconds
         << " p90_ns=" << record.p90Nanoseconds << " "
         << formatCandidateRemark(record.evaluation);
  return std::move(stream.str());
}

ResourceCostAnalysis::ResourceCostAnalysis(Operation *root,
                                           ResourceSnapshot resources)
    : resources(resources), liveBytes(estimatePeakLiveBytes(root)) {}

CandidateEvaluation
ResourceCostAnalysis::evaluate(const CandidateCost &candidate) const {
  CandidateCost effectiveCandidate = candidate;
  if (!effectiveCandidate.hasPeakLiveBytes && liveBytes.known) {
    effectiveCandidate.hasPeakLiveBytes = true;
    effectiveCandidate.baselinePeakLiveBytes = liveBytes.peakLiveBytes;
    effectiveCandidate.estimatedPeakLiveBytes = liveBytes.peakLiveBytes;
  }
  if (!liveBytes.known) {
    if (liveBytes.reason == ResourceCostRejectReason::DynamicShape)
      effectiveCandidate.hasDynamicShape = true;
    else
      effectiveCandidate.hasUnknownResource = true;
  }
  CandidateEvaluation evaluation =
      evaluateCandidateCost(resources, effectiveCandidate);
  if (liveBytes.reason == ResourceCostRejectReason::Overflow) {
    evaluation.accepted = false;
    evaluation.reason = ResourceCostRejectReason::Overflow;
    evaluation.benefitScore = 0;
    evaluation.remark = formatCandidateRemark(evaluation);
  }
  return evaluation;
}
