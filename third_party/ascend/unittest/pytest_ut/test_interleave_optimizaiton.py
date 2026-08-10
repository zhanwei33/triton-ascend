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
import triton
import triton.language as tl

import torch
import torch_npu
import pytest
import test_common


def torch_interleave_load(q, k, head_dim_half, bias):
    d_indices = torch.arange(0, head_dim_half)
    k[d_indices * 2 + bias] = q[d_indices * 2 + bias]
    k[d_indices * 2 + 1 + bias] = -q[d_indices * 2 + 1 + bias]
    return k


def torch_interleave_load_with_mask(q, k, head_dim_half, bias, numel):
    d_indices = torch.arange(0, min(head_dim_half, numel))
    k[d_indices * 2 + bias] = q[d_indices * 2 + bias]
    k[d_indices * 2 + 1 + bias] = -q[d_indices * 2 + 1 + bias]
    return k


def torch_interleave_loadstore_with_mask(q, head_dim_half, bias, numel):
    d_indices = torch.arange(0, min(head_dim_half, numel))
    # it's unneccessary since we store it back without edit: q[d_indices * 2 + bias] = q[d_indices * 2 + bias]
    q[d_indices * 2 + 1 + bias] = -q[d_indices * 2 + 1 + bias]
    return q


@triton.jit
def triton_interleave_load(q_ptr, k_ptr, head_dim_half: tl.constexpr, bias: tl.constexpr):
    d_indices = tl.program_id(0) + tl.arange(0, head_dim_half)
    q_base = q_ptr + bias
    q_real = tl.load(q_base + d_indices * 2)
    q_imag = tl.load(q_base + d_indices * 2 + 1)
    new_q_real = q_real
    new_q_imag = -q_imag
    tl.store(k_ptr + d_indices * 2 + bias, new_q_real)
    tl.store(k_ptr + d_indices * 2 + 1 + bias, new_q_imag)


@triton.jit
def triton_interleave_load_runtime_bias(q_ptr, k_ptr, bias, head_dim_half: tl.constexpr):
    d_indices = tl.program_id(0) + tl.arange(0, head_dim_half)
    q_base = q_ptr + bias
    q_real = tl.load(q_base + d_indices * 2)
    q_imag = tl.load(q_base + d_indices * 2 + 1)
    tl.store(k_ptr + d_indices, q_real)
    tl.store(k_ptr + d_indices + head_dim_half, -q_imag)


@triton.jit
def triton_deinterleave_same_lane_static_load(q_ptr, out_ptr, head_dim_half: tl.constexpr):
    d_indices = tl.program_id(0) + tl.arange(0, head_dim_half)
    first = tl.load(q_ptr + d_indices * 2 + 64)
    second = tl.load(q_ptr + d_indices * 2 + 66)
    tl.store(out_ptr + d_indices, first)
    tl.store(out_ptr + d_indices + head_dim_half, second)


@triton.jit
def triton_deinterleave_complementary_static_load(q_ptr, out_ptr, head_dim_half: tl.constexpr):
    d_indices = tl.program_id(0) + tl.arange(0, head_dim_half)
    even_lane = tl.load(q_ptr + d_indices * 2 + 64)
    odd_lane = tl.load(q_ptr + d_indices * 2 + 65)
    tl.store(out_ptr + d_indices, even_lane)
    tl.store(out_ptr + d_indices + head_dim_half, odd_lane)


@triton.jit
def triton_interleave_load_with_mask(q_ptr, k_ptr, head_dim_half: tl.constexpr, bias: tl.constexpr,
                                     numel: tl.constexpr):
    d_indices = tl.program_id(0) + tl.arange(0, head_dim_half)
    mask = d_indices < numel
    q_base = q_ptr + bias
    q_real = tl.load(q_base + d_indices * 2, mask)
    q_imag = tl.load(q_base + d_indices * 2 + 1, mask)
    new_q_real = q_real
    new_q_imag = -q_imag
    tl.store(k_ptr + d_indices * 2 + bias, new_q_real, mask)
    tl.store(k_ptr + d_indices * 2 + 1 + bias, new_q_imag, mask)


# when load and store are on the same pointer, sometimes we can only optimize the store with mask
@triton.jit
def triton_interleave_loadstore_with_mask(q_ptr, head_dim_half: tl.constexpr, bias: tl.constexpr, numel: tl.constexpr):
    d_indices = tl.arange(0, head_dim_half)
    mask = d_indices < numel
    q_base = q_ptr + bias
    q_real = tl.load(q_base + d_indices * 2, mask)
    q_imag = tl.load(q_base + d_indices * 2 + 1, mask)
    new_q_real = q_real
    new_q_imag = -q_imag
    tl.store(q_base + d_indices * 2, new_q_real, mask)
    tl.store(q_base + d_indices * 2 + 1, new_q_imag, mask)


@pytest.mark.parametrize('para_type,data_type,head_dim_half,bias', [
    ['float32', torch.float32, 16, 4],
    ['float32', torch.float32, 16, 64],
    ['float32', torch.float32, 16, 65],
])
def test_interleave(para_type, data_type, head_dim_half, bias):
    length = bias + head_dim_half * 2
    q = torch.randn((length, ), dtype=data_type).npu()
    k = torch.zeros_like(q, dtype=data_type).npu()
    k_ref = torch.zeros_like(q, dtype=data_type).npu()

    triton_interleave_load[(1, )](q, k, head_dim_half, bias)
    k_ref = torch_interleave_load(q, k_ref, head_dim_half, bias)
    assert torch.allclose(k, k_ref)


def test_interleave_runtime_bias_uses_deinterleave():
    head_dim_half = 16
    bias = 64
    q = torch.randn((bias + head_dim_half * 2, ), dtype=torch.float32).npu()
    k = torch.zeros((head_dim_half * 2, ), dtype=torch.float32).npu()

    kernel = triton_interleave_load_runtime_bias[(1, )](q, k, bias, head_dim_half)

    expected = torch.cat((q[bias::2][:head_dim_half], -q[bias + 1::2][:head_dim_half]))
    assert torch.allclose(k, expected)
    # The dynamic `bias` is an index SSA value rather than a tl.constexpr
    # attribute. B and B + 1 form a complementary pair, so one expanded copy
    # supplies the two tensor.extract_slice lane views.
    assert kernel.asm['ttadapter'].count('tensor.extract_slice') == 2
    assert kernel.asm['ttadapter'].count('memref.copy') == 1


def test_interleave_same_lane_static_offsets_fall_back():
    head_dim_half = 16
    q = torch.randn((64 + head_dim_half * 2 + 2, ), dtype=torch.float32).npu()
    out = torch.empty((head_dim_half * 2, ), dtype=torch.float32).npu()
    expected = torch.cat((q[64::2][:head_dim_half], q[66::2][:head_dim_half]))

    kernel = triton_deinterleave_same_lane_static_load[(1, )](q, out, head_dim_half)

    assert torch.allclose(out, expected)
    # 64 and 66 normalize to distinct EVEN bases, so the load pair must remain
    # two ordinary stride-2 copies rather than two independent deinterleaves.
    assert 'tensor.extract_slice' not in kernel.asm['ttadapter']
    assert kernel.asm['ttadapter'].count('memref.copy') >= 2


def test_interleave_complementary_static_offsets_use_deinterleave():
    head_dim_half = 16
    q = torch.randn((64 + head_dim_half * 2, ), dtype=torch.float32).npu()
    out = torch.empty((head_dim_half * 2, ), dtype=torch.float32).npu()
    expected = torch.cat((q[64::2][:head_dim_half], q[65::2][:head_dim_half]))

    kernel = triton_deinterleave_complementary_static_load[(1, )](q, out, head_dim_half)

    assert torch.allclose(out, expected)
    # 64/65 share normalized base 64 and select the EVEN/ODD lanes, so one
    # expanded copy is materialized and both original values become slices.
    assert kernel.asm['ttadapter'].count('tensor.extract_slice') == 2
    assert kernel.asm['ttadapter'].count('memref.copy') == 1


@pytest.mark.parametrize('para_type,data_type,head_dim_half,bias,numel', [
    ['float32', torch.float32, 16, 0, 8],
    ['float32', torch.float32, 16, 64, 8],
])
def test_interleave_with_mask(para_type, data_type, head_dim_half, bias, numel):
    length = bias + head_dim_half * 2
    q = torch.randn((length, ), dtype=data_type).npu()
    k = torch.zeros_like(q, dtype=data_type).npu()
    k_ref = torch.zeros_like(q, dtype=data_type).npu()

    triton_interleave_load_with_mask[(1, )](q, k, head_dim_half, bias, numel)
    k_ref = torch_interleave_load_with_mask(q, k_ref, head_dim_half, bias, numel)
    assert torch.allclose(k, k_ref)


@pytest.mark.parametrize('para_type,data_type,head_dim_half,bias,numel', [
    ['float32', torch.float32, 16, 0, 8],
    ['float32', torch.float32, 16, 64, 8],
])
def test_interleave_loadstore_with_mask(para_type, data_type, head_dim_half, bias, numel):
    length = bias + head_dim_half * 2
    q = torch.randn((length, ), dtype=data_type).npu()
    q_ref = q.clone()

    triton_interleave_loadstore_with_mask[(1, )](q, head_dim_half, bias, numel)
    q_ref = torch_interleave_loadstore_with_mask(q_ref, head_dim_half, bias, numel)
    assert torch.allclose(q, q_ref)
