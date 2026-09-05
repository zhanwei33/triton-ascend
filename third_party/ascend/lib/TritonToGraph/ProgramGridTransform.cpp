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

#include "TritonToGraph/ProgramGridTransform.h"

#include "mlir/IR/Builders.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"

#include <array>
#include <optional>

using namespace mlir;
using namespace mlir::triton::cfg;

namespace {

constexpr llvm::StringLiteral kVersion = "version";
constexpr llvm::StringLiteral kTransforms = "transforms";
constexpr llvm::StringLiteral kOrder = "order";
constexpr llvm::StringLiteral kKind = "kind";
constexpr llvm::StringLiteral kAxis = "axis";
constexpr llvm::StringLiteral kFactor = "factor";
constexpr llvm::StringLiteral kLogicalExtent = "logical_extent";
constexpr llvm::StringLiteral kPersistentCoverage = "persistent_coverage";
constexpr llvm::StringLiteral kGridStrideAbiVerified =
    "grid_stride_abi_verified";
constexpr llvm::StringLiteral kCeilDiv = "ceil_div";

bool hasExactKeys(DictionaryAttr dictionary, ArrayRef<llvm::StringRef> keys) {
  if (dictionary.size() != keys.size())
    return false;
  return llvm::all_of(keys,
                      [&](llvm::StringRef key) { return dictionary.get(key); });
}

std::optional<int64_t> getInteger(DictionaryAttr dictionary,
                                  llvm::StringRef name) {
  auto value = dyn_cast_or_null<IntegerAttr>(dictionary.get(name));
  if (!value)
    return std::nullopt;
  return value.getInt();
}

std::optional<bool> getBoolean(DictionaryAttr dictionary,
                               llvm::StringRef name) {
  auto value = dyn_cast_or_null<BoolAttr>(dictionary.get(name));
  if (!value)
    return std::nullopt;
  return value.getValue();
}

} // namespace

FailureOr<ProgramGridTransformContract>
mlir::triton::cfg::parseProgramGridTransformContract(Attribute attribute) {
  auto contract = dyn_cast_or_null<DictionaryAttr>(attribute);
  if (!contract || !hasExactKeys(contract, {kVersion, kTransforms}))
    return failure();

  std::optional<int64_t> version = getInteger(contract, kVersion);
  if (!version || *version != kProgramGridTransformsVersion)
    return failure();

  auto transforms = dyn_cast_or_null<ArrayAttr>(contract.get(kTransforms));
  if (!transforms || transforms.empty())
    return failure();

  ProgramGridTransformContract parsed;
  parsed.version = *version;
  std::array<std::optional<int64_t>, 3> originalExtentByAxis;
  unsigned persistentTransformCount = 0;

  for (auto [expectedOrder, attribute] : llvm::enumerate(transforms)) {
    auto transform = dyn_cast<DictionaryAttr>(attribute);
    if (!transform ||
        !hasExactKeys(transform,
                      {kOrder, kKind, kAxis, kFactor, kLogicalExtent,
                       kPersistentCoverage, kGridStrideAbiVerified}))
      return failure();

    std::optional<int64_t> order = getInteger(transform, kOrder);
    auto kind = dyn_cast_or_null<StringAttr>(transform.get(kKind));
    std::optional<int64_t> axis = getInteger(transform, kAxis);
    std::optional<int64_t> factor = getInteger(transform, kFactor);
    std::optional<int64_t> logicalExtent = getInteger(transform, kLogicalExtent);
    std::optional<bool> persistentCoverage =
        getBoolean(transform, kPersistentCoverage);
    std::optional<bool> gridStrideAbiVerified =
        getBoolean(transform, kGridStrideAbiVerified);
    if (!order || !kind || !axis || !factor || !logicalExtent ||
        !persistentCoverage || !gridStrideAbiVerified ||
        *order != static_cast<int64_t>(expectedOrder) ||
        kind.getValue() != kCeilDiv || *axis < 0 || *axis > 2 ||
        *factor < 2 || *logicalExtent < 1 ||
        (*gridStrideAbiVerified && !*persistentCoverage) ||
        (*persistentCoverage && !*gridStrideAbiVerified))
      return failure();

    std::optional<int64_t> &originalExtent = originalExtentByAxis[*axis];
    if (originalExtent && *originalExtent != *logicalExtent)
      return failure();
    originalExtent = *logicalExtent;

    persistentTransformCount += *persistentCoverage ? 1 : 0;
    if (persistentTransformCount > 1)
      return failure();

    parsed.transforms.push_back(ProgramGridTransform{
        static_cast<int32_t>(*order), static_cast<int32_t>(*axis), *factor,
        *logicalExtent, *persistentCoverage, *gridStrideAbiVerified});
  }
  return parsed;
}

DictionaryAttr mlir::triton::cfg::serializeProgramGridTransformContract(
    MLIRContext *context, const ProgramGridTransformContract &contract) {
  Builder builder(context);
  SmallVector<Attribute> transforms;
  transforms.reserve(contract.transforms.size());
  for (const ProgramGridTransform &transform : contract.transforms) {
    transforms.push_back(DictionaryAttr::get(
        context,
        {{builder.getStringAttr(kOrder), builder.getI32IntegerAttr(transform.order)},
         {builder.getStringAttr(kKind), builder.getStringAttr(kCeilDiv)},
         {builder.getStringAttr(kAxis), builder.getI32IntegerAttr(transform.axis)},
         {builder.getStringAttr(kFactor), builder.getI64IntegerAttr(transform.factor)},
         {builder.getStringAttr(kLogicalExtent),
          builder.getI64IntegerAttr(transform.logicalExtent)},
         {builder.getStringAttr(kPersistentCoverage),
          builder.getBoolAttr(transform.persistentCoverage)},
         {builder.getStringAttr(kGridStrideAbiVerified),
          builder.getBoolAttr(transform.gridStrideAbiVerified)}}));
  }
  return DictionaryAttr::get(
      context,
      {{builder.getStringAttr(kVersion), builder.getI64IntegerAttr(contract.version)},
       {builder.getStringAttr(kTransforms), ArrayAttr::get(context, transforms)}});
}

LogicalResult mlir::triton::cfg::setProgramGridTransformContract(
    ModuleOp module, const ProgramGridTransformContract &contract) {
  DictionaryAttr serialized =
      serializeProgramGridTransformContract(module.getContext(), contract);
  if (failed(parseProgramGridTransformContract(serialized)))
    return failure();
  module->setAttr(kProgramGridTransformsAttr, serialized);
  return success();
}
