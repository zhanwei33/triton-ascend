#include "TritonToGraph/ResourceCostModel.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/Parser/Parser.h"

#include <gtest/gtest.h>

#include <limits>
#include <string>

using namespace mlir;
using namespace mlir::triton::cfg;

namespace {

OwningOpRef<ModuleOp> parseModule(MLIRContext &context, llvm::StringRef text) {
  context.getOrLoadDialect<arith::ArithDialect>();
  context.getOrLoadDialect<func::FuncDialect>();
  context.getOrLoadDialect<scf::SCFDialect>();
  context.getOrLoadDialect<tensor::TensorDialect>();
  return parseSourceString<ModuleOp>(text, &context);
}

CandidateCost makeCandidate(const CandidatePlan &plan = {}) {
  CandidateCost candidate;
  candidate.plan = plan;
  candidate.plan.stableId =
      candidate.plan.stableId.empty() ? "candidate" : candidate.plan.stableId;
  candidate.logicalTasksBefore = 256;
  candidate.logicalTasksAfter = 256;
  candidate.actualProgramsBefore = 256;
  candidate.actualProgramsAfter = 64;
  candidate.launchesBefore = 1;
  candidate.launchesAfter = 1;
  candidate.gmReadBytesBefore = 4096;
  candidate.gmReadBytesAfter = 2048;
  candidate.gmWriteBytesBefore = 4096;
  candidate.gmWriteBytesAfter = 2048;
  candidate.storeCountBefore = 8;
  candidate.storeCountAfter = 4;
  candidate.addressCalculationsBefore = 128;
  candidate.addressCalculationsAfter = 64;
  candidate.baselinePeakLiveBytes = 256;
  candidate.estimatedPeakLiveBytes = 512;
  candidate.hasPeakLiveBytes = true;
  return candidate;
}

ResourceSnapshot knownResources() {
  return ResourceSnapshot::fromExplicit(/*ubCapacityBytes=*/4096,
                                        /*deviceCoreCount=*/8,
                                        /*minProgramsPerCore=*/2,
                                        /*ubSafetyPercent=*/80);
}

} // namespace

TEST(GraphOptimizationResourceCostModelTest,
     SumsSimultaneouslyLiveStaticTensorsFromIR) {
  MLIRContext context;
  auto module = parseModule(context, R"mlir(
module {
  func.func @simultaneous() {
    %a = tensor.empty() : tensor<16xf32>
    %b = tensor.empty() : tensor<16xf32>
    %sum = arith.addf %a, %b : tensor<16xf32>
    return
  }
}
)mlir");
  ASSERT_TRUE(module);

  LiveByteEstimate estimate = estimatePeakLiveBytes(module->getOperation());
  ASSERT_TRUE(estimate.known);
  EXPECT_EQ(estimate.peakLiveBytes, 3 * 16 * sizeof(float));
}

TEST(GraphOptimizationResourceCostModelTest,
     HandlesLoopCarrierOnceAndExtendsInvariantLiveRange) {
  MLIRContext context;
  auto module = parseModule(context, R"mlir(
module {
  func.func @loop_once() {
    %c0 = arith.constant 0 : index
    %c4 = arith.constant 4 : index
    %c1 = arith.constant 1 : index
    %init = tensor.empty() : tensor<4xf32>
    %invariant = tensor.empty() : tensor<4xf32>
    %result = scf.for %i = %c0 to %c4 step %c1 iter_args(%acc = %init) -> (tensor<4xf32>) {
      %first = arith.addf %acc, %invariant : tensor<4xf32>
      %tail = tensor.empty() : tensor<4xf32>
      %next = arith.addf %first, %tail : tensor<4xf32>
      scf.yield %next : tensor<4xf32>
    }
    return
  }
}
)mlir");
  ASSERT_TRUE(module);

  LiveByteEstimate estimate = estimatePeakLiveBytes(module->getOperation());
  ASSERT_TRUE(estimate.known);
  // The loop body is evaluated once for liveness: carrier, invariant, first,
  // tail, and next coexist at its peak.  It must not grow with the trip count.
  EXPECT_EQ(estimate.peakLiveBytes, 5 * 4 * sizeof(float));
}

TEST(GraphOptimizationResourceCostModelTest,
     SupportsBF16FP16FP32AndRejectsDynamicOrOverflowShapes) {
  MLIRContext context;
  const auto bf16Bytes = getStaticTensorBytes(
      RankedTensorType::get({16}, BFloat16Type::get(&context)));
  const auto fp16Bytes = getStaticTensorBytes(
      RankedTensorType::get({16}, Float16Type::get(&context)));
  const auto fp32Bytes = getStaticTensorBytes(
      RankedTensorType::get({16}, Float32Type::get(&context)));
  const auto zeroBytes = getStaticTensorBytes(
      RankedTensorType::get({0}, Float32Type::get(&context)));
  ASSERT_TRUE(bf16Bytes);
  ASSERT_TRUE(fp16Bytes);
  ASSERT_TRUE(fp32Bytes);
  ASSERT_TRUE(zeroBytes);
  EXPECT_EQ(*bf16Bytes, 32u);
  EXPECT_EQ(*fp16Bytes, 32u);
  EXPECT_EQ(*fp32Bytes, 64u);
  EXPECT_EQ(*zeroBytes, 0u);
  EXPECT_FALSE(getStaticTensorBytes(RankedTensorType::get(
      {ShapedType::kDynamic}, Float32Type::get(&context))));
  EXPECT_FALSE(getStaticTensorBytes(RankedTensorType::get(
      {std::numeric_limits<int64_t>::max(), 2}, Float32Type::get(&context))));
}

TEST(GraphOptimizationResourceCostModelTest,
     RejectsUnknownUBOverflowAndInsufficientParallelism) {
  CandidateCost candidate = makeCandidate();
  EXPECT_EQ(evaluateCandidateCost({}, candidate).reason,
            ResourceCostRejectReason::UnknownResource);

  CandidateCost overflow = candidate;
  overflow.estimatedPeakLiveBytes = 4096;
  EXPECT_EQ(evaluateCandidateCost(knownResources(), overflow).reason,
            ResourceCostRejectReason::UBOverflow);

  CandidateCost parallel = candidate;
  parallel.actualProgramsAfter = 15;
  EXPECT_EQ(evaluateCandidateCost(knownResources(), parallel).reason,
            ResourceCostRejectReason::InsufficientParallelism);

  CandidateCost arithmeticOverflow = candidate;
  arithmeticOverflow.gmReadBytesBefore = std::numeric_limits<uint64_t>::max();
  arithmeticOverflow.gmReadBytesAfter = 0;
  EXPECT_EQ(evaluateCandidateCost(knownResources(), arithmeticOverflow).reason,
            ResourceCostRejectReason::Overflow);
}

TEST(GraphOptimizationResourceCostModelTest,
     PersistentCandidatesPriceLogicalTasksAndActualProgramsSeparately) {
  CandidateCost candidate = makeCandidate();
  candidate.plan = CandidatePlan{2, 4, 1, "persistent", 0};
  candidate.persistent = true;
  candidate.logicalTasksBefore = 4096;
  candidate.logicalTasksAfter = 1024;
  candidate.actualProgramsBefore = 4096;
  candidate.actualProgramsAfter = 32;

  CandidateEvaluation evaluation =
      evaluateCandidateCost(knownResources(), candidate);
  ASSERT_TRUE(evaluation.accepted);
  EXPECT_EQ(evaluation.effectiveWorkPerProgram, 32u);
  EXPECT_NE(evaluation.remark.find("logical_tasks=4096->1024"),
            std::string::npos);
  EXPECT_NE(evaluation.remark.find("actual_programs=4096->32"),
            std::string::npos);
}

TEST(GraphOptimizationResourceCostModelTest,
     EnumeratesAndOrdersCandidatesDeterministicallyWithCompleteRemarks) {
  auto first = enumerateCandidatePlans({4, 1, 2, 2}, {2, 1}, {2, 1}, "plan");
  auto second = enumerateCandidatePlans({4, 1, 2, 2}, {2, 1}, {2, 1}, "plan");
  ASSERT_EQ(first.size(), 12u);
  ASSERT_EQ(first.size(), second.size());
  for (size_t index = 0; index < first.size(); ++index) {
    EXPECT_EQ(first[index].stableId, second[index].stableId);
    EXPECT_EQ(first[index].tensorizeFactor, second[index].tensorizeFactor);
    EXPECT_EQ(first[index].blockT, second[index].blockT);
    EXPECT_EQ(first[index].staticAxisFusionFactor,
              second[index].staticAxisFusionFactor);
  }

  CandidateCost lhs = makeCandidate(first.back());
  CandidateCost rhs = makeCandidate(first.front());
  lhs.gmReadBytesAfter = rhs.gmReadBytesAfter;
  lhs.gmWriteBytesAfter = rhs.gmWriteBytesAfter;
  lhs.actualProgramsAfter = rhs.actualProgramsAfter;
  lhs.storeCountAfter = rhs.storeCountAfter;
  lhs.addressCalculationsAfter = rhs.addressCalculationsAfter;
  lhs.estimatedPeakLiveBytes = rhs.estimatedPeakLiveBytes;
  llvm::SmallVector<CandidateEvaluation> evaluations = {
      evaluateCandidateCost(knownResources(), lhs),
      evaluateCandidateCost(knownResources(), rhs)};
  sortCandidateEvaluations(evaluations);
  ASSERT_TRUE(evaluations.front().accepted);
  EXPECT_EQ(evaluations.front().candidate.plan.stableId,
            first.front().stableId);
  EXPECT_NE(evaluations.front().remark.find("tensorize_factor="),
            std::string::npos);
  EXPECT_NE(evaluations.front().remark.find("block_t="), std::string::npos);
  EXPECT_NE(evaluations.front().remark.find("static_axis_fusion_factor="),
            std::string::npos);
  EXPECT_NE(evaluations.front().remark.find("peak_live_bytes="),
            std::string::npos);

  ProfilerCalibrationRecord record;
  record.kernelName = "norm_q";
  record.profilerArtifact = "kernel_details_norm_q.csv";
  record.sampleCount = 20;
  record.medianNanoseconds = 123;
  record.p90Nanoseconds = 145;
  record.evaluation = evaluations.front();
  const std::string calibration = formatProfilerCalibrationRecord(record);
  EXPECT_NE(calibration.find("schema=v1"), std::string::npos);
  EXPECT_NE(calibration.find("median_ns=123"), std::string::npos);
  EXPECT_NE(calibration.find("resource-cost candidate="), std::string::npos);
}
