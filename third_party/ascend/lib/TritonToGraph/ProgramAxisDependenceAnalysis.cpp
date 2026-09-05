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
 */

#include "TritonToGraph/ProgramAxisDependenceAnalysis.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/Matchers.h"
#include "mlir/Interfaces/CallInterfaces.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "triton/Dialect/Triton/IR/Dialect.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/STLExtras.h"

#include <cassert>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

using namespace mlir;
using namespace mlir::triton;
using namespace mlir::triton::cfg;

namespace {

struct OffsetForm {
  bool valid = true;
  int64_t programCoefficient = 0;
  int64_t laneMin = 0;
  int64_t laneMax = 0;
};

bool addNoOverflow(int64_t lhs, int64_t rhs, int64_t &result) {
  if ((rhs > 0 && lhs > std::numeric_limits<int64_t>::max() - rhs) ||
      (rhs < 0 && lhs < std::numeric_limits<int64_t>::min() - rhs))
    return false;
  result = lhs + rhs;
  return true;
}

bool multiplyNoOverflow(int64_t lhs, int64_t rhs, int64_t &result) {
  return !__builtin_mul_overflow(lhs, rhs, &result);
}

std::optional<int64_t> getConstantInt(Value value) {
  APInt constant;
  if (!matchPattern(value, m_ConstantInt(&constant)))
    return std::nullopt;
  return constant.getSExtValue();
}

OffsetForm invalidOffsetForm() {
  OffsetForm form;
  form.valid = false;
  return form;
}

OffsetForm addOffsetForms(const OffsetForm &lhs, const OffsetForm &rhs,
                          bool subtractRhs) {
  if (!lhs.valid || !rhs.valid)
    return invalidOffsetForm();
  const int64_t rhsCoefficient = subtractRhs ? -rhs.programCoefficient
                                             : rhs.programCoefficient;
  const int64_t rhsMin = subtractRhs ? -rhs.laneMax : rhs.laneMin;
  const int64_t rhsMax = subtractRhs ? -rhs.laneMin : rhs.laneMax;
  OffsetForm result;
  if (!addNoOverflow(lhs.programCoefficient, rhsCoefficient,
                     result.programCoefficient) ||
      !addNoOverflow(lhs.laneMin, rhsMin, result.laneMin) ||
      !addNoOverflow(lhs.laneMax, rhsMax, result.laneMax))
    return invalidOffsetForm();
  return result;
}

OffsetForm scaleOffsetForm(const OffsetForm &input, int64_t factor) {
  if (!input.valid)
    return invalidOffsetForm();
  OffsetForm result;
  if (!multiplyNoOverflow(input.programCoefficient, factor,
                          result.programCoefficient) ||
      !multiplyNoOverflow(input.laneMin, factor, result.laneMin) ||
      !multiplyNoOverflow(input.laneMax, factor, result.laneMax))
    return invalidOffsetForm();
  if (factor < 0)
    std::swap(result.laneMin, result.laneMax);
  return result;
}

OffsetForm analyzeOffset(Value value, int32_t targetAxis,
                         DenseSet<Value> &visited) {
  if (!visited.insert(value).second)
    return invalidOffsetForm();

  auto finish = [&](OffsetForm form) {
    visited.erase(value);
    return form;
  };

  if (auto pid = value.getDefiningOp<triton::GetProgramIdOp>()) {
    if (pid.getAxisAsInt() != targetAxis)
      return finish(invalidOffsetForm());
    OffsetForm form;
    form.programCoefficient = 1;
    return finish(form);
  }
  if (value.getDefiningOp<triton::GetNumProgramsOp>())
    return finish(invalidOffsetForm());
  if (std::optional<int64_t> constant = getConstantInt(value)) {
    OffsetForm form;
    form.laneMin = *constant;
    form.laneMax = *constant;
    return finish(form);
  }
  if (auto range = value.getDefiningOp<triton::MakeRangeOp>()) {
    if (range.getEnd() <= range.getStart())
      return finish(invalidOffsetForm());
    OffsetForm form;
    form.laneMin = range.getStart();
    form.laneMax = range.getEnd() - 1;
    return finish(form);
  }
  if (auto splat = value.getDefiningOp<triton::SplatOp>())
    return finish(analyzeOffset(splat.getSrc(), targetAxis, visited));
  if (auto broadcast = value.getDefiningOp<triton::BroadcastOp>())
    return finish(analyzeOffset(broadcast.getSrc(), targetAxis, visited));
  if (auto expand = value.getDefiningOp<triton::ExpandDimsOp>())
    return finish(analyzeOffset(expand.getSrc(), targetAxis, visited));
  if (auto add = value.getDefiningOp<arith::AddIOp>()) {
    OffsetForm lhs = analyzeOffset(add.getLhs(), targetAxis, visited);
    OffsetForm rhs = analyzeOffset(add.getRhs(), targetAxis, visited);
    return finish(addOffsetForms(lhs, rhs, /*subtractRhs=*/false));
  }
  if (auto subtract = value.getDefiningOp<arith::SubIOp>()) {
    OffsetForm lhs = analyzeOffset(subtract.getLhs(), targetAxis, visited);
    OffsetForm rhs = analyzeOffset(subtract.getRhs(), targetAxis, visited);
    return finish(addOffsetForms(lhs, rhs, /*subtractRhs=*/true));
  }
  if (auto multiply = value.getDefiningOp<arith::MulIOp>()) {
    if (std::optional<int64_t> lhs = getConstantInt(multiply.getLhs()))
      return finish(
          scaleOffsetForm(analyzeOffset(multiply.getRhs(), targetAxis, visited),
                          *lhs));
    if (std::optional<int64_t> rhs = getConstantInt(multiply.getRhs()))
      return finish(
          scaleOffsetForm(analyzeOffset(multiply.getLhs(), targetAxis, visited),
                          *rhs));
    return finish(invalidOffsetForm());
  }
  return finish(invalidOffsetForm());
}

bool valueDependsOnAxis(Value value, int32_t targetAxis, DenseSet<Value> &seen) {
  if (!seen.insert(value).second)
    return false;
  if (auto pid = value.getDefiningOp<triton::GetProgramIdOp>())
    return pid.getAxisAsInt() == targetAxis;
  Operation *definition = value.getDefiningOp();
  if (!definition)
    return false;
  return llvm::any_of(definition->getOperands(), [&](Value operand) {
    return valueDependsOnAxis(operand, targetAxis, seen);
  });
}

StoreAddressIndependence classifyStoreAddress(triton::StoreOp store,
                                              int32_t targetAxis,
                                              bool &pointerDependsOnAxis) {
  DenseSet<Value> dependencySeen;
  pointerDependsOnAxis =
      valueDependsOnAxis(store.getPtr(), targetAxis, dependencySeen);
  if (!pointerDependsOnAxis)
    return StoreAddressIndependence::NotProgramDependent;

  auto addPtr = store.getPtr().getDefiningOp<triton::AddPtrOp>();
  if (!addPtr)
    return StoreAddressIndependence::Unknown;
  DenseSet<Value> offsetSeen;
  OffsetForm offset = analyzeOffset(addPtr.getOffset(), targetAxis, offsetSeen);
  if (!offset.valid || offset.programCoefficient == 0 ||
      offset.laneMax < offset.laneMin)
    return StoreAddressIndependence::Unknown;

  // Distinct program ids are one coefficient apart.  A static per-program
  // lane interval that is narrower than that coefficient cannot overlap.
  const uint64_t span = static_cast<uint64_t>(offset.laneMax) -
                        static_cast<uint64_t>(offset.laneMin);
  const uint64_t coefficient = offset.programCoefficient < 0
                                   ? static_cast<uint64_t>(-(offset.programCoefficient + 1)) + 1
                                   : static_cast<uint64_t>(offset.programCoefficient);
  return coefficient > span ? StoreAddressIndependence::ProvenDisjoint
                            : StoreAddressIndependence::Unknown;
}

bool isControlFlowEscape(Operation *operation) {
  if (isa<triton::ReturnOp, CallOpInterface>(operation))
    return true;
  StringRef name = operation->getName().getStringRef();
  return name == "cf.br" || name == "cf.cond_br" || name.ends_with(".yield");
}

bool hasUnsupportedEffect(Operation *operation) {
  if (isa<triton::StoreOp, triton::LoadOp>(operation))
    return false;
  if (isa<CallOpInterface>(operation))
    return true;
  StringRef name = operation->getName().getStringRef();
  if (name.contains("atomic") || name.contains("barrier") ||
      name.contains("print"))
    return true;
  auto effects = dyn_cast<MemoryEffectOpInterface>(operation);
  if (!effects)
    return false;
  SmallVector<MemoryEffects::EffectInstance> effectInstances;
  effects.getEffects(effectInstances);
  return llvm::any_of(effectInstances, [](const auto &effect) {
    return isa<MemoryEffects::Write, MemoryEffects::Allocate,
               MemoryEffects::Free>(effect.getEffect());
  });
}

void recordOperationFacts(Operation *operation, ProgramAxisDependence &info,
                          DenseSet<Operation *> &seenStores) {
  if (auto reduce = dyn_cast<triton::ReduceOp>(operation))
    info.reductionAxes.push_back(reduce.getAxis());
  if (auto scan = dyn_cast<triton::ScanOp>(operation))
    info.reductionAxes.push_back(scan.getAxis());

  if (isControlFlowEscape(operation))
    info.escapes = true;
  if (auto store = dyn_cast<triton::StoreOp>(operation)) {
    info.hasSideEffects = true;
    if (seenStores.insert(operation).second) {
      bool pointerDependsOnAxis = false;
      StoreAddressIndependence independence =
          classifyStoreAddress(store, info.axis, pointerDependsOnAxis);
      info.stores.push_back(
          {operation, pointerDependsOnAxis, independence});
    }
    return;
  }
  if (hasUnsupportedEffect(operation)) {
    info.hasSideEffects = true;
    info.hasUnsupportedSideEffects = true;
  }
}

void buildClosure(ProgramAxisDependence &info) {
  DenseSet<Value> visitedValues;
  DenseSet<Operation *> visitedOperations;
  DenseSet<Operation *> seenStores;
  SmallVector<Value> worklist(info.programIds.begin(), info.programIds.end());

  while (!worklist.empty()) {
    Value value = worklist.pop_back_val();
    if (!visitedValues.insert(value).second)
      continue;
    for (OpOperand &use : value.getUses()) {
      Operation *operation = use.getOwner();
      if (!visitedOperations.insert(operation).second)
        continue;
      info.dependenceClosure.push_back(operation);
      recordOperationFacts(operation, info, seenStores);
      for (Value result : operation->getResults())
        worklist.push_back(result);
    }
  }
}

} // namespace

bool ProgramAxisDependence::hasOnlyDisjointStoreAddresses() const {
  return !stores.empty() && llvm::all_of(stores, [](const auto &store) {
    return store.pointerDependsOnAxis &&
           store.independence == StoreAddressIndependence::ProvenDisjoint;
  });
}

bool ProgramAxisDependence::isIndependentAxisTransformCandidate() const {
  return hasProgramId() && !escapes && !hasUnsupportedSideEffects &&
         !readsNumPrograms && hasOnlyDisjointStoreAddresses();
}

ProgramAxisDependenceAnalysis::ProgramAxisDependenceAnalysis(
    triton::FuncOp function) {
  for (int32_t axis = 0; axis < 3; ++axis)
    axes[axis].axis = axis;

  function.walk([&](triton::GetProgramIdOp pid) {
    const int32_t axis = pid.getAxisAsInt();
    if (axis >= 0 && axis < static_cast<int32_t>(axes.size()))
      axes[axis].programIds.push_back(pid.getResult());
  });
  function.walk([&](triton::GetNumProgramsOp programs) {
    const int32_t axis = programs.getAxisAsInt();
    if (axis >= 0 && axis < static_cast<int32_t>(axes.size()))
      axes[axis].readsNumPrograms = true;
  });
  for (ProgramAxisDependence &axis : axes)
    buildClosure(axis);
}

const ProgramAxisDependence &ProgramAxisDependenceAnalysis::get(
    int32_t axis) const {
  assert(axis >= 0 && axis < static_cast<int32_t>(axes.size()) &&
         "program axis must be x, y, or z");
  return axes[axis];
}
