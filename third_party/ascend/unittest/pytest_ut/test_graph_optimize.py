# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
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
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

import os

os.environ["TORCH_DEVICE_BACKEND_AUTOLOAD"] = "0"

import pytest
import triton
import triton.language as tl
from triton._C.libtriton import ascend, ir
from triton._C.libtriton.ascend import ir as ascend_ir
from triton.backends.ascend.compiler import NPUOptions, make_ttir
from triton.compiler.code_generator import ast_to_ttir
from triton.compiler.compiler import ASTSource

pytestmark = pytest.mark.backend("none")


@triton.jit
def graph_optimize_kernel(x_ptr, y_ptr, BLOCK: tl.constexpr):
    offsets = tl.arange(0, BLOCK)
    value = tl.load(x_ptr + offsets)
    tl.store(y_ptr + offsets, value)


@triton.jit
def graph_optimize_legacy_memory_isolation_kernel(x_ptr, y_ptr, BLOCK: tl.constexpr):
    # This is deliberately a strided AddPtr load/store shape.  It is eligible
    # for the legacy T2L StridedLoadStoreRewrite, but must remain untouched by
    # the early generic graph-optimize pass even when its default mask carries
    # all seven rule identities.
    offsets = tl.arange(0, BLOCK) * 3
    value = tl.load(x_ptr + offsets)
    tl.store(y_ptr + offsets, value)


@triton.jit
def diagonal_shift_forward_kernel(x_ptr, y_ptr, BLOCK: tl.constexpr):
    # Shift a cumulative sum left by one the way the segmented-cumsum kernels
    # spell it: a sub-diagonal mask over a BLOCK x BLOCK matrix, reduced back
    # to one dimension.  DiagonalMaskRemoval must collapse it to a subtraction.
    offsets = tl.arange(0, BLOCK)
    x = tl.load(x_ptr + offsets)
    cumulative = tl.cumsum(x, axis=0)
    rows = tl.arange(0, BLOCK)[:, None]
    cols = tl.arange(0, BLOCK)[None, :]
    matrix = tl.where(rows == cols + 1, cumulative[None, :], 0.0)
    tl.store(y_ptr + offsets, tl.sum(matrix, axis=1))


@triton.jit
def diagonal_shift_reverse_kernel(x_ptr, y_ptr, BLOCK: tl.constexpr):
    # The mirrored form: a reverse scan shifted right by a super-diagonal mask.
    offsets = tl.arange(0, BLOCK)
    x = tl.load(x_ptr + offsets)
    cumulative = tl.cumsum(x, axis=0, reverse=True)
    rows = tl.arange(0, BLOCK)[:, None]
    cols = tl.arange(0, BLOCK)[None, :]
    matrix = tl.where(rows == cols - 1, cumulative[None, :], 0.0)
    tl.store(y_ptr + offsets, tl.sum(matrix, axis=1))


@triton.jit
def wrapped_tile_copy_kernel(src_ptr, dst_ptr, n, BLOCK_K: tl.constexpr, BLOCK_N: tl.constexpr):
    # The fused-MoE weight-tile shape: the column offset wraps on a runtime
    # dimension, and the store mask already discards the lanes past it.
    # ConvertModuloToMask must drop the wrap and mask the load instead.
    pid = tl.program_id(0)
    offs_n = pid * BLOCK_N + tl.arange(0, BLOCK_N)
    offs_k = tl.arange(0, BLOCK_K)
    wrapped = offs_n % n
    tile = tl.load(src_ptr + (offs_k[:, None] * n + wrapped[None, :]))
    tl.store(
        dst_ptr + (offs_k[:, None] * n + offs_n[None, :]),
        tile,
        mask=offs_n[None, :] < n,
    )


@triton.jit
def constexpr_wrapped_tile_copy_kernel(src_ptr, dst_ptr, N: tl.constexpr, BLOCK_K: tl.constexpr, BLOCK_N: tl.constexpr):
    # Same shape with a compile-time bound.  TritonToStructured can keep this
    # wrap and re-express it as a strided access, which is exactly equivalent,
    # so ConvertModuloToMask must leave it alone.
    pid = tl.program_id(0)
    offs_n = pid * BLOCK_N + tl.arange(0, BLOCK_N)
    offs_k = tl.arange(0, BLOCK_K)
    wrapped = offs_n % N
    tile = tl.load(src_ptr + (offs_k[:, None] * N + wrapped[None, :]))
    tl.store(
        dst_ptr + (offs_k[:, None] * N + offs_n[None, :]),
        tile,
        mask=offs_n[None, :] < N,
    )


@triton.jit
def overflow_assert_provenance_kernel(output_ptr, n, BLOCK: tl.constexpr):
    offsets = tl.arange(0, BLOCK)
    checked_offset = offsets * n
    # Deliberately reuse the automatic message.  This must remain a user
    # assertion without the private auto-overflow provenance marker.
    tl.device_assert(
        checked_offset >= 0,
        "int32 overflow detected for operation mul",
    )
    tl.store(output_ptr + checked_offset, 0.0)


def make_ast_ttir(options):
    source = ASTSource(
        graph_optimize_kernel,
        {"x_ptr": "*fp32", "y_ptr": "*fp32"},
        {"BLOCK": 16},
    )
    context = ir.context()
    ir.load_dialects(context)
    ascend_ir.load_dialects(context)
    return ast_to_ttir(graph_optimize_kernel, source, context, options, {}, {})


def make_legacy_memory_isolation_ast_ttir(options):
    source = ASTSource(
        graph_optimize_legacy_memory_isolation_kernel,
        {"x_ptr": "*fp32", "y_ptr": "*fp32"},
        {"BLOCK": 16},
    )
    context = ir.context()
    ir.load_dialects(context)
    ascend_ir.load_dialects(context)
    return ast_to_ttir(
        graph_optimize_legacy_memory_isolation_kernel,
        source,
        context,
        options,
        {},
        {},
    )


def assert_ttir_text_reparseable(ttir, tmp_path, name):
    path = tmp_path / f"{name}.ttir.mlir"
    path.write_text(ttir)
    context = ir.context()
    ir.load_dialects(context)
    ascend_ir.load_dialects(context)
    reparsed = ir.parse_mlir_module(str(path), context)
    assert "tt.func" in str(reparsed)


def assert_reparseable(module, tmp_path, name):
    assert_ttir_text_reparseable(str(module), tmp_path, name)


def test_graph_optimize_pass_accepts_zero_rule_mask(tmp_path):
    module = make_ast_ttir(NPUOptions(arch="Ascend910_95"))
    pm = ir.pass_manager(module.context)
    ascend.passes.ttir.add_graph_optimize(pm, rule_mask=0)
    pm.run(module, "")

    assert "tt.func" in str(module)
    assert_reparseable(module, tmp_path, "zero-rule-mask")


def test_default_generic_graph_mask_excludes_legacy_memory_compatibility(tmp_path, ):
    """The 8/16/32/64 identities are enabled by default, not generic rules.

    The generic GraphOptimizePass runs at early TTIR.  Row, Axis, Chunk, and
    StridedLoadStoreRewrite retain their original compatibility-pass slots, so
    this strided memory shape must not acquire either coalescing metadata or
    an indirect-memory op merely because the default mask is 511.
    """
    default_options = NPUOptions(arch="Ascend910_95", enable_graph_optimize=True)
    native_only_options = NPUOptions(
        arch="Ascend910_95",
        enable_graph_optimize=True,
        graph_optimize_rule_mask=7,
    )
    assert default_options.graph_optimize_rule_mask == 511

    default_result = make_ttir(
        make_legacy_memory_isolation_ast_ttir(default_options),
        {},
        default_options,
    )
    native_only_result = make_ttir(
        make_legacy_memory_isolation_ast_ttir(native_only_options),
        {},
        native_only_options,
    )
    text = str(default_result)

    assert "hacc.coalesce_factor" not in text
    assert "hacc.coalesce_axis" not in text
    assert "hacc.coalesce_grid_ceil_div" not in text
    assert "tt.indirect_load" not in text
    assert "tt.indirect_store" not in text
    # The native graph-rule bundle exercised by this shape is
    # LoadStoreTranspose | TransposePointwiseReorder | StoreCoalescing (1|2|4).
    # Adding the four compatibility identities, DiagonalMaskRemoval and
    # ConvertModuloToMask to reach 511 must be observationally inert in
    # make_ttir(): this input carries neither a diagonal-select-reduce pattern
    # nor a wrapped tile address.
    assert text == str(native_only_result)
    assert_reparseable(default_result, tmp_path, "generic-legacy-memory-isolation")


def test_make_ttir_supports_graph_optimize_toggle(monkeypatch, tmp_path):
    monkeypatch.setenv("TRITON_ALWAYS_COMPILE", "1")

    for enabled in (False, True):
        options = NPUOptions(
            arch="Ascend910_95",
            enable_graph_optimize=enabled,
        )
        for attempt in range(2):
            module = make_ast_ttir(options)
            result = make_ttir(module, {}, options)

            assert result is module
            assert "tt.func" in str(result)
            assert_reparseable(
                result,
                tmp_path,
                f"graph-optimize-{int(enabled)}-{attempt}",
            )


def test_auto_overflow_assert_has_frontend_provenance_marker():
    source = ASTSource(
        overflow_assert_provenance_kernel,
        {"output_ptr": "*fp32", "n": "i32"},
        {"BLOCK": 16},
    )
    context = ir.context()
    ir.load_dialects(context)
    ascend_ir.load_dialects(context)
    module = ast_to_ttir(
        overflow_assert_provenance_kernel,
        source,
        context,
        NPUOptions(
            arch="Ascend910_95",
            debug=True,
            sanitize_overflow=True,
        ),
        {},
        {},
    )
    overflow_asserts = [
        line for line in str(module).splitlines() if '"int32 overflow detected for operation mul"' in line
    ]
    assert any("tt.auto_overflow_assert" in line for line in overflow_asserts)
    assert any("tt.auto_overflow_assert" not in line for line in overflow_asserts)


def _require_npu():
    torch = pytest.importorskip("torch")
    pytest.importorskip("torch_npu", exc_type=ImportError)
    if not hasattr(torch, "npu") or not torch.npu.is_available():
        pytest.skip("Ascend NPU is unavailable")
    return torch


def test_graph_optimize_numerical_equivalence(monkeypatch, tmp_path):
    """Exercise the complete JIT pipeline on hardware with cache bypassed."""
    torch = _require_npu()
    monkeypatch.setenv("TRITON_ALWAYS_COMPILE", "1")
    source = torch.arange(16, dtype=torch.float32, device="npu")
    outputs = {}

    for enabled in (False, True):
        repeated_outputs = []
        for attempt in range(2):
            # JITFunction has its own in-memory cache, so clear it before each
            # launch to ensure TRITON_ALWAYS_COMPILE reaches triton.compile().
            graph_optimize_kernel.device_caches.clear()
            output = torch.empty_like(source)
            compiled = graph_optimize_kernel[(1, )](
                source,
                output,
                BLOCK=16,
                enable_graph_optimize=enabled,
            )
            torch.npu.synchronize()
            assert_ttir_text_reparseable(
                compiled.asm["ttir"],
                tmp_path,
                f"hardware-{int(enabled)}-{attempt}",
            )
            repeated_outputs.append(output.cpu())

        torch.testing.assert_close(repeated_outputs[0], source.cpu())
        torch.testing.assert_close(repeated_outputs[1], repeated_outputs[0])
        outputs[enabled] = repeated_outputs[-1]

    torch.testing.assert_close(outputs[True], outputs[False])


def make_diagonal_shift_ttir(kernel, options, block=16):
    source = ASTSource(
        kernel,
        {"x_ptr": "*fp32", "y_ptr": "*fp32"},
        {"BLOCK": block},
    )
    context = ir.context()
    ir.load_dialects(context)
    ascend_ir.load_dialects(context)
    module = ast_to_ttir(kernel, source, context, options, {}, {})
    return str(make_ttir(module, {}, options))


@pytest.mark.parametrize(
    "kernel",
    (diagonal_shift_forward_kernel, diagonal_shift_reverse_kernel),
    ids=("forward-left-shift", "reverse-right-shift"),
)
def test_diagonal_mask_removal_collapses_quadratic_shift(kernel):
    """The 16x16 intermediate and its reduction give way to one subtraction."""
    options = NPUOptions(arch="Ascend910B1", enable_graph_optimize=True)

    ttir = make_diagonal_shift_ttir(kernel, options)

    assert "arith.subf" in ttir
    assert "tt.reduce" not in ttir
    assert "tt.scan" in ttir
    # The quadratic intermediate is what the rule exists to remove.
    assert "tensor<16x16xf32>" not in ttir
    assert "tensor<16x16xi1>" not in ttir


@pytest.mark.parametrize(
    "kernel",
    (diagonal_shift_forward_kernel, diagonal_shift_reverse_kernel),
    ids=("forward-left-shift", "reverse-right-shift"),
)
def test_diagonal_mask_removal_is_gated_by_its_rule_bit(kernel):
    """Mask 127 is every other identity, so the pattern must survive intact."""
    options = NPUOptions(
        arch="Ascend910B1",
        enable_graph_optimize=True,
        graph_optimize_rule_mask=127,
    )

    ttir = make_diagonal_shift_ttir(kernel, options)

    assert "tt.reduce" in ttir
    assert "tensor<16x16xf32>" in ttir
    assert "arith.subf" not in ttir


def test_diagonal_mask_removal_applies_without_simt_route():
    """The rewrite is compute logic, so it must not depend on force_simt_only.

    Its predecessor lived behind the 910_95 plus force-SIMT gate of
    processStridedLoadStoreRewriteOperations and therefore never ran on other
    targets.
    """
    for force_simt_only in (False, True):
        options = NPUOptions(
            arch="Ascend910B1",
            enable_graph_optimize=True,
            force_simt_only=force_simt_only,
        )

        ttir = make_diagonal_shift_ttir(diagonal_shift_forward_kernel, options)

        assert "arith.subf" in ttir
        assert "tt.reduce" not in ttir


def test_diagonal_mask_removal_numerical_equivalence(monkeypatch, tmp_path):
    """Compare against the unrewritten kernel on hardware.

    The float rewrite is not bit-exact, since scan[i] - x[i] only recovers
    scan[i - 1] up to rounding, so this asserts closeness rather than equality.
    """
    torch = _require_npu()
    monkeypatch.setenv("TRITON_ALWAYS_COMPILE", "1")
    source = torch.rand(16, dtype=torch.float32, device="npu")
    expected = torch.cat([
        torch.zeros(1, dtype=torch.float32),
        torch.cumsum(source.cpu(), dim=0)[:-1],
    ])

    outputs = {}
    for rule_mask in (127, 511):
        diagonal_shift_forward_kernel.device_caches.clear()
        output = torch.empty_like(source)
        compiled = diagonal_shift_forward_kernel[(1, )](
            source,
            output,
            BLOCK=16,
            graph_optimize_rule_mask=rule_mask,
        )
        torch.npu.synchronize()
        assert_ttir_text_reparseable(
            compiled.asm["ttir"],
            tmp_path,
            f"diagonal-{rule_mask}",
        )
        outputs[rule_mask] = output.cpu()

    assert "arith.subf" in compiled.asm["ttir"]
    torch.testing.assert_close(outputs[127], expected, rtol=1e-5, atol=1e-6)
    torch.testing.assert_close(outputs[511], outputs[127], rtol=1e-5, atol=1e-6)


def make_wrapped_tile_ttir(options, block_k=8, block_n=16, bound=None):
    kernel = wrapped_tile_copy_kernel if bound is None else constexpr_wrapped_tile_copy_kernel
    signature = {"src_ptr": "*fp32", "dst_ptr": "*fp32"}
    constants = {"BLOCK_K": block_k, "BLOCK_N": block_n}
    if bound is None:
        signature["n"] = "i32"
    else:
        constants["N"] = bound

    source = ASTSource(kernel, signature, constants)
    context = ir.context()
    ir.load_dialects(context)
    ascend_ir.load_dialects(context)
    module = ast_to_ttir(kernel, source, context, options, {}, {})
    return str(make_ttir(module, {}, options))


def test_convert_modulo_to_mask_linearizes_wrapped_tile():
    """The wrap gives way to a linear offset plus a boundary mask on the load.

    The point of the rewrite is the address form: TritonToStructured can turn a
    linear masked access into one contiguous transfer, while it has no way to
    keep a wrapped address contiguous.
    """
    options = NPUOptions(arch="Ascend910B1", enable_graph_optimize=True)

    ttir = make_wrapped_tile_ttir(options)

    assert "arith.remsi" not in ttir
    assert "arith.cmpi slt" in ttir
    # The load carries a mask and a zero fill it did not have before.
    assert "tt.load" in ttir
    assert "arith.constant dense<0.000000e+00> : tensor<8x16xf32>" in ttir


def test_convert_modulo_to_mask_is_gated_by_its_rule_bit():
    """Mask 255 is every other identity, so the wrap must survive intact."""
    options = NPUOptions(
        arch="Ascend910B1",
        enable_graph_optimize=True,
        graph_optimize_rule_mask=255,
    )

    ttir = make_wrapped_tile_ttir(options)

    assert "arith.remsi" in ttir


def test_convert_modulo_to_mask_leaves_compile_time_bounds_alone():
    """A constant divisor belongs to TritonToStructured, which stays equivalent.

    visitOperandRem re-expresses such a wrap as a strided access instead of
    discarding it, so claiming this candidate would trade an exact rewrite for
    an approximate one.
    """
    options = NPUOptions(arch="Ascend910B1", enable_graph_optimize=True)

    ttir = make_wrapped_tile_ttir(options, bound=20)

    assert "arith.remsi" in ttir


def test_convert_modulo_to_mask_applies_without_simt_route():
    """The rewrite is address logic, so it must not depend on force_simt_only."""
    for force_simt_only in (False, True):
        options = NPUOptions(
            arch="Ascend910B1",
            enable_graph_optimize=True,
            force_simt_only=force_simt_only,
        )

        ttir = make_wrapped_tile_ttir(options)

        assert "arith.remsi" not in ttir


def test_convert_modulo_to_mask_numerical_equivalence(monkeypatch, tmp_path):
    """The lanes the store keeps must read exactly what the wrap gave them.

    A bound that is not a multiple of the tile makes the last program a boundary
    tile, which is the only place the wrap ever mattered.
    """
    torch = _require_npu()
    monkeypatch.setenv("TRITON_ALWAYS_COMPILE", "1")
    block_k = 8
    block_n = 16
    bound = 20
    # The wrapped lanes now address past the last row instead of folding back
    # into the tensor, so the allocation carries one tile of slack.  Whether the
    # lowered transfer actually stops at the mask is a TritonToStructured
    # property, asserted structurally by the tests above.
    source = torch.rand(block_k * bound + block_n, dtype=torch.float32, device="npu")
    expected = source[:block_k * bound].cpu()

    outputs = {}
    for rule_mask in (255, 511):
        wrapped_tile_copy_kernel.device_caches.clear()
        output = torch.zeros(block_k * bound, dtype=torch.float32, device="npu")
        grid = ((bound + block_n - 1) // block_n, )
        compiled = wrapped_tile_copy_kernel[grid](
            source,
            output,
            bound,
            BLOCK_K=block_k,
            BLOCK_N=block_n,
            graph_optimize_rule_mask=rule_mask,
        )
        torch.npu.synchronize()
        assert_ttir_text_reparseable(
            compiled.asm["ttir"],
            tmp_path,
            f"wrapped-tile-{rule_mask}",
        )
        outputs[rule_mask] = output.cpu()

    assert "arith.remsi" not in compiled.asm["ttir"]
    torch.testing.assert_close(outputs[255], expected, rtol=0, atol=0)
    torch.testing.assert_close(outputs[511], outputs[255], rtol=0, atol=0)


def test_graph_optimize_options_contribute_to_npu_hash():
    assert (NPUOptions(enable_graph_optimize=False).hash() != NPUOptions(enable_graph_optimize=True).hash())
