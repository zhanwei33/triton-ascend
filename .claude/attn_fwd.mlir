module attributes {dlti.target_system_spec = #dlti.target_system_spec<"NPU" : #hacc.target_device_spec<#dlti.dl_entry<"AI_CORE_COUNT", 28 : i32>, #dlti.dl_entry<"CUBE_CORE_COUNT", 28 : i32>, #dlti.dl_entry<"VECTOR_CORE_COUNT", 56 : i32>, #dlti.dl_entry<"UB_SIZE", 2031616 : i32>, #dlti.dl_entry<"L1_SIZE", 4194304 : i32>, #dlti.dl_entry<"L0A_SIZE", 524288 : i32>, #dlti.dl_entry<"L0B_SIZE", 524288 : i32>, #dlti.dl_entry<"L0C_SIZE", 2097152 : i32>, #dlti.dl_entry<"UB_ALIGN_SIZE", 256 : i32>, #dlti.dl_entry<"L1_ALIGN_SIZE", 256 : i32>, #dlti.dl_entry<"L0C_ALIGN_SIZE", 4096 : i32>, #dlti.dl_entry<"MINIMAL_D_CACHE_SIZE", 262144 : i32>, #dlti.dl_entry<"MAXIMUM_D_CACHE_SIZE", 983040 : i32>, #dlti.dl_entry<"ARCH", "dav-c310">>>, hacc.target = #hacc.target<"Ascend950PR_9579">, hivm.module_core_type = #hivm.module_core_type<MIX>} {
  func.func @_attn_fwd_mix_aiv_outlined_merged_vf_0(%arg0: memref<128xf32, #hivm.address_space<ub>>, %arg1: memref<128xf32, #hivm.address_space<ub>>, %arg2: memref<128xf32, #hivm.address_space<ub>>, %arg3: memref<128xf32, #hivm.address_space<ub>>, %arg4: memref<128x128xf32, #hivm.address_space<ub>>, %arg5: memref<128x128xf16, #hivm.address_space<ub>>) attributes {hivm.func_core_type = #hivm.func_core_type<AIV>, hivm.vector_function} {
    %c1 = arith.constant 1 : index
    %cst = arith.constant dense<0.693147182> : vector<64xf32>
    %cst_0 = arith.constant 0.000000e+00 : f32
    %c64 = arith.constant 64 : index
    %c128 = arith.constant 128 : index
    %c0 = arith.constant 0 : index
    scf.for %arg6 = %c0 to %c128 step %c64 {
      %subview = memref.subview %arg0[%arg6] [64] [1] : memref<128xf32, #hivm.address_space<ub>> to memref<64xf32, strided<[1], offset: ?>, #hivm.address_space<ub>>
      %subview_1 = memref.subview %arg1[%arg6] [64] [1] : memref<128xf32, #hivm.address_space<ub>> to memref<64xf32, strided<[1], offset: ?>, #hivm.address_space<ub>>
      %subview_2 = memref.subview %arg2[%arg6] [64] [1] : memref<128xf32, #hivm.address_space<ub>> to memref<64xf32, strided<[1], offset: ?>, #hivm.address_space<ub>>
      %0 = vector.transfer_read %subview[%c0], %cst_0 {in_bounds = [true]} : memref<64xf32, strided<[1], offset: ?>, #hivm.address_space<ub>>, vector<64xf32>
      %1 = vector.transfer_read %subview_1[%c0], %cst_0 {in_bounds = [true]} : memref<64xf32, strided<[1], offset: ?>, #hivm.address_space<ub>>, vector<64xf32>
      %2 = math.log %1 : vector<64xf32>
      %3 = arith.divf %2, %cst : vector<64xf32>
      %4 = arith.addf %0, %3 : vector<64xf32>
      vector.transfer_write %4, %subview_2[%c0] {in_bounds = [true]} : vector<64xf32>, memref<64xf32, strided<[1], offset: ?>, #hivm.address_space<ub>>
    }
    scf.for %arg6 = %c0 to %c128 step %c1 {
      %subview = memref.subview %arg3[%arg6] [1] [1] : memref<128xf32, #hivm.address_space<ub>> to memref<1xf32, strided<[1], offset: ?>, #hivm.address_space<ub>>
      scf.for %arg7 = %c0 to %c128 step %c64 {
        %subview_1 = memref.subview %arg4[%arg6, %arg7] [1, 64] [1, 1] : memref<128x128xf32, #hivm.address_space<ub>> to memref<1x64xf32, strided<[128, 1], offset: ?>, #hivm.address_space<ub>>
        %subview_2 = memref.subview %arg5[%arg6, %arg7] [1, 64] [1, 1] : memref<128x128xf16, #hivm.address_space<ub>> to memref<1x64xf16, strided<[128, 1], offset: ?>, #hivm.address_space<ub>>
        %subview_3 = memref.subview %subview_1[0, 0] [1, 64] [1, 1] : memref<1x64xf32, strided<[128, 1], offset: ?>, #hivm.address_space<ub>> to memref<64xf32, #map, #hivm.address_space<ub>>
        %0 = vector.transfer_read %subview_3[%c0], %cst_0 {in_bounds = [true]} : memref<64xf32, #map, #hivm.address_space<ub>>, vector<64xf32>
        %1 = vector.transfer_read %subview[%c0], %cst_0 {in_bounds = [true, true], permutation_map = #map1} : memref<1xf32, strided<[1], offset: ?>, #hivm.address_space<ub>>, vector<1x64xf32>
        %2 = vector.shape_cast %1 : vector<1x64xf32> to vector<64xf32>
        %3 = arith.divf %0, %2 : vector<64xf32>
        %4 = arith.truncf %3 {enable_saturate = false, round_mode = #hfusion.round_mode<rint>, unsigned_mode = #hfusion.unsigned_mode<si2si>} : vector<64xf32> to vector<64xf16>
        %subview_4 = memref.subview %subview_2[0, 0] [1, 64] [1, 1] : memref<1x64xf16, strided<[128, 1], offset: ?>, #hivm.address_space<ub>> to memref<64xf16, #map, #hivm.address_space<ub>>
        vector.transfer_write %4, %subview_4[%c0] {in_bounds = [true]} : vector<64xf16>, memref<64xf16, #map, #hivm.address_space<ub>>
      }
    }
    return
  }
  func.func @_attn_fwd_mix_aic(%arg0: memref<?xi8, #hivm.address_space<gm>> {hacc.arg_type = #hacc.arg_type<sync_block_lock>}, %arg1: memref<?xi8, #hivm.address_space<gm>> {hacc.arg_type = #hacc.arg_type<workspace>}, %arg2: memref<?xf16, #hivm.address_space<gm>> {tt.divisibility = 16 : i32, tt.tensor_kind = 0 : i32}, %arg3: memref<?xf16, #hivm.address_space<gm>> {tt.divisibility = 16 : i32, tt.tensor_kind = 0 : i32}, %arg4: memref<?xf16, #hivm.address_space<gm>> {tt.divisibility = 16 : i32, tt.tensor_kind = 0 : i32}, %arg5: f32, %arg6: memref<?xf32, #hivm.address_space<gm>> {tt.divisibility = 16 : i32, tt.tensor_kind = 1 : i32}, %arg7: memref<?xf16, #hivm.address_space<gm>> {tt.divisibility = 16 : i32, tt.tensor_kind = 1 : i32}, %arg8: i32 {tt.divisibility = 16 : i32}, %arg9: i32 {tt.divisibility = 16 : i32}, %arg10: i32 {tt.divisibility = 16 : i32}, %arg11: i32 {tt.divisibility = 16 : i32}, %arg12: i32 {tt.divisibility = 16 : i32}, %arg13: i32 {tt.divisibility = 16 : i32}, %arg14: i32 {tt.divisibility = 16 : i32}, %arg15: i32 {tt.divisibility = 16 : i32}, %arg16: i32 {tt.divisibility = 16 : i32}, %arg17: i32 {tt.divisibility = 16 : i32}, %arg18: i32 {tt.divisibility = 16 : i32}, %arg19: i32 {tt.divisibility = 16 : i32}, %arg20: i32, %arg21: i32, %arg22: i32, %arg23: i32 {tt.divisibility = 16 : i32}, %arg24: i32 {tt.divisibility = 16 : i32}, %arg25: i32, %arg26: i32, %arg27: i32) attributes {SyncBlockLockArgIdx = 0 : i64, WorkspaceArgIdx = 1 : i64, func_dyn_memref_args = dense<[true, true, true, true, true, false, true, true, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false]> : vector<28xi1>, hacc.entry, hacc.function_kind = #hacc.function_kind<DEVICE>, hivm.func_core_type = #hivm.func_core_type<AIC>, hivm.part_of_mix, hivm.vf_mode = #hivm.vf_mode<SIMD>, mix_mode = "mix", parallel_mode = "simd"} {
    %c-1_i64 = arith.constant -1 : i64
    %c0_i64 = arith.constant 0 : i64
    %c183296_i64 = arith.constant 183296 : i64
    %c16384_i64 = arith.constant 16384 : i64
    %c49152_i64 = arith.constant 49152 : i64
    %c40960_i64 = arith.constant 40960 : i64
    %c32768_i64 = arith.constant 32768 : i64
    %c0 = arith.constant 0 : index
    %true = arith.constant true
    %cst = arith.constant 0.000000e+00 : f16
    %c128 = arith.constant 128 : index
    %c32_i32 = arith.constant 32 : i32
    %c0_i32 = arith.constant 0 : i32
    %c128_i32 = arith.constant 128 : i32
    %c32 = arith.constant 32 : index
    hivm.hir.set_ctrl false at ctrl[60]
    hivm.hir.set_ctrl true at ctrl[48]
    %0 = arith.muli %arg25, %arg26 : i32
    %1 = arith.muli %0, %arg27 : i32
    annotation.mark %1 {logical_block_num} : i32
    %2 = hivm.hir.get_block_idx -> i64
    %3 = arith.trunci %2 : i64 to i32
    %4 = arith.divsi %3, %arg27 : i32
    %5 = arith.remsi %4, %arg26 : i32
    %6 = arith.muli %arg27, %arg26 : i32
    %7 = arith.divsi %3, %6 : i32
    %8 = arith.remsi %7, %arg25 : i32
    %9 = arith.divsi %5, %arg21 : i32
    %10 = arith.remsi %5, %arg21 : i32
    %11 = arith.extsi %9 : i32 to i64
    %12 = arith.extsi %arg8 : i32 to i64
    %13 = arith.muli %11, %12 : i64
    %14 = arith.extsi %10 : i32 to i64
    %15 = arith.extsi %arg9 : i32 to i64
    %16 = arith.muli %14, %15 : i64
    %17 = arith.addi %13, %16 : i64
    %18 = arith.extsi %arg11 : i32 to i64
    %19 = arith.muli %11, %18 : i64
    %20 = arith.extsi %arg12 : i32 to i64
    %21 = arith.muli %14, %20 : i64
    %22 = arith.addi %19, %21 : i64
    %23 = arith.muli %8, %c128_i32 : i32
    %24 = arith.index_cast %17 : i64 to index
    %25 = arith.index_cast %23 : i32 to index
    %26 = arith.index_cast %arg10 : i32 to index
    %27 = affine.apply #map2()[%24, %25, %26]
    %reinterpret_cast = memref.reinterpret_cast %arg2 to offset: [%27], sizes: [128, 128], strides: [%26, 1] : memref<?xf16, #hivm.address_space<gm>> to memref<128x128xf16, strided<[?, 1], offset: ?>, #hivm.address_space<gm>>
    %28 = hivm.hir.pointer_cast(%c0_i64) : memref<8x8x16x16xf16, #hivm.address_space<cbuf>>
    %cast = memref.cast %28 : memref<8x8x16x16xf16, #hivm.address_space<cbuf>> to memref<?x?x?x?xf16, #hivm.address_space<cbuf>>
    %29 = affine.apply #map3()[%25]
    %30 = arith.index_cast %arg23 : i32 to index
    %31 = arith.maxsi %25, %30 : index
    %32 = arith.minsi %29, %31 : index
    %33 = affine.apply #map4()[%32, %25]
    %34 = arith.cmpi slt, %33, %c128 : index
    %subview = memref.subview %reinterpret_cast[0, 0] [%33, 128] [1, 1] : memref<128x128xf16, strided<[?, 1], offset: ?>, #hivm.address_space<gm>> to memref<?x128xf16, strided<[?, 1], offset: ?>, #hivm.address_space<gm>>
    %35 = affine.apply #map5()[%32, %25]
    %subview_0 = memref.subview %28[0, 0, 0, 0] [8, %35, 16, 16] [1, 1, 1, 1] : memref<8x8x16x16xf16, #hivm.address_space<cbuf>> to memref<8x?x16x16xf16, strided<[2048, 256, 16, 1]>, #hivm.address_space<cbuf>>
    %cast_1 = memref.cast %subview_0 : memref<8x?x16x16xf16, strided<[2048, 256, 16, 1]>, #hivm.address_space<cbuf>> to memref<?x?x?x?xf16, strided<[?, ?, ?, 1], offset: ?>, #hivm.address_space<cbuf>>
    scf.if %34 {
      %collapse_shape = memref.collapse_shape %28 [[0, 1, 2, 3]] : memref<8x8x16x16xf16, #hivm.address_space<cbuf>> into memref<16384xf16, #hivm.address_space<cbuf>>
      hivm.hir.vbrc ins(%cst : f16) outs(%collapse_shape : memref<16384xf16, #hivm.address_space<cbuf>>)
    }
    hivm.hir.pipe_barrier[<PIPE_MTE2>]
    hivm.hir.nd2nz {dst_continuous} ins(%subview : memref<?x128xf16, strided<[?, 1], offset: ?>, #hivm.address_space<gm>>) outs(%cast_1 : memref<?x?x?x?xf16, strided<[?, ?, ?, 1], offset: ?>, #hivm.address_space<cbuf>>)
    hivm.hir.set_flag[<PIPE_MTE2>, <PIPE_MTE1>, <EVENT_ID0>]
    %36 = arith.muli %arg16, %c32_i32 : i32
    %37 = arith.index_cast %22 : i64 to index
    %38 = arith.index_cast %arg16 : i32 to index
    hivm.hir.sync_block_set[<CUBE>, <PIPE_MTE1>, <PIPE_MTE3>] flag = 2
    hivm.hir.sync_block_set[<CUBE>, <PIPE_MTE1>, <PIPE_MTE3>] flag = 4
    %39 = arith.index_cast %arg24 : i32 to index
    %40 = arith.index_cast %36 : i32 to index
    hivm.hir.wait_flag[<PIPE_MTE2>, <PIPE_MTE1>, <EVENT_ID0>]
    hivm.hir.set_flag[<PIPE_MTE1>, <PIPE_MTE2>, <EVENT_ID0>]
    hivm.hir.set_flag[<PIPE_FIX>, <PIPE_M>, <EVENT_ID0>]
    hivm.hir.set_flag[<PIPE_FIX>, <PIPE_M>, <EVENT_ID1>]
    hivm.hir.set_flag[<PIPE_M>, <PIPE_MTE1>, <EVENT_ID0>]
    hivm.hir.set_flag[<PIPE_M>, <PIPE_MTE1>, <EVENT_ID1>]
    %41 = scf.for %arg28 = %c0_i32 to %arg24 step %c32_i32 iter_args(%arg29 = %37) -> (index)  : i32 {
      %reinterpret_cast_2 = memref.reinterpret_cast %arg4 to offset: [%arg29], sizes: [32, 128], strides: [%38, 1] : memref<?xf16, #hivm.address_space<gm>> to memref<32x128xf16, strided<[?, 1], offset: ?>, #hivm.address_space<gm>>
      %42 = hivm.hir.pointer_cast(%c0_i64) : memref<128x32xf16, #hivm.address_space<ub>>
      annotation.mark %42 {effects = ["write", "read"], hivm.tightly_coupled_buffer = #hivm.tightly_coupled_buffer<0>} : memref<128x32xf16, #hivm.address_space<ub>>
      %43 = arith.index_cast %arg28 : i32 to index
      %44 = affine.apply #map6()[%43]
      %45 = arith.maxsi %43, %39 : index
      %46 = arith.minsi %44, %45 : index
      %47 = affine.apply #map4()[%46, %43]
      %48 = arith.cmpi slt, %47, %c32 : index
      annotation.mark %42 {MayImplicitTransposeWithLastAxis} : memref<128x32xf16, #hivm.address_space<ub>>
      annotation.mark %42 {MayImplicitTransposeWithLastAxis} : memref<128x32xf16, #hivm.address_space<ub>>
      %49 = hivm.hir.pointer_cast(%c32768_i64) : memref<2x8x16x16xf16, #hivm.address_space<cbuf>>
      annotation.mark %49 {effects = ["write", "read"], hivm.tightly_coupled_buffer = #hivm.tightly_coupled_buffer<1>} : memref<2x8x16x16xf16, #hivm.address_space<cbuf>>
      hivm.hir.sync_block_wait[<CUBE>, <PIPE_MTE3>, <PIPE_MTE1>] flag = 1
      %50 = hivm.hir.pointer_cast(%c0_i64) : memref<2x8x16x16xf32, #hivm.address_space<cc>>
      %cast_3 = memref.cast %50 : memref<2x8x16x16xf32, #hivm.address_space<cc>> to memref<?x?x?x?xf32, #hivm.address_space<cc>>
      hivm.hir.wait_flag[<PIPE_FIX>, <PIPE_M>, <EVENT_ID0>]
      hivm.hir.mmadL1 {already_set_real_mkn, fixpipe_already_inserted = true} ins(%cast, %49, %true, %c128, %c128, %c32 : memref<?x?x?x?xf16, #hivm.address_space<cbuf>>, memref<2x8x16x16xf16, #hivm.address_space<cbuf>>, i1, index, index, index) outs(%cast_3 : memref<?x?x?x?xf32, #hivm.address_space<cc>>)
      hivm.hir.set_flag[<PIPE_M>, <PIPE_FIX>, <EVENT_ID0>]
      hivm.hir.sync_block_set[<CUBE>, <PIPE_MTE1>, <PIPE_MTE3>] flag = 2
      %51 = affine.apply #map5()[%46, %43]
      %subview_4 = memref.subview %50[0, 0, 0, 0] [%51, 8, 16, 16] [1, 1, 1, 1] : memref<2x8x16x16xf32, #hivm.address_space<cc>> to memref<?x8x16x16xf32, strided<[2048, 256, 16, 1]>, #hivm.address_space<cc>>
      %cast_5 = memref.cast %subview_4 : memref<?x8x16x16xf32, strided<[2048, 256, 16, 1]>, #hivm.address_space<cc>> to memref<?x?x?x?xf32, strided<[?, ?, ?, 1], offset: ?>, #hivm.address_space<cc>>
      %52 = affine.apply #map7()[%46, %43]
      %53 = hivm.hir.pointer_cast(%c0_i64) : memref<16384xi8, #hivm.address_space<ub>>
      %view = memref.view %53[%c0][%52] : memref<16384xi8, #hivm.address_space<ub>> to memref<128x?x1xf32, #hivm.address_space<ub>>
      %subview_6 = memref.subview %view[0, 0, 0] [128, %47, 1] [1, 1, 1] : memref<128x?x1xf32, #hivm.address_space<ub>> to memref<128x?xf32, strided<[?, 1]>, #hivm.address_space<ub>>
      annotation.mark %subview_6 {effects = ["write", "read"], hivm.tightly_coupled_buffer = #hivm.tightly_coupled_buffer<2>} : memref<128x?xf32, strided<[?, 1]>, #hivm.address_space<ub>>
      hivm.hir.sync_block_wait[<CUBE>, <PIPE_V>, <PIPE_FIX>] flag = 3
      hivm.hir.wait_flag[<PIPE_M>, <PIPE_FIX>, <EVENT_ID0>]
      hivm.hir.fixpipe {dma_mode = #hivm.dma_mode<nz2nd>} ins(%cast_5 : memref<?x?x?x?xf32, strided<[?, ?, ?, 1], offset: ?>, #hivm.address_space<cc>>) outs(%subview_6 : memref<128x?xf32, strided<[?, 1]>, #hivm.address_space<ub>>)
      hivm.hir.set_flag[<PIPE_FIX>, <PIPE_M>, <EVENT_ID0>]
      hivm.hir.sync_block_set[<CUBE>, <PIPE_FIX>, <PIPE_V>] flag = 0
      %54 = hivm.hir.pointer_cast(%c40960_i64) : memref<8x2x16x16xf16, #hivm.address_space<cbuf>>
      %cast_7 = memref.cast %54 : memref<8x2x16x16xf16, #hivm.address_space<cbuf>> to memref<?x?x?x?xf16, #hivm.address_space<cbuf>>
      %subview_8 = memref.subview %reinterpret_cast_2[0, 0] [%47, 128] [1, 1] : memref<32x128xf16, strided<[?, 1], offset: ?>, #hivm.address_space<gm>> to memref<?x128xf16, strided<[?, 1], offset: ?>, #hivm.address_space<gm>>
      %subview_9 = memref.subview %54[0, 0, 0, 0] [8, %51, 16, 16] [1, 1, 1, 1] : memref<8x2x16x16xf16, #hivm.address_space<cbuf>> to memref<8x?x16x16xf16, strided<[512, 256, 16, 1]>, #hivm.address_space<cbuf>>
      %cast_10 = memref.cast %subview_9 : memref<8x?x16x16xf16, strided<[512, 256, 16, 1]>, #hivm.address_space<cbuf>> to memref<?x?x?x?xf16, strided<[?, ?, ?, 1], offset: ?>, #hivm.address_space<cbuf>>
      hivm.hir.wait_flag[<PIPE_MTE1>, <PIPE_MTE2>, <EVENT_ID0>]
      scf.if %48 {
        %collapse_shape = memref.collapse_shape %54 [[0, 1, 2, 3]] : memref<8x2x16x16xf16, #hivm.address_space<cbuf>> into memref<4096xf16, #hivm.address_space<cbuf>>
        hivm.hir.vbrc ins(%cst : f16) outs(%collapse_shape : memref<4096xf16, #hivm.address_space<cbuf>>)
      }
      hivm.hir.pipe_barrier[<PIPE_MTE2>]
      hivm.hir.nd2nz {dst_continuous} ins(%subview_8 : memref<?x128xf16, strided<[?, 1], offset: ?>, #hivm.address_space<gm>>) outs(%cast_10 : memref<?x?x?x?xf16, strided<[?, ?, ?, 1], offset: ?>, #hivm.address_space<cbuf>>)
      hivm.hir.set_flag[<PIPE_MTE2>, <PIPE_MTE1>, <EVENT_ID0>]
      %55 = hivm.hir.pointer_cast(%c49152_i64) : memref<2x8x16x16xf16, #hivm.address_space<cbuf>>
      annotation.mark %55 {effects = ["write", "read"], hivm.tightly_coupled_buffer = #hivm.tightly_coupled_buffer<3>} : memref<2x8x16x16xf16, #hivm.address_space<cbuf>>
      hivm.hir.sync_block_wait[<CUBE>, <PIPE_MTE3>, <PIPE_MTE1>] flag = 1
      %56 = hivm.hir.pointer_cast(%c16384_i64) : memref<8x8x16x16xf32, #hivm.address_space<cc>>
      %cast_11 = memref.cast %56 : memref<8x8x16x16xf32, #hivm.address_space<cc>> to memref<?x?x?x?xf32, #hivm.address_space<cc>>
      hivm.hir.wait_flag[<PIPE_FIX>, <PIPE_M>, <EVENT_ID1>]
      hivm.hir.mmadL1 {already_set_real_mkn, fixpipe_already_inserted = true} ins(%55, %cast_7, %true, %c128, %c32, %c128 : memref<2x8x16x16xf16, #hivm.address_space<cbuf>>, memref<?x?x?x?xf16, #hivm.address_space<cbuf>>, i1, index, index, index) outs(%cast_11 : memref<?x?x?x?xf32, #hivm.address_space<cc>>) sync_related_args(%c-1_i64, %c0_i64, %c-1_i64, %c0_i64, %c-1_i64, %c-1_i64, %c-1_i64 : i64, i64, i64, i64, i64, i64, i64)
      hivm.hir.set_flag[<PIPE_M>, <PIPE_FIX>, <EVENT_ID0>]
      hivm.hir.sync_block_set[<CUBE>, <PIPE_MTE1>, <PIPE_MTE3>] flag = 4
      %57 = hivm.hir.pointer_cast(%c183296_i64) : memref<128x128xf32, #hivm.address_space<ub>>
      annotation.mark %57 {effects = ["write", "read"], hivm.tightly_coupled_buffer = #hivm.tightly_coupled_buffer<4>} : memref<128x128xf32, #hivm.address_space<ub>>
      hivm.hir.sync_block_wait[<CUBE>, <PIPE_V>, <PIPE_FIX>] flag = 5
      hivm.hir.wait_flag[<PIPE_M>, <PIPE_FIX>, <EVENT_ID0>]
      hivm.hir.fixpipe {dma_mode = #hivm.dma_mode<nz2nd>} ins(%cast_11 : memref<?x?x?x?xf32, #hivm.address_space<cc>>) outs(%57 : memref<128x128xf32, #hivm.address_space<ub>>)
      hivm.hir.set_flag[<PIPE_FIX>, <PIPE_M>, <EVENT_ID1>]
      hivm.hir.sync_block_set[<CUBE>, <PIPE_FIX>, <PIPE_V>] flag = 0
      %58 = affine.apply #map8()[%arg29, %40]
      scf.yield %58 : index
    }
    hivm.hir.wait_flag[<PIPE_M>, <PIPE_MTE1>, <EVENT_ID0>]
    hivm.hir.wait_flag[<PIPE_M>, <PIPE_MTE1>, <EVENT_ID1>]
    hivm.hir.wait_flag[<PIPE_FIX>, <PIPE_M>, <EVENT_ID0>]
    hivm.hir.wait_flag[<PIPE_FIX>, <PIPE_M>, <EVENT_ID1>]
    hivm.hir.wait_flag[<PIPE_MTE1>, <PIPE_MTE2>, <EVENT_ID0>]
    hivm.hir.sync_block_wait[<CUBE>, <PIPE_V>, <PIPE_FIX>] flag = 3
    hivm.hir.sync_block_wait[<CUBE>, <PIPE_V>, <PIPE_FIX>] flag = 5
    hivm.hir.pipe_barrier[<PIPE_ALL>]
    return
  }
  func.func @_attn_fwd_mix_aiv_outlined_vf_0(%arg0: memref<128x32xf16, #hivm.address_space<ub>>) attributes {hivm.func_core_type = #hivm.func_core_type<AIV>, hivm.vector_function, no_inline} {
    %cst = arith.constant dense<0.000000e+00> : vector<128xf16>
    %c1 = arith.constant 1 : index
    %c128 = arith.constant 128 : index
    %c0 = arith.constant 0 : index
    scf.for %arg1 = %c0 to %c128 step %c1 {
      %subview = memref.subview %arg0[%arg1, 0] [1, 32] [1, 1] : memref<128x32xf16, #hivm.address_space<ub>> to memref<1x32xf16, strided<[32, 1], offset: ?>, #hivm.address_space<ub>>
      %0 = vector.constant_mask [32] : vector<128xi1>
      annotation.mark %0 {mask_op_idx = 0 : i32} : vector<128xi1>
      %subview_0 = memref.subview %subview[0, 0] [1, 32] [1, 1] : memref<1x32xf16, strided<[32, 1], offset: ?>, #hivm.address_space<ub>> to memref<32xf16, #map, #hivm.address_space<ub>>
      vector.transfer_write %cst, %subview_0[%c0], %0 {in_bounds = [true]} : vector<128xf16>, memref<32xf16, #map, #hivm.address_space<ub>>
    }
    return
  }
  func.func @_attn_fwd_mix_aiv_outlined_vf_1(%arg0: memref<128x32xf16, #hivm.address_space<ub>>, %arg1: memref<2x128x16xf16, strided<[2064, 16, 1]>, #hivm.address_space<ub>>) attributes {hivm.func_core_type = #hivm.func_core_type<AIV>, hivm.vector_function, no_inline} {
    %cst = arith.constant 0.000000e+00 : f16
    %c1 = arith.constant 1 : index
    %c128 = arith.constant 128 : index
    %c0 = arith.constant 0 : index
    scf.for %arg2 = %c0 to %c128 step %c1 {
      %subview = memref.subview %arg1[0, %arg2, 0] [2, 1, 16] [1, 1, 1] : memref<2x128x16xf16, strided<[2064, 16, 1]>, #hivm.address_space<ub>> to memref<2x1x16xf16, strided<[2064, 16, 1], offset: ?>, #hivm.address_space<ub>>
      %subview_0 = memref.subview %arg0[%arg2, 0] [1, 32] [1, 1] : memref<128x32xf16, #hivm.address_space<ub>> to memref<1x32xf16, strided<[32, 1], offset: ?>, #hivm.address_space<ub>>
      %0 = vector.transfer_read %subview_0[%c0, %c0], %cst {in_bounds = [true, false]} : memref<1x32xf16, strided<[32, 1], offset: ?>, #hivm.address_space<ub>>, vector<1x128xf16>
      %1 = vector.shape_cast %0 : vector<1x128xf16> to vector<8x1x16xf16>
      %2 = vector.constant_mask [32] : vector<128xi1>
      %3 = vector.shape_cast %2 : vector<128xi1> to vector<8x16xi1>
      annotation.mark %3 {mask_op_idx = -1 : i32} : vector<8x16xi1>
      %subview_1 = memref.subview %subview[0, 0, 0] [2, 1, 16] [1, 1, 1] : memref<2x1x16xf16, strided<[2064, 16, 1], offset: ?>, #hivm.address_space<ub>> to memref<2x16xf16, #map9, #hivm.address_space<ub>>
      %4 = vector.shape_cast %1 : vector<8x1x16xf16> to vector<8x16xf16>
      vector.transfer_write %4, %subview_1[%c0, %c0], %3 {in_bounds = [true, true]} : vector<8x16xf16>, memref<2x16xf16, #map9, #hivm.address_space<ub>>
    }
    return
  }
  func.func @_attn_fwd_mix_aiv_outlined_vf_2(%arg0: memref<128x32xf32, #hivm.address_space<ub>>) attributes {hivm.func_core_type = #hivm.func_core_type<AIV>, hivm.vector_function, no_inline} {
    %cst = arith.constant dense<0xFF800000> : vector<64xf32>
    %c1 = arith.constant 1 : index
    %c128 = arith.constant 128 : index
    %c0 = arith.constant 0 : index
    scf.for %arg1 = %c0 to %c128 step %c1 {
      %subview = memref.subview %arg0[%arg1, 0] [1, 32] [1, 1] : memref<128x32xf32, #hivm.address_space<ub>> to memref<1x32xf32, strided<[32, 1], offset: ?>, #hivm.address_space<ub>>
      %0 = vector.constant_mask [32] : vector<64xi1>
      annotation.mark %0 {mask_op_idx = 0 : i32} : vector<64xi1>
      %subview_0 = memref.subview %subview[0, 0] [1, 32] [1, 1] : memref<1x32xf32, strided<[32, 1], offset: ?>, #hivm.address_space<ub>> to memref<32xf32, #map, #hivm.address_space<ub>>
      vector.transfer_write %cst, %subview_0[%c0], %0 {in_bounds = [true]} : vector<64xf32>, memref<32xf32, #map, #hivm.address_space<ub>>
    }
    return
  }
  func.func @_attn_fwd_mix_aiv_outlined_vf_3(%arg0: memref<128x32xf32, #hivm.address_space<ub>>, %arg1: memref<1xf32, #hivm.address_space<ub>>, %arg2: memref<128x32xf32, #hivm.address_space<ub>>, %arg3: memref<128x32xi32, #hivm.address_space<ub>>) attributes {hivm.func_core_type = #hivm.func_core_type<AIV>, hivm.vector_function, no_inline} {
    %cst = arith.constant dense<2147483647> : vector<64xi32>
    %cst_0 = arith.constant 0.000000e+00 : f32
    %c1 = arith.constant 1 : index
    %c128 = arith.constant 128 : index
    %c0 = arith.constant 0 : index
    scf.for %arg4 = %c0 to %c128 step %c1 {
      %subview = memref.subview %arg0[%arg4, 0] [1, 32] [1, 1] : memref<128x32xf32, #hivm.address_space<ub>> to memref<1x32xf32, strided<[32, 1], offset: ?>, #hivm.address_space<ub>>
      %subview_1 = memref.subview %arg2[%arg4, 0] [1, 32] [1, 1] : memref<128x32xf32, #hivm.address_space<ub>> to memref<1x32xf32, strided<[32, 1], offset: ?>, #hivm.address_space<ub>>
      %0 = vector.constant_mask [32] : vector<64xi1>
      annotation.mark %0 {mask_op_idx = 0 : i32} : vector<64xi1>
      %subview_2 = memref.subview %subview[0, 0] [1, 32] [1, 1] : memref<1x32xf32, strided<[32, 1], offset: ?>, #hivm.address_space<ub>> to memref<32xf32, #map, #hivm.address_space<ub>>
      %1 = vector.transfer_read %subview_2[%c0], %cst_0, %0 {in_bounds = [true]} : memref<32xf32, #map, #hivm.address_space<ub>>, vector<64xf32>
      annotation.mark %1 {reached_mask_ops_idx = 0 : i32} : vector<64xf32>
      %2 = vector.transfer_read %arg1[%c0], %cst_0 {in_bounds = [true, true], permutation_map = #map10} : memref<1xf32, #hivm.address_space<ub>>, vector<1x64xf32>
      %3 = vector.shape_cast %2 : vector<1x64xf32> to vector<64xf32>
      annotation.mark %3 {reached_mask_ops_idx = 0 : i32} : vector<64xf32>
      %4 = arith.mulf %1, %3 : vector<64xf32>
      annotation.mark %4 {reached_mask_ops_idx = 0 : i32} : vector<64xf32>
      %subview_3 = memref.subview %subview_1[0, 0] [1, 32] [1, 1] : memref<1x32xf32, strided<[32, 1], offset: ?>, #hivm.address_space<ub>> to memref<32xf32, #map, #hivm.address_space<ub>>
      vector.transfer_write %4, %subview_3[%c0], %0 {in_bounds = [true]} : vector<64xf32>, memref<32xf32, #map, #hivm.address_space<ub>>
      %subview_4 = memref.subview %arg3[%arg4, 0] [1, 32] [1, 1] : memref<128x32xi32, #hivm.address_space<ub>> to memref<1x32xi32, strided<[32, 1], offset: ?>, #hivm.address_space<ub>>
      %subview_5 = memref.subview %subview_4[0, 0] [1, 32] [1, 1] : memref<1x32xi32, strided<[32, 1], offset: ?>, #hivm.address_space<ub>> to memref<32xi32, #map, #hivm.address_space<ub>>
      vector.transfer_write %cst, %subview_5[%c0], %0 {in_bounds = [true]} : vector<64xi32>, memref<32xi32, #map, #hivm.address_space<ub>>
    }
    return
  }
  func.func @_attn_fwd_mix_aiv_outlined_vf_4(%arg0: memref<128x32xi32, #hivm.address_space<ub>>, %arg1: memref<128x32xi32, #hivm.address_space<ub>>, %arg2: memref<128x32xf32, #hivm.address_space<ub>>, %arg3: memref<128xf32, #hivm.address_space<ub>>) attributes {hivm.func_core_type = #hivm.func_core_type<AIV>, hivm.vector_function, no_inline} {
    %cst = arith.constant dense<0xFF800000> : vector<64xf32>
    %cst_0 = arith.constant dense<0> : vector<64xi32>
    %cst_1 = arith.constant dense<1> : vector<64xi32>
    %cst_2 = arith.constant dense<-2139095040> : vector<64xi32>
    %c0_i32 = arith.constant 0 : i32
    %cst_3 = arith.constant 0.000000e+00 : f32
    %c1 = arith.constant 1 : index
    %c128 = arith.constant 128 : index
    %c0 = arith.constant 0 : index
    scf.for %arg4 = %c0 to %c128 step %c1 {
      %subview = memref.subview %arg3[%arg4] [1] [1] : memref<128xf32, #hivm.address_space<ub>> to memref<1xf32, strided<[1], offset: ?>, #hivm.address_space<ub>>
      %subview_4 = memref.subview %arg0[%arg4, 0] [1, 32] [1, 1] : memref<128x32xi32, #hivm.address_space<ub>> to memref<1x32xi32, strided<[32, 1], offset: ?>, #hivm.address_space<ub>>
      %subview_5 = memref.subview %arg1[%arg4, 0] [1, 32] [1, 1] : memref<128x32xi32, #hivm.address_space<ub>> to memref<1x32xi32, strided<[32, 1], offset: ?>, #hivm.address_space<ub>>
      %0 = vector.constant_mask [32] : vector<64xi1>
      %subview_6 = memref.subview %subview_4[0, 0] [1, 32] [1, 1] : memref<1x32xi32, strided<[32, 1], offset: ?>, #hivm.address_space<ub>> to memref<32xi32, #map, #hivm.address_space<ub>>
      %1 = vector.transfer_read %subview_6[%c0], %c0_i32, %0 {in_bounds = [true]} : memref<32xi32, #map, #hivm.address_space<ub>>, vector<64xi32>
      %subview_7 = memref.subview %subview_5[0, 0] [1, 32] [1, 1] : memref<1x32xi32, strided<[32, 1], offset: ?>, #hivm.address_space<ub>> to memref<32xi32, #map, #hivm.address_space<ub>>
      %2 = vector.transfer_read %subview_7[%c0], %c0_i32, %0 {in_bounds = [true]} : memref<32xi32, #map, #hivm.address_space<ub>>, vector<64xi32>
      %3 = arith.andi %1, %2 : vector<64xi32>
      %subview_8 = memref.subview %arg2[%arg4, 0] [1, 32] [1, 1] : memref<128x32xf32, #hivm.address_space<ub>> to memref<1x32xf32, strided<[32, 1], offset: ?>, #hivm.address_space<ub>>
      %subview_9 = memref.subview %subview_8[0, 0] [1, 32] [1, 1] : memref<1x32xf32, strided<[32, 1], offset: ?>, #hivm.address_space<ub>> to memref<32xf32, #map, #hivm.address_space<ub>>
      %4 = vector.transfer_read %subview_9[%c0], %cst_3, %0 {in_bounds = [true]} : memref<32xf32, #map, #hivm.address_space<ub>>, vector<64xf32>
      %5 = arith.addi %3, %cst_2 : vector<64xi32>
      %6 = arith.minsi %5, %cst_1 : vector<64xi32>
      %7 = arith.maxsi %6, %cst_0 : vector<64xi32>
      %8 = arith.cmpi ne, %7, %cst_0 : vector<64xi32>
      %9 = arith.select %8, %cst, %4 : vector<64xi1>, vector<64xf32>
      %10 = arith.select %0, %9, %cst : vector<64xi1>, vector<64xf32>
      %subview_10 = memref.subview %subview[0] [1] [1] : memref<1xf32, strided<[1], offset: ?>, #hivm.address_space<ub>> to memref<f32, strided<[], offset: ?>, #hivm.address_space<ub>>
      %11 = vector.transfer_read %subview_10[], %cst_3 : memref<f32, strided<[], offset: ?>, #hivm.address_space<ub>>, vector<f32>
      %12 = vector.shape_cast %11 : vector<f32> to vector<1xf32>
      %13 = builtin.unrealized_conversion_cast %12 : vector<1xf32> to f32
      %14 = vector.reduction <maximumf>, %10, %13 {withoutInitMergeOp} : vector<64xf32> into f32
      %15 = builtin.unrealized_conversion_cast %14 : f32 to vector<1xf32>
      %16 = vector.shape_cast %15 : vector<1xf32> to vector<f32>
      vector.transfer_write %16, %subview_10[] : vector<f32>, memref<f32, strided<[], offset: ?>, #hivm.address_space<ub>>
    }
    return
  }
  func.func @_attn_fwd_mix_aiv_outlined_vf_5(%arg0: memref<128xi32, #hivm.address_space<ub>>, %arg1: memref<128xi32, #hivm.address_space<ub>>, %arg2: memref<128xf32, #hivm.address_space<ub>>, %arg3: memref<128xi32, #hivm.address_space<ub>>, %arg4: memref<128xf32, #hivm.address_space<ub>>, %arg5: memref<128xf32, #hivm.address_space<ub>>, %arg6: memref<128xf32, #hivm.address_space<ub>>, %arg7: memref<128x128xf32, #hivm.address_space<ub>>, %arg8: memref<128x32xf32, #hivm.address_space<ub>>, %arg9: memref<128x128xf32, #hivm.address_space<ub>>, %arg10: memref<128xf32, #hivm.address_space<ub>>, %arg11: memref<2x128x16xf16, strided<[2064, 16, 1]>, #hivm.address_space<ub>>, %arg12: memref<128xf32, #hivm.address_space<ub>>, %arg13: memref<128xf32, #hivm.address_space<ub>>) attributes {hivm.func_core_type = #hivm.func_core_type<AIV>, hivm.vector_function, no_inline} {
    %cst = arith.constant dense<0.000000e+00> : vector<64xf32>
    %cst_0 = arith.constant dense<0.693147182> : vector<64xf32>
    %cst_1 = arith.constant dense<0xFF800000> : vector<64xf32>
    %cst_2 = arith.constant dense<0> : vector<64xi32>
    %cst_3 = arith.constant dense<1> : vector<64xi32>
    %cst_4 = arith.constant dense<-2139095040> : vector<64xi32>
    %cst_5 = arith.constant 0.000000e+00 : f32
    %c0_i32 = arith.constant 0 : i32
    %c1 = arith.constant 1 : index
    %c64 = arith.constant 64 : index
    %c128 = arith.constant 128 : index
    %c0 = arith.constant 0 : index
    scf.for %arg14 = %c0 to %c128 step %c64 {
      %subview = memref.subview %arg0[%arg14] [64] [1] : memref<128xi32, #hivm.address_space<ub>> to memref<64xi32, strided<[1], offset: ?>, #hivm.address_space<ub>>
      %subview_6 = memref.subview %arg1[%arg14] [64] [1] : memref<128xi32, #hivm.address_space<ub>> to memref<64xi32, strided<[1], offset: ?>, #hivm.address_space<ub>>
      %0 = vector.transfer_read %subview[%c0], %c0_i32 {in_bounds = [true]} : memref<64xi32, strided<[1], offset: ?>, #hivm.address_space<ub>>, vector<64xi32>
      %1 = vector.transfer_read %subview_6[%c0], %c0_i32 {in_bounds = [true]} : memref<64xi32, strided<[1], offset: ?>, #hivm.address_space<ub>>, vector<64xi32>
      %2 = arith.andi %0, %1 : vector<64xi32>
      %subview_7 = memref.subview %arg2[%arg14] [64] [1] : memref<128xf32, #hivm.address_space<ub>> to memref<64xf32, strided<[1], offset: ?>, #hivm.address_space<ub>>
      %subview_8 = memref.subview %arg3[%arg14] [64] [1] : memref<128xi32, #hivm.address_space<ub>> to memref<64xi32, strided<[1], offset: ?>, #hivm.address_space<ub>>
      %3 = vector.transfer_read %subview_8[%c0], %c0_i32 {in_bounds = [true]} : memref<64xi32, strided<[1], offset: ?>, #hivm.address_space<ub>>, vector<64xi32>
      %4 = arith.andi %3, %1 : vector<64xi32>
      %subview_9 = memref.subview %arg4[%arg14] [64] [1] : memref<128xf32, #hivm.address_space<ub>> to memref<64xf32, strided<[1], offset: ?>, #hivm.address_space<ub>>
      %subview_10 = memref.subview %arg5[%arg14] [64] [1] : memref<128xf32, #hivm.address_space<ub>> to memref<64xf32, strided<[1], offset: ?>, #hivm.address_space<ub>>
      %5 = vector.transfer_read %subview_7[%c0], %cst_5 {in_bounds = [true]} : memref<64xf32, strided<[1], offset: ?>, #hivm.address_space<ub>>, vector<64xf32>
      %6 = vector.transfer_read %subview_9[%c0], %cst_5 {in_bounds = [true]} : memref<64xf32, strided<[1], offset: ?>, #hivm.address_space<ub>>, vector<64xf32>
      %7 = arith.addi %4, %cst_4 : vector<64xi32>
      %8 = arith.minsi %7, %cst_3 : vector<64xi32>
      %9 = arith.maxsi %8, %cst_2 : vector<64xi32>
      %10 = arith.cmpi ne, %9, %cst_2 : vector<64xi32>
      %11 = arith.select %10, %cst_1, %6 : vector<64xi1>, vector<64xf32>
      %12 = arith.addi %2, %cst_4 : vector<64xi32>
      %13 = arith.minsi %12, %cst_3 : vector<64xi32>
      %14 = arith.maxsi %13, %cst_2 : vector<64xi32>
      %15 = arith.cmpi ne, %14, %cst_2 : vector<64xi32>
      %16 = arith.select %15, %cst_1, %5 : vector<64xi1>, vector<64xf32>
      %17 = arith.maximumf %16, %11 : vector<64xf32>
      vector.transfer_write %17, %subview_10[%c0] {in_bounds = [true]} : vector<64xf32>, memref<64xf32, strided<[1], offset: ?>, #hivm.address_space<ub>>
    }
    scf.for %arg14 = %c0 to %c128 step %c64 {
      %subview = memref.subview %arg2[%arg14] [64] [1] : memref<128xf32, #hivm.address_space<ub>> to memref<64xf32, strided<[1], offset: ?>, #hivm.address_space<ub>>
      %subview_6 = memref.subview %arg5[%arg14] [64] [1] : memref<128xf32, #hivm.address_space<ub>> to memref<64xf32, strided<[1], offset: ?>, #hivm.address_space<ub>>
      %subview_7 = memref.subview %arg6[%arg14] [64] [1] : memref<128xf32, #hivm.address_space<ub>> to memref<64xf32, strided<[1], offset: ?>, #hivm.address_space<ub>>
      %0 = vector.transfer_read %subview[%c0], %cst_5 {in_bounds = [true]} : memref<64xf32, strided<[1], offset: ?>, #hivm.address_space<ub>>, vector<64xf32>
      %1 = vector.transfer_read %subview_6[%c0], %cst_5 {in_bounds = [true]} : memref<64xf32, strided<[1], offset: ?>, #hivm.address_space<ub>>, vector<64xf32>
      %2 = arith.subf %0, %1 : vector<64xf32>
      %3 = arith.mulf %2, %cst_0 : vector<64xf32>
      %4 = math.exp %3 : vector<64xf32>
      vector.transfer_write %4, %subview_7[%c0] {in_bounds = [true]} : vector<64xf32>, memref<64xf32, strided<[1], offset: ?>, #hivm.address_space<ub>>
    }
    scf.for %arg14 = %c0 to %c128 step %c1 {
      %subview = memref.subview %arg6[%arg14] [1] [1] : memref<128xf32, #hivm.address_space<ub>> to memref<1xf32, strided<[1], offset: ?>, #hivm.address_space<ub>>
      scf.for %arg15 = %c0 to %c128 step %c64 {
        %subview_13 = memref.subview %arg7[%arg14, %arg15] [1, 64] [1, 1] : memref<128x128xf32, #hivm.address_space<ub>> to memref<1x64xf32, strided<[128, 1], offset: ?>, #hivm.address_space<ub>>
        %subview_14 = memref.subview %arg9[%arg14, %arg15] [1, 64] [1, 1] : memref<128x128xf32, #hivm.address_space<ub>> to memref<1x64xf32, strided<[128, 1], offset: ?>, #hivm.address_space<ub>>
        %subview_15 = memref.subview %subview_13[0, 0] [1, 64] [1, 1] : memref<1x64xf32, strided<[128, 1], offset: ?>, #hivm.address_space<ub>> to memref<64xf32, #map, #hivm.address_space<ub>>
        %18 = vector.transfer_read %subview_15[%c0], %cst_5 {in_bounds = [true]} : memref<64xf32, #map, #hivm.address_space<ub>>, vector<64xf32>
        %19 = vector.transfer_read %subview[%c0], %cst_5 {in_bounds = [true, true], permutation_map = #map1} : memref<1xf32, strided<[1], offset: ?>, #hivm.address_space<ub>>, vector<1x64xf32>
        %20 = vector.shape_cast %19 : vector<1x64xf32> to vector<64xf32>
        %21 = arith.mulf %18, %20 : vector<64xf32>
        %subview_16 = memref.subview %subview_14[0, 0] [1, 64] [1, 1] : memref<1x64xf32, strided<[128, 1], offset: ?>, #hivm.address_space<ub>> to memref<64xf32, #map, #hivm.address_space<ub>>
        vector.transfer_write %21, %subview_16[%c0] {in_bounds = [true]} : vector<64xf32>, memref<64xf32, #map, #hivm.address_space<ub>>
      }
      %subview_6 = memref.subview %arg10[%arg14] [1] [1] : memref<128xf32, #hivm.address_space<ub>> to memref<1xf32, strided<[1], offset: ?>, #hivm.address_space<ub>>
      %subview_7 = memref.subview %arg8[%arg14, 0] [1, 32] [1, 1] : memref<128x32xf32, #hivm.address_space<ub>> to memref<1x32xf32, strided<[32, 1], offset: ?>, #hivm.address_space<ub>>
      %subview_8 = memref.subview %arg5[%arg14] [1] [1] : memref<128xf32, #hivm.address_space<ub>> to memref<1xf32, strided<[1], offset: ?>, #hivm.address_space<ub>>
      %0 = vector.constant_mask [32] : vector<64xi1>
      %subview_9 = memref.subview %subview_7[0, 0] [1, 32] [1, 1] : memref<1x32xf32, strided<[32, 1], offset: ?>, #hivm.address_space<ub>> to memref<32xf32, #map, #hivm.address_space<ub>>
      %1 = vector.transfer_read %subview_9[%c0], %cst_5, %0 {in_bounds = [true]} : memref<32xf32, #map, #hivm.address_space<ub>>, vector<64xf32>
      %2 = vector.transfer_read %subview_8[%c0], %cst_5 {in_bounds = [true, true], permutation_map = #map1} : memref<1xf32, strided<[1], offset: ?>, #hivm.address_space<ub>>, vector<1x64xf32>
      %3 = vector.shape_cast %2 : vector<1x64xf32> to vector<64xf32>
      %4 = arith.subf %1, %3 : vector<64xf32>
      %5 = arith.mulf %4, %cst_0 : vector<64xf32>
      %6 = math.exp %5 : vector<64xf32>
      %7 = arith.select %0, %6, %cst : vector<64xi1>, vector<64xf32>
      %8 = arith.addf %7, %cst {reductionOp} : vector<64xf32>
      %subview_10 = memref.subview %subview_6[0] [1] [1] : memref<1xf32, strided<[1], offset: ?>, #hivm.address_space<ub>> to memref<f32, strided<[], offset: ?>, #hivm.address_space<ub>>
      %9 = vector.transfer_read %subview_10[], %cst_5 : memref<f32, strided<[], offset: ?>, #hivm.address_space<ub>>, vector<f32>
      %10 = vector.shape_cast %9 : vector<f32> to vector<1xf32>
      %11 = builtin.unrealized_conversion_cast %10 : vector<1xf32> to f32
      %12 = vector.reduction <add>, %8, %11 {withoutInitMergeOp} : vector<64xf32> into f32
      %13 = builtin.unrealized_conversion_cast %12 : f32 to vector<1xf32>
      %14 = vector.shape_cast %13 : vector<1xf32> to vector<f32>
      vector.transfer_write %14, %subview_10[] : vector<f32>, memref<f32, strided<[], offset: ?>, #hivm.address_space<ub>>
      %subview_11 = memref.subview %arg11[0, %arg14, 0] [2, 1, 16] [1, 1, 1] : memref<2x128x16xf16, strided<[2064, 16, 1]>, #hivm.address_space<ub>> to memref<2x1x16xf16, strided<[2064, 16, 1], offset: ?>, #hivm.address_space<ub>>
      %15 = arith.truncf %6 {enable_saturate = false, round_mode = #hfusion.round_mode<rint>, unsigned_mode = #hfusion.unsigned_mode<si2si>} : vector<64xf32> to vector<64xf16>
      %16 = vector.shape_cast %0 : vector<64xi1> to vector<4x16xi1>
      annotation.mark %16 {mask_op_idx = -1 : i32} : vector<4x16xi1>
      %subview_12 = memref.subview %subview_11[0, 0, 0] [2, 1, 16] [1, 1, 1] : memref<2x1x16xf16, strided<[2064, 16, 1], offset: ?>, #hivm.address_space<ub>> to memref<2x16xf16, #map9, #hivm.address_space<ub>>
      %17 = vector.shape_cast %15 : vector<64xf16> to vector<4x16xf16>
      vector.transfer_write %17, %subview_12[%c0, %c0], %16 {in_bounds = [true, true]} : vector<4x16xf16>, memref<2x16xf16, #map9, #hivm.address_space<ub>>
    }
    scf.for %arg14 = %c0 to %c128 step %c64 {
      %subview = memref.subview %arg12[%arg14] [64] [1] : memref<128xf32, #hivm.address_space<ub>> to memref<64xf32, strided<[1], offset: ?>, #hivm.address_space<ub>>
      %subview_6 = memref.subview %arg6[%arg14] [64] [1] : memref<128xf32, #hivm.address_space<ub>> to memref<64xf32, strided<[1], offset: ?>, #hivm.address_space<ub>>
      %subview_7 = memref.subview %arg10[%arg14] [64] [1] : memref<128xf32, #hivm.address_space<ub>> to memref<64xf32, strided<[1], offset: ?>, #hivm.address_space<ub>>
      %subview_8 = memref.subview %arg13[%arg14] [64] [1] : memref<128xf32, #hivm.address_space<ub>> to memref<64xf32, strided<[1], offset: ?>, #hivm.address_space<ub>>
      %0 = vector.transfer_read %subview[%c0], %cst_5 {in_bounds = [true]} : memref<64xf32, strided<[1], offset: ?>, #hivm.address_space<ub>>, vector<64xf32>
      %1 = vector.transfer_read %subview_6[%c0], %cst_5 {in_bounds = [true]} : memref<64xf32, strided<[1], offset: ?>, #hivm.address_space<ub>>, vector<64xf32>
      %2 = vector.transfer_read %subview_7[%c0], %cst_5 {in_bounds = [true]} : memref<64xf32, strided<[1], offset: ?>, #hivm.address_space<ub>>, vector<64xf32>
      %3 = arith.mulf %0, %1 : vector<64xf32>
      %4 = arith.addf %3, %2 : vector<64xf32>
      vector.transfer_write %4, %subview_8[%c0] {in_bounds = [true]} : vector<64xf32>, memref<64xf32, strided<[1], offset: ?>, #hivm.address_space<ub>>
    }
    return
  }
  func.func @_attn_fwd_mix_aiv_outlined_vf_6(%arg0: memref<128x128xf32, #hivm.address_space<ub>>, %arg1: memref<128x128xf32, #hivm.address_space<ub>>, %arg2: memref<128x128xf32, #hivm.address_space<ub>>) attributes {hivm.func_core_type = #hivm.func_core_type<AIV>, hivm.vector_function, no_inline} {
    %cst = arith.constant 0.000000e+00 : f32
    %c1 = arith.constant 1 : index
    %c64 = arith.constant 64 : index
    %c128 = arith.constant 128 : index
    %c0 = arith.constant 0 : index
    scf.for %arg3 = %c0 to %c128 step %c1 {
      scf.for %arg4 = %c0 to %c128 step %c64 {
        %subview = memref.subview %arg0[%arg3, %arg4] [1, 64] [1, 1] : memref<128x128xf32, #hivm.address_space<ub>> to memref<1x64xf32, strided<[128, 1], offset: ?>, #hivm.address_space<ub>>
        %subview_0 = memref.subview %arg1[%arg3, %arg4] [1, 64] [1, 1] : memref<128x128xf32, #hivm.address_space<ub>> to memref<1x64xf32, strided<[128, 1], offset: ?>, #hivm.address_space<ub>>
        %subview_1 = memref.subview %arg2[%arg3, %arg4] [1, 64] [1, 1] : memref<128x128xf32, #hivm.address_space<ub>> to memref<1x64xf32, strided<[128, 1], offset: ?>, #hivm.address_space<ub>>
        %subview_2 = memref.subview %subview[0, 0] [1, 64] [1, 1] : memref<1x64xf32, strided<[128, 1], offset: ?>, #hivm.address_space<ub>> to memref<64xf32, #map, #hivm.address_space<ub>>
        %0 = vector.transfer_read %subview_2[%c0], %cst {in_bounds = [true]} : memref<64xf32, #map, #hivm.address_space<ub>>, vector<64xf32>
        %subview_3 = memref.subview %subview_0[0, 0] [1, 64] [1, 1] : memref<1x64xf32, strided<[128, 1], offset: ?>, #hivm.address_space<ub>> to memref<64xf32, #map, #hivm.address_space<ub>>
        %1 = vector.transfer_read %subview_3[%c0], %cst {in_bounds = [true]} : memref<64xf32, #map, #hivm.address_space<ub>>, vector<64xf32>
        %2 = arith.addf %0, %1 : vector<64xf32>
        %subview_4 = memref.subview %subview_1[0, 0] [1, 64] [1, 1] : memref<1x64xf32, strided<[128, 1], offset: ?>, #hivm.address_space<ub>> to memref<64xf32, #map, #hivm.address_space<ub>>
        vector.transfer_write %2, %subview_4[%c0] {in_bounds = [true]} : vector<64xf32>, memref<64xf32, #map, #hivm.address_space<ub>>
      }
    }
    return
  }
  func.func @_attn_fwd_mix_aiv_outlined_vf_7(%arg0: memref<128x128xf32, #hivm.address_space<ub>>) attributes {hivm.func_core_type = #hivm.func_core_type<AIV>, hivm.vector_function, no_inline} {
    %cst = arith.constant dense<0.000000e+00> : vector<64xf32>
    %c1 = arith.constant 1 : index
    %c64 = arith.constant 64 : index
    %c128 = arith.constant 128 : index
    %c0 = arith.constant 0 : index
    scf.for %arg1 = %c0 to %c128 step %c1 {
      scf.for %arg2 = %c0 to %c128 step %c64 {
        %subview = memref.subview %arg0[%arg1, %arg2] [1, 64] [1, 1] : memref<128x128xf32, #hivm.address_space<ub>> to memref<1x64xf32, strided<[128, 1], offset: ?>, #hivm.address_space<ub>>
        %subview_0 = memref.subview %subview[0, 0] [1, 64] [1, 1] : memref<1x64xf32, strided<[128, 1], offset: ?>, #hivm.address_space<ub>> to memref<64xf32, #map, #hivm.address_space<ub>>
        vector.transfer_write %cst, %subview_0[%c0] {in_bounds = [true]} : vector<64xf32>, memref<64xf32, #map, #hivm.address_space<ub>>
      }
    }
    return
  }
  func.func @_attn_fwd_mix_aiv_outlined_vf_8(%arg0: memref<128xf32, #hivm.address_space<ub>>, %arg1: memref<128xf32, #hivm.address_space<ub>>) attributes {hivm.func_core_type = #hivm.func_core_type<AIV>, hivm.vector_function, no_inline} {
    %cst = arith.constant dense<1.000000e+00> : vector<64xf32>
    %cst_0 = arith.constant dense<0xFF800000> : vector<64xf32>
    %c64 = arith.constant 64 : index
    %c128 = arith.constant 128 : index
    %c0 = arith.constant 0 : index
    scf.for %arg2 = %c0 to %c128 step %c64 {
      %subview = memref.subview %arg0[%arg2] [64] [1] : memref<128xf32, #hivm.address_space<ub>> to memref<64xf32, strided<[1], offset: ?>, #hivm.address_space<ub>>
      vector.transfer_write %cst_0, %subview[%c0] {in_bounds = [true]} : vector<64xf32>, memref<64xf32, strided<[1], offset: ?>, #hivm.address_space<ub>>
      %subview_1 = memref.subview %arg1[%arg2] [64] [1] : memref<128xf32, #hivm.address_space<ub>> to memref<64xf32, strided<[1], offset: ?>, #hivm.address_space<ub>>
      vector.transfer_write %cst, %subview_1[%c0] {in_bounds = [true]} : vector<64xf32>, memref<64xf32, strided<[1], offset: ?>, #hivm.address_space<ub>>
    }
    return
  }
  func.func @_attn_fwd_mix_aiv_outlined_vf_9(%arg0: memref<1xf32, #hivm.address_space<ub>>, %arg1: memref<1xf32, #hivm.address_space<ub>>) attributes {hivm.func_core_type = #hivm.func_core_type<AIV>, hivm.vector_function, no_inline} {
    %cst = arith.constant dense<1.44269502> : vector<64xf32>
    %cst_0 = arith.constant 0.000000e+00 : f32
    %c0 = arith.constant 0 : index
    %0 = vector.constant_mask [1] : vector<64xi1>
    annotation.mark %0 {mask_op_idx = 0 : i32} : vector<64xi1>
    %1 = vector.transfer_read %arg0[%c0], %cst_0, %0 {in_bounds = [true]} : memref<1xf32, #hivm.address_space<ub>>, vector<64xf32>
    annotation.mark %1 {reached_mask_ops_idx = 0 : i32} : vector<64xf32>
    %2 = arith.mulf %1, %cst : vector<64xf32>
    annotation.mark %2 {reached_mask_ops_idx = 0 : i32} : vector<64xf32>
    vector.transfer_write %2, %arg1[%c0], %0 {in_bounds = [true]} : vector<64xf32>, memref<1xf32, #hivm.address_space<ub>>
    return
  }
  func.func @_attn_fwd_mix_aiv_outlined_vf_10(%arg0: memref<128xi32, #hivm.address_space<ub>>) attributes {hivm.func_core_type = #hivm.func_core_type<AIV>, hivm.vector_function, no_inline} {
    %cst = arith.constant dense<2147483647> : vector<64xi32>
    %c64 = arith.constant 64 : index
    %c128 = arith.constant 128 : index
    %c0 = arith.constant 0 : index
    scf.for %arg1 = %c0 to %c128 step %c64 {
      %subview = memref.subview %arg0[%arg1] [64] [1] : memref<128xi32, #hivm.address_space<ub>> to memref<64xi32, strided<[1], offset: ?>, #hivm.address_space<ub>>
      vector.transfer_write %cst, %subview[%c0] {in_bounds = [true]} : vector<64xi32>, memref<64xi32, strided<[1], offset: ?>, #hivm.address_space<ub>>
    }
    return
  }
  func.func @_attn_fwd_mix_aiv(%arg0: memref<?xi8, #hivm.address_space<gm>> {hacc.arg_type = #hacc.arg_type<sync_block_lock>}, %arg1: memref<?xi8, #hivm.address_space<gm>> {hacc.arg_type = #hacc.arg_type<workspace>}, %arg2: memref<?xf16, #hivm.address_space<gm>> {tt.divisibility = 16 : i32, tt.tensor_kind = 0 : i32}, %arg3: memref<?xf16, #hivm.address_space<gm>> {tt.divisibility = 16 : i32, tt.tensor_kind = 0 : i32}, %arg4: memref<?xf16, #hivm.address_space<gm>> {tt.divisibility = 16 : i32, tt.tensor_kind = 0 : i32}, %arg5: f32, %arg6: memref<?xf32, #hivm.address_space<gm>> {tt.divisibility = 16 : i32, tt.tensor_kind = 1 : i32}, %arg7: memref<?xf16, #hivm.address_space<gm>> {tt.divisibility = 16 : i32, tt.tensor_kind = 1 : i32}, %arg8: i32 {tt.divisibility = 16 : i32}, %arg9: i32 {tt.divisibility = 16 : i32}, %arg10: i32 {tt.divisibility = 16 : i32}, %arg11: i32 {tt.divisibility = 16 : i32}, %arg12: i32 {tt.divisibility = 16 : i32}, %arg13: i32 {tt.divisibility = 16 : i32}, %arg14: i32 {tt.divisibility = 16 : i32}, %arg15: i32 {tt.divisibility = 16 : i32}, %arg16: i32 {tt.divisibility = 16 : i32}, %arg17: i32 {tt.divisibility = 16 : i32}, %arg18: i32 {tt.divisibility = 16 : i32}, %arg19: i32 {tt.divisibility = 16 : i32}, %arg20: i32, %arg21: i32, %arg22: i32, %arg23: i32 {tt.divisibility = 16 : i32}, %arg24: i32 {tt.divisibility = 16 : i32}, %arg25: i32, %arg26: i32, %arg27: i32) attributes {SyncBlockLockArgIdx = 0 : i64, WorkspaceArgIdx = 1 : i64, func_dyn_memref_args = dense<[true, true, true, true, true, false, true, true, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false]> : vector<28xi1>, hacc.entry, hacc.function_kind = #hacc.function_kind<DEVICE>, hivm.func_core_type = #hivm.func_core_type<AIV>, hivm.part_of_mix, hivm.vf_mode = #hivm.vf_mode<SIMD>, mix_mode = "mix", parallel_mode = "simd"} {
    %c512_i64 = arith.constant 512 : i64
    %c49152_i64 = arith.constant 49152 : i64
    %c16448_i64 = arith.constant 16448 : i64
    %c183296_i64 = arith.constant 183296 : i64
    %c117760_i64 = arith.constant 117760 : i64
    %c117248_i64 = arith.constant 117248 : i64
    %c116736_i64 = arith.constant 116736 : i64
    %c116224_i64 = arith.constant 116224 : i64
    %c99840_i64 = arith.constant 99840 : i64
    %c32768_i64 = arith.constant 32768 : i64
    %c8192_i64 = arith.constant 8192 : i64
    %c0_i64 = arith.constant 0 : i64
    %c99328_i64 = arith.constant 99328 : i64
    %c98816_i64 = arith.constant 98816 : i64
    %c248832_i64 = arith.constant 248832 : i64
    %c33280_i64 = arith.constant 33280 : i64
    %c249344_i64 = arith.constant 249344 : i64
    %cst = arith.constant 0.000000e+00 : f16
    %c32_i32 = arith.constant 32 : i32
    %c0_i32 = arith.constant 0 : i32
    %c128_i32 = arith.constant 128 : i32
    %c32 = arith.constant 32 : index
    %c0 = arith.constant 0 : index
    hivm.hir.set_ctrl false at ctrl[60]
    hivm.hir.set_ctrl true at ctrl[48]
    %0 = arith.muli %arg25, %arg26 : i32
    %1 = arith.muli %0, %arg27 : i32
    annotation.mark %1 {logical_block_num} : i32
    %2 = hivm.hir.get_block_idx -> i64
    %3 = arith.trunci %2 : i64 to i32
    %4 = arith.divsi %3, %arg27 : i32
    %5 = arith.remsi %4, %arg26 : i32
    %6 = arith.muli %arg27, %arg26 : i32
    %7 = arith.divsi %3, %6 : i32
    %8 = arith.remsi %7, %arg25 : i32
    %9 = hivm.hir.pointer_cast(%c249344_i64) : memref<1xf32, #hivm.address_space<ub>>
    %10 = hivm.hir.pointer_cast(%c33280_i64) : memref<128x128xf32, #hivm.address_space<ub>>
    call @_attn_fwd_mix_aiv_outlined_vf_7(%10) {hivm.vector_function, no_inline} : (memref<128x128xf32, #hivm.address_space<ub>>) -> ()
    %11 = hivm.hir.pointer_cast(%c248832_i64) : memref<128xf32, #hivm.address_space<ub>>
    %12 = hivm.hir.pointer_cast(%c98816_i64) : memref<128xf32, #hivm.address_space<ub>>
    call @_attn_fwd_mix_aiv_outlined_vf_8(%11, %12) {hivm.vector_function, no_inline} : (memref<128xf32, #hivm.address_space<ub>>, memref<128xf32, #hivm.address_space<ub>>) -> ()
    %13 = arith.divsi %5, %arg21 : i32
    %14 = arith.remsi %5, %arg21 : i32
    %15 = arith.extsi %13 : i32 to i64
    %16 = arith.extsi %14 : i32 to i64
    %17 = arith.extsi %arg17 : i32 to i64
    %18 = arith.muli %15, %17 : i64
    %19 = arith.extsi %arg18 : i32 to i64
    %20 = arith.muli %16, %19 : i64
    %21 = arith.addi %18, %20 : i64
    %22 = arith.extsi %arg11 : i32 to i64
    %23 = arith.muli %15, %22 : i64
    %24 = arith.extsi %arg12 : i32 to i64
    %25 = arith.muli %16, %24 : i64
    %26 = arith.addi %23, %25 : i64
    %27 = arith.muli %8, %c128_i32 : i32
    %28 = arith.index_cast %27 : i32 to index
    %29 = arith.index_cast %21 : i64 to index
    %30 = arith.index_cast %arg19 : i32 to index
    %31 = affine.apply #map2()[%29, %28, %30]
    %reinterpret_cast = memref.reinterpret_cast %arg7 to offset: [%31], sizes: [128, 128], strides: [%30, 1] : memref<?xf16, #hivm.address_space<gm>> to memref<128x128xf16, strided<[?, 1], offset: ?>, #hivm.address_space<gm>>
    %32 = affine.apply #map3()[%28]
    %33 = arith.index_cast %arg23 : i32 to index
    %34 = arith.maxsi %28, %33 : index
    %35 = arith.minsi %32, %34 : index
    %36 = affine.apply #map4()[%35, %28]
    memref.store %arg5, %9[%c0] : memref<1xf32, #hivm.address_space<ub>>
    hivm.hir.set_flag[<PIPE_S>, <PIPE_V>, <EVENT_ID0>]
    %37 = hivm.hir.pointer_cast(%c249344_i64) : memref<1xf32, #hivm.address_space<ub>>
    hivm.hir.wait_flag[<PIPE_S>, <PIPE_V>, <EVENT_ID0>]
    call @_attn_fwd_mix_aiv_outlined_vf_9(%9, %37) {hivm.vector_function, no_inline} : (memref<1xf32, #hivm.address_space<ub>>, memref<1xf32, #hivm.address_space<ub>>) -> ()
    %38 = arith.muli %arg13, %c32_i32 : i32
    %39 = arith.index_cast %26 : i64 to index
    %40 = arith.index_cast %arg13 : i32 to index
    hivm.hir.sync_block_set[<VECTOR>, <PIPE_V>, <PIPE_FIX>] flag = 3
    hivm.hir.sync_block_set[<VECTOR>, <PIPE_V>, <PIPE_FIX>] flag = 5
    %41 = arith.index_cast %arg24 : i32 to index
    %42 = hivm.hir.get_sub_block_idx -> i64
    %43 = arith.index_cast %42 : i64 to index
    %44 = arith.cmpi eq, %43, %c0 : index
    %45 = hivm.hir.pointer_cast(%c99328_i64) : memref<128xi32, #hivm.address_space<ub>>
    call @_attn_fwd_mix_aiv_outlined_vf_10(%45) {hivm.vector_function, no_inline} : (memref<128xi32, #hivm.address_space<ub>>) -> ()
    %46 = arith.index_cast %38 : i32 to index
    %47:4 = scf.for %arg28 = %c0_i32 to %arg24 step %c32_i32 iter_args(%arg29 = %12, %arg30 = %10, %arg31 = %11, %arg32 = %39) -> (memref<128xf32, #hivm.address_space<ub>>, memref<128x128xf32, #hivm.address_space<ub>>, memref<128xf32, #hivm.address_space<ub>>, index)  : i32 {
      %cast = memref.cast %arg31 : memref<128xf32, #hivm.address_space<ub>> to memref<128xf32, strided<[?], offset: ?>, #hivm.address_space<ub>>
      %reinterpret_cast_4 = memref.reinterpret_cast %arg3 to offset: [%arg32], sizes: [128, 32], strides: [1, %40] : memref<?xf16, #hivm.address_space<gm>> to memref<128x32xf16, strided<[1, ?], offset: ?>, #hivm.address_space<gm>>
      %53 = hivm.hir.pointer_cast(%c0_i64) : memref<128x32xf16, #hivm.address_space<ub>>
      annotation.mark %53 {effects = ["write", "read"], hivm.tightly_coupled_buffer = #hivm.tightly_coupled_buffer<0>} : memref<128x32xf16, #hivm.address_space<ub>>
      %54 = arith.index_cast %arg28 : i32 to index
      %55 = affine.apply #map6()[%54]
      %56 = arith.maxsi %54, %41 : index
      %57 = arith.minsi %55, %56 : index
      %58 = affine.apply #map4()[%57, %54]
      %59 = arith.cmpi slt, %58, %c32 : index
      %subview_5 = memref.subview %reinterpret_cast_4[0, 0] [128, %58] [1, 1] : memref<128x32xf16, strided<[1, ?], offset: ?>, #hivm.address_space<gm>> to memref<128x?xf16, strided<[1, ?], offset: ?>, #hivm.address_space<gm>>
      %subview_6 = memref.subview %53[0, 0] [128, %58] [1, 1] : memref<128x32xf16, #hivm.address_space<ub>> to memref<128x?xf16, strided<[32, 1]>, #hivm.address_space<ub>>
      scf.if %59 {
        func.call @_attn_fwd_mix_aiv_outlined_vf_0(%53) {hivm.vector_function, no_inline} : (memref<128x32xf16, #hivm.address_space<ub>>) -> ()
      }
      hivm.hir.set_flag[<PIPE_V>, <PIPE_MTE2>, <EVENT_ID0>]
      hivm.hir.wait_flag[<PIPE_V>, <PIPE_MTE2>, <EVENT_ID0>]
      hivm.hir.load ins(%subview_5 : memref<128x?xf16, strided<[1, ?], offset: ?>, #hivm.address_space<gm>>) outs(%subview_6 : memref<128x?xf16, strided<[32, 1]>, #hivm.address_space<ub>>) pad_mode = <PadValue> pad_value = %cst : f16 eviction_policy = <EvictFirst>
      hivm.hir.set_flag[<PIPE_MTE2>, <PIPE_V>, <EVENT_ID0>]
      annotation.mark %53 {MayImplicitTransposeWithLastAxis} : memref<128x32xf16, #hivm.address_space<ub>>
      annotation.mark %53 {MayImplicitTransposeWithLastAxis} : memref<128x32xf16, #hivm.address_space<ub>>
      %60 = hivm.hir.pointer_cast(%c8192_i64) : memref<2x129x16x1xf16, #hivm.address_space<ub>>
      %subview_7 = memref.subview %60[0, 0, 0, 0] [2, 128, 16, 1] [1, 1, 1, 1] : memref<2x129x16x1xf16, #hivm.address_space<ub>> to memref<2x128x16xf16, strided<[2064, 16, 1]>, #hivm.address_space<ub>>
      hivm.hir.wait_flag[<PIPE_MTE2>, <PIPE_V>, <EVENT_ID0>]
      func.call @_attn_fwd_mix_aiv_outlined_vf_1(%53, %subview_7) {hivm.vector_function, no_inline} : (memref<128x32xf16, #hivm.address_space<ub>>, memref<2x128x16xf16, strided<[2064, 16, 1]>, #hivm.address_space<ub>>) -> ()
      hivm.hir.set_flag[<PIPE_V>, <PIPE_MTE3>, <EVENT_ID0>]
      %expand_shape = memref.expand_shape %subview_7 [[0], [1, 2], [3]] output_shape [2, 8, 16, 16] : memref<2x128x16xf16, strided<[2064, 16, 1]>, #hivm.address_space<ub>> into memref<2x8x16x16xf16, strided<[2064, 256, 16, 1]>, #hivm.address_space<ub>>
      %61 = hivm.hir.pointer_cast(%c32768_i64) : memref<2x8x16x16xf16, #hivm.address_space<cbuf>>
      annotation.mark %61 {effects = ["write", "read"], hivm.tightly_coupled_buffer = #hivm.tightly_coupled_buffer<1>} : memref<2x8x16x16xf16, #hivm.address_space<cbuf>>
      hivm.hir.sync_block_wait[<VECTOR>, <PIPE_MTE1>, <PIPE_MTE3>] flag = 2
      hivm.hir.wait_flag[<PIPE_V>, <PIPE_MTE3>, <EVENT_ID0>]
      scf.if %44 {
        %collapse_shape = memref.collapse_shape %expand_shape [[0], [1, 2, 3]] : memref<2x8x16x16xf16, strided<[2064, 256, 16, 1]>, #hivm.address_space<ub>> into memref<2x2048xf16, strided<[2064, 1]>, #hivm.address_space<ub>>
        %collapse_shape_13 = memref.collapse_shape %61 [[0], [1, 2, 3]] : memref<2x8x16x16xf16, #hivm.address_space<cbuf>> into memref<2x2048xf16, #hivm.address_space<cbuf>>
        hivm.hir.copy ins(%collapse_shape : memref<2x2048xf16, strided<[2064, 1]>, #hivm.address_space<ub>>) outs(%collapse_shape_13 : memref<2x2048xf16, #hivm.address_space<cbuf>>)
      } {limit_sub_block_id0}
      hivm.hir.set_flag[<PIPE_MTE3>, <PIPE_V>, <EVENT_ID0>]
      hivm.hir.sync_block_set[<VECTOR>, <PIPE_MTE3>, <PIPE_MTE1>] flag = 1
      %62 = affine.apply #map7()[%57, %54]
      %63 = hivm.hir.pointer_cast(%c0_i64) : memref<16384xi8, #hivm.address_space<ub>>
      %view = memref.view %63[%c0][%62] : memref<16384xi8, #hivm.address_space<ub>> to memref<128x?x1xf32, #hivm.address_space<ub>>
      %subview_8 = memref.subview %view[0, 0, 0] [128, %58, 1] [1, 1, 1] : memref<128x?x1xf32, #hivm.address_space<ub>> to memref<128x?xf32, strided<[?, 1]>, #hivm.address_space<ub>>
      annotation.mark %subview_8 {effects = ["write", "read"], hivm.tightly_coupled_buffer = #hivm.tightly_coupled_buffer<2>} : memref<128x?xf32, strided<[?, 1]>, #hivm.address_space<ub>>
      hivm.hir.sync_block_wait[<VECTOR>, <PIPE_FIX>, <PIPE_V>] flag = 0
      %64 = hivm.hir.pointer_cast(%c99840_i64) : memref<128x32xf32, #hivm.address_space<ub>>
      func.call @_attn_fwd_mix_aiv_outlined_vf_2(%64) {hivm.vector_function, no_inline} : (memref<128x32xf32, #hivm.address_space<ub>>) -> ()
      %subview_9 = memref.subview %64[0, 0] [128, %58] [1, 1] : memref<128x32xf32, #hivm.address_space<ub>> to memref<128x?xf32, strided<[32, 1]>, #hivm.address_space<ub>>
      hivm.hir.copy ins(%subview_8 : memref<128x?xf32, strided<[?, 1]>, #hivm.address_space<ub>>) outs(%subview_9 : memref<128x?xf32, strided<[32, 1]>, #hivm.address_space<ub>>)
      hivm.hir.sync_block_set[<VECTOR>, <PIPE_V>, <PIPE_FIX>] flag = 3
      %65 = hivm.hir.pointer_cast(%c99840_i64) : memref<128x32xf32, #hivm.address_space<ub>>
      %66 = hivm.hir.pointer_cast(%c0_i64) : memref<128x32xi32, #hivm.address_space<ub>>
      hivm.hir.wait_flag[<PIPE_MTE3>, <PIPE_V>, <EVENT_ID0>]
      func.call @_attn_fwd_mix_aiv_outlined_vf_3(%64, %37, %65, %66) {hivm.vector_function, no_inline} : (memref<128x32xf32, #hivm.address_space<ub>>, memref<1xf32, #hivm.address_space<ub>>, memref<128x32xf32, #hivm.address_space<ub>>, memref<128x32xi32, #hivm.address_space<ub>>) -> ()
      %67 = hivm.hir.bitcast %65 : memref<128x32xf32, #hivm.address_space<ub>> -> memref<128x32xi32, #hivm.address_space<ub>>
      %68 = hivm.hir.pointer_cast(%c116224_i64) : memref<128xf32, #hivm.address_space<ub>>
      func.call @_attn_fwd_mix_aiv_outlined_vf_4(%67, %66, %65, %68) {hivm.vector_function, no_inline} : (memref<128x32xi32, #hivm.address_space<ub>>, memref<128x32xi32, #hivm.address_space<ub>>, memref<128x32xf32, #hivm.address_space<ub>>, memref<128xf32, #hivm.address_space<ub>>) -> ()
      %69 = hivm.hir.bitcast %cast : memref<128xf32, strided<[?], offset: ?>, #hivm.address_space<ub>> -> memref<128xi32, strided<[?], offset: ?>, #hivm.address_space<ub>>
      %70 = hivm.hir.bitcast %68 : memref<128xf32, #hivm.address_space<ub>> -> memref<128xi32, #hivm.address_space<ub>>
      %71 = hivm.hir.pointer_cast(%c116736_i64) : memref<128xf32, #hivm.address_space<ub>>
      %72 = hivm.hir.pointer_cast(%c117248_i64) : memref<128xf32, #hivm.address_space<ub>>
      %73 = hivm.hir.pointer_cast(%c117760_i64) : memref<128x128xf32, #hivm.address_space<ub>>
      %74 = hivm.hir.pointer_cast(%c183296_i64) : memref<128xf32, #hivm.address_space<ub>>
      %75 = hivm.hir.pointer_cast(%c16448_i64) : memref<2x129x16x1xf16, #hivm.address_space<ub>>
      %subview_10 = memref.subview %75[0, 0, 0, 0] [2, 128, 16, 1] [1, 1, 1, 1] : memref<2x129x16x1xf16, #hivm.address_space<ub>> to memref<2x128x16xf16, strided<[2064, 16, 1]>, #hivm.address_space<ub>>
      %76 = hivm.hir.pointer_cast(%c98816_i64) : memref<128xf32, #hivm.address_space<ub>>
      %cast_11 = memref.cast %69 : memref<128xi32, strided<[?], offset: ?>, #hivm.address_space<ub>> to memref<128xi32, #hivm.address_space<ub>>
      func.call @_attn_fwd_mix_aiv_outlined_vf_5(%cast_11, %45, %arg31, %70, %68, %71, %72, %arg30, %65, %73, %74, %subview_10, %arg29, %76) {hivm.vector_function, no_inline} : (memref<128xi32, #hivm.address_space<ub>>, memref<128xi32, #hivm.address_space<ub>>, memref<128xf32, #hivm.address_space<ub>>, memref<128xi32, #hivm.address_space<ub>>, memref<128xf32, #hivm.address_space<ub>>, memref<128xf32, #hivm.address_space<ub>>, memref<128xf32, #hivm.address_space<ub>>, memref<128x128xf32, #hivm.address_space<ub>>, memref<128x32xf32, #hivm.address_space<ub>>, memref<128x128xf32, #hivm.address_space<ub>>, memref<128xf32, #hivm.address_space<ub>>, memref<2x128x16xf16, strided<[2064, 16, 1]>, #hivm.address_space<ub>>, memref<128xf32, #hivm.address_space<ub>>, memref<128xf32, #hivm.address_space<ub>>) -> ()
      hivm.hir.set_flag[<PIPE_V>, <PIPE_MTE3>, <EVENT_ID0>]
      %expand_shape_12 = memref.expand_shape %subview_10 [[0], [1, 2], [3]] output_shape [2, 8, 16, 16] : memref<2x128x16xf16, strided<[2064, 16, 1]>, #hivm.address_space<ub>> into memref<2x8x16x16xf16, strided<[2064, 256, 16, 1]>, #hivm.address_space<ub>>
      %77 = hivm.hir.pointer_cast(%c49152_i64) : memref<2x8x16x16xf16, #hivm.address_space<cbuf>>
      annotation.mark %77 {effects = ["write", "read"], hivm.tightly_coupled_buffer = #hivm.tightly_coupled_buffer<3>} : memref<2x8x16x16xf16, #hivm.address_space<cbuf>>
      hivm.hir.sync_block_wait[<VECTOR>, <PIPE_MTE1>, <PIPE_MTE3>] flag = 4
      hivm.hir.wait_flag[<PIPE_V>, <PIPE_MTE3>, <EVENT_ID0>]
      scf.if %44 {
        %collapse_shape = memref.collapse_shape %expand_shape_12 [[0], [1, 2, 3]] : memref<2x8x16x16xf16, strided<[2064, 256, 16, 1]>, #hivm.address_space<ub>> into memref<2x2048xf16, strided<[2064, 1]>, #hivm.address_space<ub>>
        %collapse_shape_13 = memref.collapse_shape %77 [[0], [1, 2, 3]] : memref<2x8x16x16xf16, #hivm.address_space<cbuf>> into memref<2x2048xf16, #hivm.address_space<cbuf>>
        hivm.hir.copy ins(%collapse_shape : memref<2x2048xf16, strided<[2064, 1]>, #hivm.address_space<ub>>) outs(%collapse_shape_13 : memref<2x2048xf16, #hivm.address_space<cbuf>>)
      } {limit_sub_block_id0}
      hivm.hir.sync_block_set[<VECTOR>, <PIPE_MTE3>, <PIPE_MTE1>] flag = 1
      %78 = hivm.hir.pointer_cast(%c183296_i64) : memref<128x128xf32, #hivm.address_space<ub>>
      annotation.mark %78 {effects = ["write", "read"], hivm.tightly_coupled_buffer = #hivm.tightly_coupled_buffer<4>} : memref<128x128xf32, #hivm.address_space<ub>>
      hivm.hir.sync_block_wait[<VECTOR>, <PIPE_FIX>, <PIPE_V>] flag = 0
      %79 = hivm.hir.pointer_cast(%c33280_i64) : memref<128x128xf32, #hivm.address_space<ub>>
      func.call @_attn_fwd_mix_aiv_outlined_vf_6(%78, %73, %79) {hivm.vector_function, no_inline} : (memref<128x128xf32, #hivm.address_space<ub>>, memref<128x128xf32, #hivm.address_space<ub>>, memref<128x128xf32, #hivm.address_space<ub>>) -> ()
      hivm.hir.sync_block_set[<VECTOR>, <PIPE_V>, <PIPE_FIX>] flag = 5
      %80 = affine.apply #map8()[%arg32, %46]
      %81 = hivm.hir.pointer_cast(%c248832_i64) : memref<128xf32, #hivm.address_space<ub>>
      hivm.hir.copy ins(%71 : memref<128xf32, #hivm.address_space<ub>>) outs(%81 : memref<128xf32, #hivm.address_space<ub>>)
      scf.yield %76, %79, %81, %80 : memref<128xf32, #hivm.address_space<ub>>, memref<128x128xf32, #hivm.address_space<ub>>, memref<128xf32, #hivm.address_space<ub>>, index
    }
    hivm.hir.set_flag[<PIPE_MTE3>, <PIPE_V>, <EVENT_ID0>]
    hivm.hir.sync_block_wait[<VECTOR>, <PIPE_MTE1>, <PIPE_MTE3>] flag = 2
    hivm.hir.sync_block_wait[<VECTOR>, <PIPE_MTE1>, <PIPE_MTE3>] flag = 4
    %48 = hivm.hir.pointer_cast(%c0_i64) : memref<128xf32, #hivm.address_space<ub>>
    %49 = arith.muli %5, %arg23 : i32
    %50 = arith.index_cast %49 : i32 to index
    %51 = affine.apply #map8()[%50, %28]
    %reinterpret_cast_0 = memref.reinterpret_cast %arg6 to offset: [%51], sizes: [128], strides: [1] : memref<?xf32, #hivm.address_space<gm>> to memref<128xf32, strided<[1], offset: ?>, #hivm.address_space<gm>>
    %subview = memref.subview %reinterpret_cast_0[0] [%36] [1] : memref<128xf32, strided<[1], offset: ?>, #hivm.address_space<gm>> to memref<?xf32, strided<[1], offset: ?>, #hivm.address_space<gm>>
    %52 = hivm.hir.pointer_cast(%c512_i64) : memref<128x128xf16, #hivm.address_space<ub>>
    hivm.hir.wait_flag[<PIPE_MTE3>, <PIPE_V>, <EVENT_ID0>]
    call @_attn_fwd_mix_aiv_outlined_merged_vf_0(%47#2, %47#0, %48, %47#0, %47#1, %52) {hivm.vector_function, no_inline, ptc_simdvf} : (memref<128xf32, #hivm.address_space<ub>>, memref<128xf32, #hivm.address_space<ub>>, memref<128xf32, #hivm.address_space<ub>>, memref<128xf32, #hivm.address_space<ub>>, memref<128x128xf32, #hivm.address_space<ub>>, memref<128x128xf16, #hivm.address_space<ub>>) -> ()
    hivm.hir.set_flag[<PIPE_V>, <PIPE_MTE3>, <EVENT_ID0>]
    %subview_1 = memref.subview %48[0] [%36] [1] : memref<128xf32, #hivm.address_space<ub>> to memref<?xf32, strided<[1]>, #hivm.address_space<ub>>
    hivm.hir.wait_flag[<PIPE_V>, <PIPE_MTE3>, <EVENT_ID0>]
    scf.if %44 {
      hivm.hir.store ins(%subview_1 : memref<?xf32, strided<[1]>, #hivm.address_space<ub>>) outs(%subview : memref<?xf32, strided<[1], offset: ?>, #hivm.address_space<gm>>)
    } {limit_sub_block_id0}
    %subview_2 = memref.subview %52[0, 0] [%36, 128] [1, 1] : memref<128x128xf16, #hivm.address_space<ub>> to memref<?x128xf16, strided<[128, 1]>, #hivm.address_space<ub>>
    %subview_3 = memref.subview %reinterpret_cast[0, 0] [%36, 128] [1, 1] : memref<128x128xf16, strided<[?, 1], offset: ?>, #hivm.address_space<gm>> to memref<?x128xf16, strided<[?, 1], offset: ?>, #hivm.address_space<gm>>
    scf.if %44 {
      hivm.hir.store ins(%subview_2 : memref<?x128xf16, strided<[128, 1]>, #hivm.address_space<ub>>) outs(%subview_3 : memref<?x128xf16, strided<[?, 1], offset: ?>, #hivm.address_space<gm>>)
    } {limit_sub_block_id0}
    hivm.hir.pipe_barrier[<PIPE_ALL>]
    return
  }
}

