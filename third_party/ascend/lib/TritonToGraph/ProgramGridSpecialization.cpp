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

#include "TritonToGraph/ProgramGridSpecialization.h"

#include "mlir/IR/Builders.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"

#include <array>
#include <limits>
#include <optional>

using namespace mlir;
using namespace mlir::triton::cfg;

namespace {

constexpr llvm::StringLiteral kVersion = "version";
constexpr llvm::StringLiteral kGrid0 = "grid_0";
constexpr llvm::StringLiteral kGrid1 = "grid_1";
constexpr llvm::StringLiteral kGrid2 = "grid_2";
constexpr llvm::StringLiteral kRuleMask = "rule_mask";
constexpr uint32_t kProgramMappingRuleMask = (1U << 9) | (1U << 10) | (1U << 11);

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

void setAttrOnFunctions(ModuleOp module, llvm::StringRef name,
                        Attribute attribute) {
  module.walk([&](Operation *operation) {
    if (operation->getName().getStringRef() == "tt.func")
      operation->setAttr(name, attribute);
  });
}

void removeAttrFromFunctions(ModuleOp module, llvm::StringRef name) {
  module.walk([&](Operation *operation) {
    if (operation->getName().getStringRef() == "tt.func")
      operation->removeAttr(name);
  });
}

} // namespace

FailureOr<ProgramGridSpecialization>
mlir::triton::cfg::parseProgramGridSpecialization(Attribute attribute) {
  auto dictionary = dyn_cast_or_null<DictionaryAttr>(attribute);
  if (!dictionary || !hasExactKeys(dictionary, {kVersion, kGrid0, kGrid1,
                                                 kGrid2, kRuleMask}))
    return failure();

  std::optional<int64_t> version = getInteger(dictionary, kVersion);
  std::optional<int64_t> grid0 = getInteger(dictionary, kGrid0);
  std::optional<int64_t> grid1 = getInteger(dictionary, kGrid1);
  std::optional<int64_t> grid2 = getInteger(dictionary, kGrid2);
  std::optional<int64_t> ruleMask = getInteger(dictionary, kRuleMask);
  if (!version || !grid0 || !grid1 || !grid2 || !ruleMask ||
      *version != kProgramGridSpecializationVersion || *grid0 < 1 ||
      *grid1 < 1 || *grid2 < 1 || *ruleMask <= 0 ||
      *ruleMask > std::numeric_limits<uint32_t>::max())
    return failure();

  const uint32_t narrowedRuleMask = static_cast<uint32_t>(*ruleMask);
  if ((narrowedRuleMask & ~kProgramMappingRuleMask) != 0)
    return failure();

  return ProgramGridSpecialization{*version,
                                   std::array<int64_t, 3>{*grid0, *grid1,
                                                          *grid2},
                                   narrowedRuleMask};
}

DictionaryAttr mlir::triton::cfg::serializeProgramGridSpecialization(
    MLIRContext *context, const ProgramGridSpecialization &specialization) {
  Builder builder(context);
  return DictionaryAttr::get(
      context,
      {{builder.getStringAttr(kVersion),
        builder.getI64IntegerAttr(specialization.version)},
       {builder.getStringAttr(kGrid0),
        builder.getI64IntegerAttr(specialization.grid[0])},
       {builder.getStringAttr(kGrid1),
        builder.getI64IntegerAttr(specialization.grid[1])},
       {builder.getStringAttr(kGrid2),
        builder.getI64IntegerAttr(specialization.grid[2])},
       {builder.getStringAttr(kRuleMask),
        builder.getI64IntegerAttr(specialization.ruleMask)}});
}

LogicalResult mlir::triton::cfg::setProgramGridSpecialization(
    ModuleOp module, const ProgramGridSpecialization &specialization) {
  DictionaryAttr serialized =
      serializeProgramGridSpecialization(module.getContext(), specialization);
  if (failed(parseProgramGridSpecialization(serialized)))
    return failure();
  module->setAttr(kProgramGridSpecializationAttr, serialized);
  setAttrOnFunctions(module, kProgramGridSpecializationAttr, serialized);
  return success();
}

void mlir::triton::cfg::clearProgramGridSpecialization(ModuleOp module) {
  module->removeAttr(kProgramGridSpecializationAttr);
  removeAttrFromFunctions(module, kProgramGridSpecializationAttr);
}
