# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
"""Structural and NPU end-to-end regressions for GraphOptimize LoadStoreTranspose."""

import json
import subprocess

import pytest
import triton
import triton.language as tl
from triton._C.libtriton import ir
from triton._C.libtriton.ascend import ir as ascend_ir
from triton.backends.ascend.compiler import NPUOptions, make_ttir
from triton.compiler.code_generator import ast_to_ttir
from triton.compiler.compiler import ASTSource
from triton.errors import TritonError

pytestmark = pytest.mark.backend("none")


@triton.jit
def fused_swiglu_fast_sigmoid(x):
    return tl.fdiv(1.0, 1.0 + tl.exp(-x))


@triton.jit
def fused_swiglu_fast_silu(x):
    dtype = x.type.element_ty
    x = x.to(tl.float32)
    return tl.fdiv(x, 1.0 + tl.exp(-x)).to(dtype)


@triton.jit
def fused_swiglu_fast_silu_bwd(dy, x):
    dtype = x.type.element_ty
    dy = dy.to(tl.float32)
    x = x.to(tl.float32)
    sigmoid = fused_swiglu_fast_sigmoid(x)
    return (dy * sigmoid * (1 + x * (1 - sigmoid))).to(dtype)


@triton.jit
def load_store_transpose_e2e_kernel(
    dy_ptr,
    g_ptr,
    fc_ptr,
    dg_ptr,
    dfc_ptr,
    db_g_ptr,
    db_fc_ptr,
    M,
    N,
    BLOCK_SIZE_M: tl.constexpr,
    BLOCK_SIZE_N: tl.constexpr,
):
    """Fixed-config fused-SwiGLU clone with the rule-1 implicit transpose.

    The original [column, row] pointer layout is intentionally retained: it is
    the load/store layout that LoadStoreTranspose changes to natural order.
    """
    dtype = dy_ptr.type.element_ty
    col_idx = tl.program_id(axis=0)
    col_off = col_idx * BLOCK_SIZE_N + tl.arange(0, BLOCK_SIZE_N)
    row_off = tl.arange(0, BLOCK_SIZE_M)
    dy_ptrs = dy_ptr + (row_off[None, :] * N + col_off[:, None])
    g_ptrs = g_ptr + (row_off[None, :] * N + col_off[:, None])
    fc_ptrs = fc_ptr + (row_off[None, :] * N + col_off[:, None])
    dg_ptrs = dg_ptr + (row_off[None, :] * N + col_off[:, None])
    dfc_ptrs = dfc_ptr + (row_off[None, :] * N + col_off[:, None])
    sum_b_g = tl.zeros((BLOCK_SIZE_N, BLOCK_SIZE_M), dtype=tl.float32)
    sum_b_fc = tl.zeros((BLOCK_SIZE_N, BLOCK_SIZE_M), dtype=tl.float32)

    for row_idx in range(0, tl.cdiv(M, BLOCK_SIZE_M)):
        mask = (row_off[None, :] < M - row_idx * BLOCK_SIZE_M) & (col_off[:, None] < N)
        dy = tl.load(dy_ptrs, mask=mask, other=0.0).to(tl.float32)
        g = tl.load(g_ptrs, mask=mask, other=0.0)
        fc = tl.load(fc_ptrs, mask=mask, other=0.0).to(tl.float32)
        silu_g = fused_swiglu_fast_silu(g)
        dg = (dy * fc).to(dtype)
        dg = fused_swiglu_fast_silu_bwd(dg, g)
        dfc = (dy * silu_g.to(tl.float32)).to(dtype)
        sum_b_g += dg.to(tl.float32)
        sum_b_fc += dfc.to(tl.float32)
        tl.store(dg_ptrs, dg, mask=mask)
        tl.store(dfc_ptrs, dfc, mask=mask)
        dy_ptrs += BLOCK_SIZE_M * N
        g_ptrs += BLOCK_SIZE_M * N
        fc_ptrs += BLOCK_SIZE_M * N
        dg_ptrs += BLOCK_SIZE_M * N
        dfc_ptrs += BLOCK_SIZE_M * N

    tl.store(db_g_ptr + col_off, tl.sum(sum_b_g, 1), mask=col_off < N)
    tl.store(db_fc_ptr + col_off, tl.sum(sum_b_fc, 1), mask=col_off < N)


def _assert_ttir_text_reparseable(ttir, tmp_path, name):
    path = tmp_path / f"{name}.ttir.mlir"
    path.write_text(ttir)
    context = ir.context()
    ir.load_dialects(context)
    ascend_ir.load_dialects(context)
    reparsed = ir.parse_mlir_module(str(path), context)
    assert "tt.func" in str(reparsed)


def _make_fused_swiglu_ttir(options, block_m=256, block_n=32):
    """Compile the original-layout clone through TTIR without a device launch."""
    source = ASTSource(
        load_store_transpose_e2e_kernel,
        {
            "dy_ptr": "*bf16",
            "g_ptr": "*bf16",
            "fc_ptr": "*bf16",
            "dg_ptr": "*bf16",
            "dfc_ptr": "*bf16",
            "db_g_ptr": "*bf16",
            "db_fc_ptr": "*bf16",
            "M": "i32",
            "N": "i32",
        },
        {"BLOCK_SIZE_M": block_m, "BLOCK_SIZE_N": block_n},
    )
    context = ir.context()
    ir.load_dialects(context)
    ascend_ir.load_dialects(context)
    module = ast_to_ttir(
        load_store_transpose_e2e_kernel,
        source,
        context,
        options,
        {},
        {},
    )
    return make_ttir(module, {}, options)


def _require_npu():
    torch = pytest.importorskip("torch")
    pytest.importorskip("torch_npu", exc_type=ImportError)
    if not hasattr(torch, "npu") or not torch.npu.is_available():
        pytest.skip("Ascend NPU is unavailable")
    return torch


def test_load_store_transpose_default_mask_matches_native_bundle(monkeypatch, tmp_path):
    """Default 511 must retain the rule-1 rewrite selected by native mask 7."""
    monkeypatch.setenv("TRITON_DUMP_DIR", str(tmp_path / "dump"))
    native_only_options = NPUOptions(
        arch="Ascend910_95",
        enable_graph_optimize=True,
        graph_optimize_rule_mask=7,
        debug=True,
        sanitize_overflow=True,
    )
    default_options = NPUOptions(
        arch="Ascend910_95",
        enable_graph_optimize=True,
        debug=True,
        sanitize_overflow=True,
    )

    native_only_ttir = str(_make_fused_swiglu_ttir(native_only_options))
    default_ttir = str(_make_fused_swiglu_ttir(default_options))

    assert default_options.graph_optimize_rule_mask == 511
    assert default_ttir == native_only_ttir
    assert "tensor<256x32x!tt.ptr<bf16>>" in default_ttir
    assert "tensor<32x256x!tt.ptr<bf16>>" not in default_ttir
    assert "hacc.coalesce_factor" not in default_ttir
    assert "tt.indirect_load" not in default_ttir
    assert "tt.indirect_store" not in default_ttir
    _assert_ttir_text_reparseable(
        default_ttir,
        tmp_path,
        "default-native-graph-rule-bundle",
    )


def test_load_store_transpose_256x32_structure(monkeypatch, tmp_path):
    """Keep the original production tile as a TTIR-only structural gate."""
    monkeypatch.setenv("TRITON_DUMP_DIR", str(tmp_path / "dump"))
    ttirs = {}
    for enabled in (False, True):
        options = NPUOptions(
            arch="Ascend910_95",
            enable_graph_optimize=enabled,
            graph_optimize_rule_mask=1,
            debug=True,
            sanitize_overflow=True,
        )
        module = _make_fused_swiglu_ttir(options)
        ttir = str(module)
        _assert_ttir_text_reparseable(ttir, tmp_path, f"fused-swiglu-256x32-{int(enabled)}")
        (tmp_path / f"fused-swiglu-256x32-{int(enabled)}.ttir").write_text(ttir)
        ttirs[enabled] = ttir

    off_ttir = ttirs[False]
    on_ttir = ttirs[True]
    assert off_ttir.count("tt.assert") == on_ttir.count("tt.assert") == 20
    assert on_ttir.count("tt.auto_overflow_assert") == 20
    assert "tensor<32x256x!tt.ptr<bf16>>" in off_ttir
    assert "tensor<32x256x!tt.ptr<bf16>>" not in on_ttir
    assert "tensor<256x32x!tt.ptr<bf16>>" in on_ttir
    for old_guard_shape in (
            "tensor<1x256xi64>",
            "tensor<32x256xi64>",
            "tensor<1x256xi1>",
            "tensor<32x256xi1>",
    ):
        assert old_guard_shape not in on_ttir

    assert on_ttir.count("tt.expand_dims") == 2
    assert on_ttir.count("tt.splat") == 15
    assert on_ttir.count("arith.muli") == 8


def test_load_store_transpose_e2e(monkeypatch, tmp_path):
    """Run the rule-1 kernel on NPU with a CANN-friendly validation tile."""
    torch = _require_npu()
    torch_npu = pytest.importorskip("torch_npu")
    monkeypatch.setenv("TRITON_ALWAYS_COMPILE", "1")
    torch.manual_seed(20260724)

    m, n = 128, 64
    dtype = torch.bfloat16
    dy = torch.randn((m, n), dtype=dtype, device="npu")
    g = torch.randn((m, n), dtype=dtype, device="npu")
    fc = torch.randn((m, n), dtype=dtype, device="npu")

    def eager_reference():
        dy_f32 = dy.float()
        g_f32 = g.float()
        fc_f32 = fc.float()
        sigmoid = 1.0 / (1.0 + torch.exp(-g_f32))
        dg = (dy_f32 * fc_f32).to(dtype).float()
        dg = (dg * sigmoid * (1.0 + g_f32 * (1.0 - sigmoid))).to(dtype)
        silu_g = (g_f32 / (1.0 + torch.exp(-g_f32))).to(dtype)
        dfc = (dy_f32 * silu_g.float()).to(dtype)
        return (
            dg,
            dfc,
            dg.float().sum(dim=0).to(dtype),
            dfc.float().sum(dim=0).to(dtype),
        )

    expected = tuple(value.cpu() for value in eager_reference())
    summary = {
        "M": m,
        "N": n,
        "dtype": str(dtype),
        "rtol": 3e-2,
        "atol": 1e-1,
        "enable_graph_optimize": [False, True],
        "graph_optimize_rule_mask": 1,
        "debug": True,
        "sanitize_overflow": True,
        "torch_version": torch.__version__,
        "torch_npu_version": getattr(torch_npu, "__version__", "unknown"),
        "cann_version": torch_npu.npu.utils.get_cann_version(),
        "candidates": [],
    }

    def run_mode(enabled, block_m, block_n, attempt):
        load_store_transpose_e2e_kernel.device_caches.clear()
        dg = torch.empty((m, n), dtype=dtype, device="npu")
        dfc = torch.empty((m, n), dtype=dtype, device="npu")
        db_g = torch.empty((n, ), dtype=dtype, device="npu")
        db_fc = torch.empty((n, ), dtype=dtype, device="npu")
        compiled = load_store_transpose_e2e_kernel[(triton.cdiv(n, block_n), )](
            dy,
            g,
            fc,
            dg,
            dfc,
            db_g,
            db_fc,
            m,
            n,
            BLOCK_SIZE_M=block_m,
            BLOCK_SIZE_N=block_n,
            num_warps=4,
            num_stages=2,
            debug=True,
            sanitize_overflow=True,
            enable_graph_optimize=enabled,
            graph_optimize_rule_mask=1,
        )
        torch.npu.synchronize()
        ttir = compiled.asm["ttir"]
        name = f"load-store-transpose-{int(enabled)}-{block_m}x{block_n}-{attempt}"
        _assert_ttir_text_reparseable(ttir, tmp_path, name)
        (tmp_path / f"{name}.ttir").write_text(ttir)
        return tuple(value.cpu() for value in (dg, dfc, db_g, db_fc)), ttir

    def run_repeated_mode(enabled, block_m, block_n):
        repeated = []
        mode_ttir = []
        for attempt in range(2):
            result, ttir = run_mode(enabled, block_m, block_n, attempt)
            repeated.append(result)
            mode_ttir.append(ttir)
        for first, second in zip(repeated[0], repeated[1]):
            torch.testing.assert_close(first, second, rtol=0, atol=0)
        return repeated[-1], mode_ttir[-1]

    def write_summary():
        (tmp_path / "load-store-transpose-summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True))

    selected = None
    outputs = {}
    ttirs = {}
    last_runtime_error = None
    for block_m, block_n in ((64, 8), (32, 8)):
        candidate = {"BLOCK_SIZE_M": block_m, "BLOCK_SIZE_N": block_n}
        try:
            off_output, off_ttir = run_repeated_mode(False, block_m, block_n)
        except (RuntimeError, TritonError, subprocess.CalledProcessError) as error:
            candidate["status"] = "off-runtime-error"
            candidate["error_type"] = type(error).__name__
            candidate["error"] = str(error)
            summary["candidates"].append(candidate)
            last_runtime_error = error
            continue

        try:
            on_output, on_ttir = run_repeated_mode(True, block_m, block_n)
        except (RuntimeError, TritonError, subprocess.CalledProcessError) as error:
            candidate["status"] = "on-runtime-error"
            candidate["error_type"] = type(error).__name__
            candidate["error"] = str(error)
            summary["candidates"].append(candidate)
            write_summary()
            raise AssertionError("graph-optimize failed for a tiling that succeeds with it disabled") from error

        selected = (block_m, block_n)
        outputs = {False: off_output, True: on_output}
        ttirs = {False: off_ttir, True: on_ttir}
        candidate["status"] = "selected"
        summary["candidates"].append(candidate)
        break

    if selected is None:
        write_summary()
        raise RuntimeError("all fused_swiglu validation tilings failed") from last_runtime_error

    for index, (off, on, reference) in enumerate(zip(outputs[False], outputs[True], expected)):
        torch.testing.assert_close(on, off, rtol=3e-2, atol=1e-1)
        torch.testing.assert_close(on, reference, rtol=3e-2, atol=1e-1)
        summary[f"output_{index}_max_abs_on_off"] = float((on.float() - off.float()).abs().max().item())
        summary[f"output_{index}_max_abs_eager"] = float((on.float() - reference.float()).abs().max().item())

    block_m, block_n = selected
    on_ttir = ttirs[True]
    off_ttir = ttirs[False]
    assert on_ttir.count("tt.assert") == off_ttir.count("tt.assert")
    assert on_ttir.count("tt.assert") > 0
    assert "overflow detected for operation" in on_ttir
    assert on_ttir.count("tt.auto_overflow_assert") == on_ttir.count("tt.assert")
    old_pointer_type = "tensor<{}x{}x!tt.ptr".format(block_n, block_m)
    new_pointer_type = "tensor<{}x{}x!tt.ptr".format(block_m, block_n)
    assert old_pointer_type in off_ttir
    assert old_pointer_type not in on_ttir
    assert new_pointer_type in on_ttir
    assert "tensor<1x{}xi64>".format(block_m) not in on_ttir
    assert "tensor<{}x{}xi64>".format(block_n, block_m) not in on_ttir
    assert "tensor<1x{}xi1>".format(block_m) not in on_ttir
    assert "tensor<{}x{}xi1>".format(block_n, block_m) not in on_ttir

    summary["selected_tiling"] = {
        "BLOCK_SIZE_M": block_m,
        "BLOCK_SIZE_N": block_n,
        "num_warps": 4,
        "num_stages": 2,
    }
    summary["graph_optimize_assert_count"] = on_ttir.count("tt.assert")
    summary["device"] = (torch.npu.get_device_name(torch.npu.current_device())
                         if hasattr(torch.npu, "get_device_name") else "npu")
    write_summary()
