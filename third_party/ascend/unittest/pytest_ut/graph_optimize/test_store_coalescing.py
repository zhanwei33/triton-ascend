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
"""NPU end-to-end regression for GraphOptimize StoreCoalescing."""

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
def store_coalescing_e2e_kernel(source_ptr, scale_ptr, output_ptr, HEAD_DIM: tl.constexpr):
    """Positive case: two adjacent bf16 stores require a 256-byte UB plan."""
    program_base = tl.program_id(axis=0) * (2 * HEAD_DIM)
    first_offsets = program_base + tl.arange(0, HEAD_DIM)
    second_offsets = program_base + tl.arange(HEAD_DIM, 2 * HEAD_DIM)
    scale = tl.load(scale_ptr)
    first = (tl.load(source_ptr + first_offsets).to(tl.float32) * scale).to(tl.bfloat16)
    second = (tl.load(source_ptr + second_offsets).to(tl.float32) * scale).to(tl.bfloat16)
    tl.store(output_ptr + first_offsets, first)
    tl.store(output_ptr + second_offsets, second)


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


def _store_stats(ttir):
    return {
        "tt.store": ttir.count("tt.store"),
        "tensor.empty": ttir.count("tensor.empty"),
        "tensor.insert_slice": ttir.count("tensor.insert_slice"),
    }


def _assert_ttir_text_reparseable(ttir, tmp_path, name):
    path = tmp_path / f"{name}.ttir.mlir"
    path.write_text(ttir)
    context = ir.context()
    ir.load_dialects(context)
    ascend_ir.load_dialects(context)
    reparsed = ir.parse_mlir_module(str(path), context)
    assert "tt.func" in str(reparsed)


def test_store_coalescing_e2e(monkeypatch, tmp_path):
    """Graph optimization must safely coalesce a K/V-style pair of bf16 stores."""
    torch = _require_npu()
    monkeypatch.setenv("TRITON_ALWAYS_COMPILE", "1")
    head_dim = 64
    sentinel = -17.0
    source = torch.arange(1, 2 * head_dim + 1, dtype=torch.int32, device="npu").to(torch.uint8)
    scale = torch.tensor([0.125], dtype=torch.float32, device="npu")
    expected = (source.cpu().to(torch.float32) * 0.125).to(torch.bfloat16)
    modes = {
        "off": {"enable_graph_optimize": False},
        "cap0": {
            "enable_graph_optimize": True,
            "graph_optimize_ub_capacity_bytes": 0,
        },
        "cap255": {
            "enable_graph_optimize": True,
            "graph_optimize_ub_capacity_bytes": 255,
        },
        "cap256": {
            "enable_graph_optimize": True,
            "graph_optimize_ub_capacity_bytes": 256,
        },
    }
    stats = {}

    for mode, graph_options in modes.items():
        store_coalescing_e2e_kernel.device_caches.clear()
        output = torch.full((4 * head_dim, ), sentinel, dtype=torch.bfloat16, device="npu")
        compiled = store_coalescing_e2e_kernel[(1, )](
            source,
            scale,
            output[2 * head_dim:],
            HEAD_DIM=head_dim,
            **graph_options,
        )
        torch.npu.synchronize()
        torch.testing.assert_close(output[2 * head_dim:].cpu(), expected, rtol=0, atol=0)
        torch.testing.assert_close(
            output[:2 * head_dim].cpu(),
            torch.full((2 * head_dim, ), sentinel, dtype=torch.bfloat16),
            rtol=0,
            atol=0,
        )
        ttir = compiled.asm["ttir"]
        _assert_ttir_text_reparseable(ttir, tmp_path, f"store-coalescing-e2e-{mode}")
        stats[mode] = _store_stats(ttir)

    assert stats["off"] == {"tt.store": 2, "tensor.empty": 0, "tensor.insert_slice": 0}
    assert stats["cap0"] == stats["off"]
    assert stats["cap255"] == stats["off"]
    assert stats["cap256"] == {"tt.store": 1, "tensor.empty": 1, "tensor.insert_slice": 2}


def test_store_coalescing_profiler(monkeypatch, tmp_path):
    """Profile scalable K/V-style stores with the rule disabled/enabled by UB size."""
    torch = _require_npu()
    monkeypatch.delenv("TRITON_ALWAYS_COMPILE", raising=False)
    head_dim = 64
    batch = 65536
    elements = batch * 2 * head_dim
    torch.manual_seed(20260820)
    source = torch.randint(0, 128, (elements, ), dtype=torch.int32, device="npu").to(torch.uint8)
    scale = torch.tensor([0.125], dtype=torch.float32, device="npu")
    modes = {
        "cap0": {
            "enable_graph_optimize": True,
            "graph_optimize_ub_capacity_bytes": 0,
        },
        "cap256": {
            "enable_graph_optimize": True,
            "graph_optimize_ub_capacity_bytes": 256,
        },
    }
    outputs = {}
    ttirs = {}
    launchers = {}
    target_names = {}

    store_coalescing_e2e_kernel.device_caches.clear()
    for mode, graph_options in modes.items():
        output = torch.empty((elements, ), dtype=torch.bfloat16, device="npu")
        compiled = store_coalescing_e2e_kernel[(batch, )](
            source,
            scale,
            output,
            HEAD_DIM=head_dim,
            **graph_options,
        )
        torch.npu.synchronize()
        outputs[mode] = output.cpu()
        ttirs[mode] = compiled.asm["ttir"]
        target_names[mode] = compiled.metadata.name

        def launch(graph_options=graph_options, output=output):
            return store_coalescing_e2e_kernel[(batch, )](
                source,
                scale,
                output,
                HEAD_DIM=head_dim,
                **graph_options,
            )

        launchers[mode] = launch

    torch.testing.assert_close(outputs["cap256"], outputs["cap0"], rtol=0, atol=0)
    assert _store_stats(ttirs["cap0"]) == {"tt.store": 2, "tensor.empty": 0, "tensor.insert_slice": 0}
    assert _store_stats(ttirs["cap256"]) == {"tt.store": 1, "tensor.empty": 1, "tensor.insert_slice": 2}

    profiles = {mode: _profile_mode(tmp_path, mode, launchers[mode], target_names[mode]) for mode in modes}
    speedup = profiles["cap0"]["latency_ms"] / profiles["cap256"]["latency_ms"]
    summary = {
        "workload": {"batch": batch, "HEAD_DIM": head_dim},
        "baseline": profiles["cap0"],
        "optimized": profiles["cap256"],
        "speedup": speedup,
    }
    summary_path = tmp_path / "store-coalescing-profiler-summary.json"
    summary_path.write_text(json.dumps(summary, indent=2, sort_keys=True))
    print(f"StoreCoalescing profiler summary: {summary_path}")
    assert speedup > 0.0
