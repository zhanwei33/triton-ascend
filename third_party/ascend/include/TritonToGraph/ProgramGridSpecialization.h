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

#ifndef TRITON_TO_GRAPH_PROGRAM_GRID_SPECIALIZATION_H
#define TRITON_TO_GRAPH_PROGRAM_GRID_SPECIALIZATION_H

#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Support/LLVM.h"
#include "llvm/ADT/StringRef.h"

#include <array>
#include <cstdint>

namespace mlir {
namespace triton {
namespace cfg {

// The full pre-transform grid is a static compiler input.  It is published on
// both the module and every tt.func so GraphOptimize rules can consume it
// without recovering extents from transformed tt.get_num_programs values.
inline constexpr llvm::StringLiteral kProgramGridSpecializationAttr =
    "hacc.grid_specialization";
inline constexpr int64_t kProgramGridSpecializationVersion = 1;

struct ProgramGridSpecialization {
  int64_t version = kProgramGridSpecializationVersion;
  std::array<int64_t, 3> grid = {1, 1, 1};
  uint32_t ruleMask = 0;
};

// Parse the fixed v1 generic attribute.  The bridge intentionally knows only
// the three stage-00 mapping bits (IAT/SPAF/PTSM), so unknown future bits fail
// rather than changing JIT cache timing accidentally.
FailureOr<ProgramGridSpecialization>
parseProgramGridSpecialization(Attribute attribute);

DictionaryAttr serializeProgramGridSpecialization(
    MLIRContext *context, const ProgramGridSpecialization &specialization);

// Set/clear module and function copies together.  Keeping this operation in
// C++ prevents a Python caller from accidentally leaving a hacc.* attr on a
// nested tt.func when the downstream vendor compiler is invoked.
LogicalResult setProgramGridSpecialization(
    ModuleOp module, const ProgramGridSpecialization &specialization);
void clearProgramGridSpecialization(ModuleOp module);

} // namespace cfg
} // namespace triton
} // namespace mlir

#endif // TRITON_TO_GRAPH_PROGRAM_GRID_SPECIALIZATION_H
