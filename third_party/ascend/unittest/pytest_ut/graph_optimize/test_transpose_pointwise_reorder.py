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
"""NPU end-to-end regression for GraphOptimize TransposePointwiseReorder."""

import json
import math

import pytest
import triton
import triton.language as tl
from triton._C.libtriton import ir
from triton._C.libtriton.ascend import ir as ascend_ir
from triton.backends.ascend.testing import do_bench_npu

pytestmark = pytest.mark.backend("none")


@triton.jit
def transpose_pointwise_reorder_e2e_kernel(a_ptr, b_ptr, output_ptr, BLOCK: tl.constexpr):
    """Positive case: tt.trans -> fp32-to-fp16 -> tt.dot."""
    tile = tl.program_id(axis=0)
    tile_base = tile * BLOCK * BLOCK
    row = tl.arange(0, BLOCK)[:, None]
    col = tl.arange(0, BLOCK)[None, :]
    offsets = tile_base + row * BLOCK + col
    a = tl.load(a_ptr + offsets)
    b = tl.load(b_ptr + offsets)
    transposed_a = tl.trans(a)
    dot = tl.dot(transposed_a.to(tl.float16), b)
    tl.store(output_ptr + offsets, dot)


def _require_npu():
    torch = pytest.importorskip("torch")
    pytest.importorskip("torch_npu", exc_type=ImportError)
    if not hasattr(torch, "npu") or not torch.npu.is_available():
        pytest.skip("Ascend NPU is unavailable")
    return torch


def _profile_mode(tmp_path, mode, launch, target_kernel_name):
    """Collect one device-side kernel latency and retain its profiler trace."""
    assert target_kernel_name
    warmup, active = 5, 30
    profile_dir = tmp_path / "profiler" / mode
    profile_dir.mkdir(parents=True, exist_ok=True)
    latency_ms = float(
        do_bench_npu(
            launch,
            warmup=warmup,
            active=active,
            prof_dir=str(profile_dir),
            keep_res=True,
            target_kernel_name=target_kernel_name,
        ))
    assert math.isfinite(latency_ms) and latency_ms > 0.0
    csv_files = sorted(profile_dir.rglob("kernel_details.csv"))
    assert len(csv_files) == 1
    return {
        "latency_ms": latency_ms,
        "target_kernel_name": target_kernel_name,
        "warmup": warmup,
        "active": active,
        "kernel_details_csv": str(csv_files[0]),
    }


def _assert_ttir_text_reparseable(ttir, tmp_path, name):
    path = tmp_path / f"{name}.ttir.mlir"
    path.write_text(ttir)
    context = ir.context()
    ir.load_dialects(context)
    ascend_ir.load_dialects(context)
    reparsed = ir.parse_mlir_module(str(path), context)
    assert "tt.func" in str(reparsed)


def _assert_transpose_before_dot(ttir, *, trans_before_cast):
    """Verify the runtime kernel's single trans/cast/dot chain, not just output."""
    trans = ttir.index("tt.trans")
    cast = ttir.index("arith.truncf")
    dot = ttir.index("tt.dot")
    if trans_before_cast:
        assert trans < cast < dot
    else:
        assert cast < trans < dot


def test_transpose_pointwise_reorder_e2e(monkeypatch, tmp_path):
    """Graph optimization must rewrite a launched dot kernel and preserve its result.

    The off/on matrix checks that graph optimization moved the transpose
    through the type cast and compares actual NPU output. The structural
    assertion prevents a numerical pass without the expected rewrite from
    being accepted as coverage.
    """
    torch = _require_npu()
    monkeypatch.setenv("TRITON_ALWAYS_COMPILE", "1")
    block = 16
    torch.manual_seed(20260817)
    a = torch.randn((block, block), dtype=torch.float32, device="npu")
    b = torch.randn((block, block), dtype=torch.float16, device="npu")
    modes = {
        "off": {"enable_graph_optimize": False},
        "on": {"enable_graph_optimize": True},
    }
    outputs = {}
    ttirs = {}

    for mode, graph_options in modes.items():
        transpose_pointwise_reorder_e2e_kernel.device_caches.clear()
        output = torch.empty((block, block), dtype=torch.float32, device="npu")
        compiled = transpose_pointwise_reorder_e2e_kernel[(1, )](
            a,
            b,
            output,
            BLOCK=block,
            **graph_options,
        )
        torch.npu.synchronize()
        assert torch.isfinite(output).all().item()
        ttir = compiled.asm["ttir"]
        _assert_ttir_text_reparseable(ttir, tmp_path, f"transpose-pointwise-e2e-{mode}")
        outputs[mode] = output.cpu()
        ttirs[mode] = ttir

    torch.testing.assert_close(outputs["on"], outputs["off"], rtol=0, atol=0)
    _assert_transpose_before_dot(ttirs["off"], trans_before_cast=True)
    _assert_transpose_before_dot(ttirs["on"], trans_before_cast=False)


def test_transpose_pointwise_reorder_profiler(monkeypatch, tmp_path):
    """Profile independent transpose/cast/dot tiles with graph optimization off/on."""
    torch = _require_npu()
    monkeypatch.delenv("TRITON_ALWAYS_COMPILE", raising=False)
    block = 16
    batch = 65536
    elements = batch * block * block
    torch.manual_seed(20260819)
    a = torch.randn((elements, ), dtype=torch.float32, device="npu")
    b = torch.randn((elements, ), dtype=torch.float16, device="npu")
    modes = {
        "off": {"enable_graph_optimize": False},
        "on": {"enable_graph_optimize": True},
    }
    outputs = {}
    ttirs = {}
    launchers = {}
    target_names = {}

    transpose_pointwise_reorder_e2e_kernel.device_caches.clear()
    for mode, graph_options in modes.items():
        output = torch.empty((elements, ), dtype=torch.float32, device="npu")
        compiled = transpose_pointwise_reorder_e2e_kernel[(batch, )](
            a,
            b,
            output,
            BLOCK=block,
            **graph_options,
        )
        torch.npu.synchronize()
        outputs[mode] = output.cpu()
        ttirs[mode] = compiled.asm["ttir"]
        target_names[mode] = compiled.metadata.name

        def launch(graph_options=graph_options, output=output):
            return transpose_pointwise_reorder_e2e_kernel[(batch, )](
                a,
                b,
                output,
                BLOCK=block,
                **graph_options,
            )

        launchers[mode] = launch

    torch.testing.assert_close(outputs["on"], outputs["off"], rtol=0, atol=0)
    _assert_transpose_before_dot(ttirs["off"], trans_before_cast=True)
    _assert_transpose_before_dot(ttirs["on"], trans_before_cast=False)

    profiles = {mode: _profile_mode(tmp_path, mode, launchers[mode], target_names[mode]) for mode in modes}
    speedup = profiles["off"]["latency_ms"] / profiles["on"]["latency_ms"]
    summary = {
        "workload": {"batch": batch, "BLOCK": block},
        "baseline": profiles["off"],
        "optimized": profiles["on"],
        "speedup": speedup,
    }
    summary_path = tmp_path / "transpose-pointwise-reorder-profiler-summary.json"
    summary_path.write_text(json.dumps(summary, indent=2, sort_keys=True))
    print(f"TransposePointwiseReorder profiler summary: {summary_path}")
    assert speedup > 0.0
