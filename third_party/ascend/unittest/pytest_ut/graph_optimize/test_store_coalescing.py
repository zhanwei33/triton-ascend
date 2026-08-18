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

import pytest
import triton
import triton.language as tl
from triton._C.libtriton import ir
from triton._C.libtriton.ascend import ir as ascend_ir

pytestmark = pytest.mark.backend("none")


@triton.jit
def store_coalescing_e2e_kernel(source_ptr, scale_ptr, output_ptr, HEAD_DIM: tl.constexpr):
    """Positive case: two adjacent bf16 stores require a 256-byte UB plan."""
    first_offsets = tl.arange(0, HEAD_DIM)
    second_offsets = tl.arange(HEAD_DIM, 2 * HEAD_DIM)
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
        stats[mode] = {
            "tt.store": ttir.count("tt.store"),
            "tensor.empty": ttir.count("tensor.empty"),
            "tensor.insert_slice": ttir.count("tensor.insert_slice"),
        }

    assert stats["off"] == {"tt.store": 2, "tensor.empty": 0, "tensor.insert_slice": 0}
    assert stats["cap0"] == stats["off"]
    assert stats["cap255"] == stats["off"]
    assert stats["cap256"] == {"tt.store": 1, "tensor.empty": 1, "tensor.insert_slice": 2}
