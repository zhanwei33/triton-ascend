// RUN: triton-opt --split-input-file %s --verify-each -graph-optimize='rule-mask=512 ub-capacity-bytes=1048576 device-core-count=1 min-programs-per-core=1 ub-safety-percent=80 compile-mode=simd_simt_template' -o - | FileCheck %s
//
// This fixture uses the same direct two-PID / affine-pointer form as the
// unchanged-DSL kernels.  It intentionally keeps the tensors small so the
// rule's shape and launch-contract checks are independent of a device UB
// budget.

// CHECK: hacc.independent_axis_tensorize
// CHECK: hacc.program_grid_transforms
// CHECK: logical_extent = 65 : i64
// CHECK: persistent_coverage = false
// CHECK-LABEL: tt.func @_merge_split_states_kernel
// CHECK: tensor<2x{{(2|4|8)}}x4xf32>
// CHECK: "tt.reduce"({{.*}}) <{axis = 0 : i32}>
// CHECK: tensor<{{(2|4|8)}}x4xf32>
// CHECK: arith.cmpi slt
module attributes {hacc.grid_specialization = {grid_0 = 8 : i64, grid_1 = 65 : i64, grid_2 = 1 : i64, rule_mask = 512 : i64, version = 1 : i64}} {
  tt.func @_merge_split_states_kernel(%input: !tt.ptr<f32>, %output: !tt.ptr<f32>) {
    %c4 = arith.constant 4 : i32
    %head = tt.get_program_id y : i32
    %splits = tt.make_range {end = 2 : i32, start = 0 : i32} : tensor<2xi32>
    %dims = tt.make_range {end = 4 : i32, start = 0 : i32} : tensor<4xi32>
    %head_offset = arith.muli %head, %c4 : i32
    %input_base = tt.addptr %input, %head_offset : !tt.ptr<f32>, i32
    %input_splat = tt.splat %input_base : !tt.ptr<f32> -> tensor<2x1x!tt.ptr<f32>>
    %input_broadcast = tt.broadcast %input_splat : tensor<2x1x!tt.ptr<f32>> -> tensor<2x4x!tt.ptr<f32>>
    %dims_expand = tt.expand_dims %dims {axis = 0 : i32} : tensor<4xi32> -> tensor<1x4xi32>
    %dims_broadcast = tt.broadcast %dims_expand : tensor<1x4xi32> -> tensor<2x4xi32>
    %input_ptrs = tt.addptr %input_broadcast, %dims_broadcast : tensor<2x4x!tt.ptr<f32>>, tensor<2x4xi32>
    %states = tt.load %input_ptrs : tensor<2x4x!tt.ptr<f32>>
    %sum = "tt.reduce"(%states) <{axis = 0 : i32}> ({
    ^bb0(%lhs: f32, %rhs: f32):
      %value = arith.addf %lhs, %rhs : f32
      tt.reduce.return %value : f32
    }) : (tensor<2x4xf32>) -> tensor<4xf32>
    %output_base = tt.addptr %output, %head_offset : !tt.ptr<f32>, i32
    %output_splat = tt.splat %output_base : !tt.ptr<f32> -> tensor<4x!tt.ptr<f32>>
    %output_ptrs = tt.addptr %output_splat, %dims : tensor<4x!tt.ptr<f32>>, tensor<4xi32>
    tt.store %output_ptrs, %sum : tensor<4x!tt.ptr<f32>>
    tt.return
  }
}

// -----

// CHECK: hacc.independent_axis_tensorize
// CHECK: logical_extent = 16 : i64
// CHECK-LABEL: tt.func @_indexer_norm_rope_kernel
// CHECK: tensor<{{(2|4|8)}}x4xf32>
// CHECK: "tt.reduce"({{.*}}) <{axis = 1 : i32}>
// CHECK: tensor<{{(2|4|8)}}xf32>
// CHECK: arith.cmpi slt
module attributes {hacc.grid_specialization = {grid_0 = 8 : i64, grid_1 = 16 : i64, grid_2 = 1 : i64, rule_mask = 512 : i64, version = 1 : i64}} {
  tt.func @_indexer_norm_rope_kernel(%input: !tt.ptr<f32>, %output: !tt.ptr<f32>) {
    %c4 = arith.constant 4 : i32
    %head = tt.get_program_id y : i32
    %dims = tt.make_range {end = 4 : i32, start = 0 : i32} : tensor<4xi32>
    %head_offset = arith.muli %head, %c4 : i32
    %input_base = tt.addptr %input, %head_offset : !tt.ptr<f32>, i32
    %input_splat = tt.splat %input_base : !tt.ptr<f32> -> tensor<4x!tt.ptr<f32>>
    %input_ptrs = tt.addptr %input_splat, %dims : tensor<4x!tt.ptr<f32>>, tensor<4xi32>
    %values = tt.load %input_ptrs : tensor<4x!tt.ptr<f32>>
    %squares = arith.mulf %values, %values : tensor<4xf32>
    %sum = "tt.reduce"(%squares) <{axis = 0 : i32}> ({
    ^bb0(%lhs: f32, %rhs: f32):
      %value = arith.addf %lhs, %rhs : f32
      tt.reduce.return %value : f32
    }) : (tensor<4xf32>) -> f32
    %scale = tt.splat %sum : f32 -> tensor<4xf32>
    %result = arith.mulf %values, %scale : tensor<4xf32>
    %output_base = tt.addptr %output, %head_offset : !tt.ptr<f32>, i32
    %output_splat = tt.splat %output_base : !tt.ptr<f32> -> tensor<4x!tt.ptr<f32>>
    %output_ptrs = tt.addptr %output_splat, %dims : tensor<4x!tt.ptr<f32>>, tensor<4xi32>
    tt.store %output_ptrs, %result : tensor<4x!tt.ptr<f32>>
    tt.return
  }
}

// -----

// A K-shaped head extent of one is a deliberate no-op even when all other
// conditions match the Q form.
// CHECK-NOT: hacc.independent_axis_tensorize
// CHECK-LABEL: module attributes {hacc.grid_specialization = {grid_0 = 8 : i64, grid_1 = 1 : i64
// CHECK-LABEL: tt.func @_indexer_norm_rope_kernel
// CHECK: tt.get_program_id y
// CHECK: "tt.reduce"({{.*}}) <{axis = 0 : i32}>
module attributes {hacc.grid_specialization = {grid_0 = 8 : i64, grid_1 = 1 : i64, grid_2 = 1 : i64, rule_mask = 512 : i64, version = 1 : i64}} {
  tt.func @_indexer_norm_rope_kernel(%input: !tt.ptr<f32>, %output: !tt.ptr<f32>) {
    %c4 = arith.constant 4 : i32
    %head = tt.get_program_id y : i32
    %dims = tt.make_range {end = 4 : i32, start = 0 : i32} : tensor<4xi32>
    %head_offset = arith.muli %head, %c4 : i32
    %input_base = tt.addptr %input, %head_offset : !tt.ptr<f32>, i32
    %input_splat = tt.splat %input_base : !tt.ptr<f32> -> tensor<4x!tt.ptr<f32>>
    %input_ptrs = tt.addptr %input_splat, %dims : tensor<4x!tt.ptr<f32>>, tensor<4xi32>
    %values = tt.load %input_ptrs : tensor<4x!tt.ptr<f32>>
    %sum = "tt.reduce"(%values) <{axis = 0 : i32}> ({
    ^bb0(%lhs: f32, %rhs: f32):
      %value = arith.addf %lhs, %rhs : f32
      tt.reduce.return %value : f32
    }) : (tensor<4xf32>) -> f32
    %scale = tt.splat %sum : f32 -> tensor<4xf32>
    %result = arith.mulf %values, %scale : tensor<4xf32>
    %output_base = tt.addptr %output, %head_offset : !tt.ptr<f32>, i32
    %output_splat = tt.splat %output_base : !tt.ptr<f32> -> tensor<4x!tt.ptr<f32>>
    %output_ptrs = tt.addptr %output_splat, %dims : tensor<4x!tt.ptr<f32>>, tensor<4xi32>
    tt.store %output_ptrs, %result : tensor<4x!tt.ptr<f32>>
    tt.return
  }
}
