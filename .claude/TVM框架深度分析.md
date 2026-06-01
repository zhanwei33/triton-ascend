# Apache TVM 框架深度分析文档

## 目录

- [1. 项目概述](#1-项目概述)
- [2. 代码仓架构](#2-代码仓架构)
- [3. TVM 的输入格式](#3-tvm-的输入格式)
- [4. 端到端编译流程](#4-端到端编译流程)
- [5. 自定义算子实现](#5-自定义算子实现)
- [6. Tensor Core 实现 Flash Attention](#6-tensor-core-实现-flash-attention)
- [7. 图优化在 Flash Attention 中的体现](#7-图优化在-flash-attention-中的体现)
- [8. TIR 级优化 Pass](#8-tir-级优化-pass)
- [9. 总结](#9-总结)

---

## 1. 项目概述

Apache TVM 是一个开放源代码的机器学习编译框架，遵循 Python-first 设计理念，支持将 ML 模型编译到多种硬件后端（CPU、GPU、NPU 等）。

**核心原则**：
- **Python-first 开发**：使机器学习编译器管道的快速定制成为可能
- **通用部署**：将模型带入最小可部署模块
- **跨层设计**：TensorIR 作为张量级表示，Relax 作为图级表示

**许可证**：Apache-2.0

**历史渊源**：
- [Halide](https://github.com/halide/Halide)：TVM 的 TIR 和算术简化模块部分源于 Halide
- [Loopy](https://github.com/inducer/loopy)：整数集分析和循环变换原语
- [Theano](https://github.com/Theano/Theano)：递归符号扫描算子的设计灵感

---

## 2. 代码仓架构

### 2.1 顶层目录结构

| 目录 | 用途 |
|------|------|
| `src/` | C++ 核心源码（编译器 + 运行时） |
| `include/` | C++ 公共头文件 |
| `python/` | Python 前端绑定 |
| `jvm/` | Java/Scala 绑定（Maven 项目） |
| `web/` | WebAssembly/JavaScript 绑定（Node.js 项目） |
| `apps/` | 示例应用（RPC 服务、Hexagon 等） |
| `tests/` | 测试代码（C++ 和 Python） |
| `3rdparty/` | 第三方依赖（CUTLASS、Flash Attention、tvm-ffi 等） |
| `cmake/` | CMake 构建工具链 |
| `docs/` | 文档 |
| `docker/` | Docker 配置 |
| `ci/` | CI/CD 配置（Jenkins/GitHub Actions） |

### 2.2 C++ 核心层 (`src/` + `include/`)

```
src/
├── ir/           # 基础 IR 基础设施
├── arith/        # 算术简化与约束求解
├── relax/        # Relax - 图级 IR 表示
│   ├── ir/       # Relax IR 定义
│   ├── op/       # Relax 算子
│   ├── transform/# 图变换 Pass
│   ├── backend/  # 后端代码生成
│   └── training/ # 训练支持
├── tirx/         # TensorIR - 张量级 IR 表示
│   ├── ir/       # TIR IR 定义
│   ├── op/       # TIR 算子
│   ├── transform/# 循环变换 Pass
│   └── analysis/ # 分析工具
├── s_tir/        # 调度 TIR（Schedule TIR）
├── te/           # Tensor Expression
├── topi/         # 张量算子索引（模板库）
├── target/       # 代码生成器
│   ├── llvm/     # LLVM 后端
│   ├── cuda/     # CUDA 后端
│   ├── rocm/     # ROCm 后端
│   ├── vulkan/   # Vulkan 后端
│   ├── metal/    # Metal 后端
│   ├── opencl/   # OpenCL 后端
│   ├── hexagon/  # Hexagon DSP 后端
│   └── webgpu/   # WebGPU 后端
├── runtime/      # 运行时系统
│   ├── vm/       # 虚拟机执行器
│   ├── rpc/      # RPC 运行时
│   ├── memory/   # 内存管理
│   ├── cuda/     # CUDA 运行时模块
│   └── extra/    # 扩展运行时（Disco 分布式等）
├── script/       # TVMScript 打印机/构建器
├── driver/       # 编译驱动
└── support/      # 工具函数
```

### 2.3 Python 前端层 (`python/tvm/`)

```
python/tvm/
├── relax/        # Relax Python API
├── tirx/         # TensorIR Python API
├── te/           # Tensor Expression API
├── topi/         # 张量算子库
├── runtime/      # 运行时 Python 绑定
├── target/       # 目标配置
├── ir/           # IR 基础
├── arith/        # 算术工具
├── script/       # TVMScript
├── contrib/      # 贡献模块
├── rpc/          # RPC 客户端
└── driver/       # 编译驱动
```

### 2.4 构建系统

- **CMake** 为主构建系统 (`CMakeLists.txt`)
- 支持可选后端：CUDA、ROCm、Vulkan、Metal、OpenCL、Hexagon 等
- 产出三个核心库：
  - `libtvm_runtime.so` - 运行时（仅运行时依赖）
  - `libtvm_compiler.so` - 编译器（链接 runtime）
  - `libtvm_runtime_extra.so` - 扩展运行时（Disco、NCCL、NVSHMEM 等）

---

## 3. TVM 的输入格式

TVM 支持**多层次**的输入形式，从高层模型格式到底层 IR 表示：

### 3.1 深度学习模型格式 (Frontend)

| 输入格式 | 导入函数 | 位置 |
|---------|---------|------|
| **ONNX** | `from_onnx()` | `python/tvm/relax/frontend/onnx/` |
| **PyTorch** | `from_fx()`, `from_exported_program()` | `python/tvm/relax/frontend/torch/` |
| **TFLite** | `from_tflite()` | `python/tvm/relax/frontend/tflite/` |
| **StableHLO** | `from_stablehlo()` | `python/tvm/relax/frontend/stablehlo/` |

### 3.2 编程接口输入

- **Relax IR** - 图级表示（计算图）
- **TensorIR (TIR)** - 张量级表示（循环程序）
- **Tensor Expression (TE)** - 声明式张量计算
- **TVMScript** - IR 的 Python DSL 表示

### 3.3 典型工作流程

```
PyTorch/ONNX/TFLite 模型
        ↓
    Frontend 导入
        ↓
   Relax IR (计算图)
        ↓
   图变换 Pass
        ↓
   TensorIR (循环级)
        ↓
   代码生成 → 目标后端 (CUDA/LLVM/Metal/...)
```

---

## 4. 端到端编译流程

以下以一个 **PyTorch MLP 模型** 为例，完整展示 TVM 从读取输入到生成可执行代码的全过程。

### 4.1 阶段一：读取输入 → 构建 IRModule

```python
import torch
from torch.export import export
import tvm
from tvm import relax
from tvm.relax.frontend.torch import from_exported_program

# 1. 定义 PyTorch 模型
class TorchMLP(torch.nn.Module):
    def __init__(self):
        super().__init__()
        self.net = torch.nn.Sequential(
            torch.nn.Flatten(),
            torch.nn.Linear(784, 256),
            torch.nn.ReLU(),
            torch.nn.Linear(256, 10),
        )
    def forward(self, x):
        return self.net(x)

# 2. 用 torch.export 导出静态图（固定输入 shape）
example_args = (torch.randn(1, 784, dtype=torch.float32),)
exported_program = export(TorchMLP().eval(), example_args)

# 3. 前端翻译：PyTorch ExportedProgram → TVM IRModule
mod = from_exported_program(
    exported_program,
    keep_params_as_input=True,   # 权重作为函数参数传入
    unwrap_unit_return_tuple=True
)

# 4. 分离参数（权重与计算图解耦）
mod, params = relax.frontend.detach_params(mod)
mod.show()
```

**此时 `mod` 的 IR 形态（Relax 图级 IR）：**

```
@main = (x: Tensor[(1, 784), f32], w0: Tensor[(256, 784), f32], b0: Tensor[(256,), f32], ...)
  -> Tensor[(1, 10), f32] {
  with R.dataflow():
    lv0 = R.matmul(x, w0) + b0        # Linear(784→256)
    lv1 = R.nn.relu(lv0)              # ReLU
    lv2 = R.matmul(lv1, w1) + b1      # Linear(256→10)
    R.output(lv2)
  return lv2
}
```

### 4.2 阶段二：IR 图变换优化

#### 4.2.1 `LegalizeOps` — 算子合法化（Relax → call_tir）

```python
mod = relax.transform.LegalizeOps()(mod)
```

将每个 Relax 算子替换为 `R.call_tir`，并生成对应的 **TensorIR 函数**：

```
@main = (x, w0, b0, ...) {
  with R.dataflow():
    lv0 = R.call_tir(fused_matmul_add, (x, w0, b0))   # 新生成 TIR 函数
    lv1 = R.call_tir(fused_relu, (lv0))
    lv2 = R.call_tir(fused_matmul_add1, (lv1, w1, b1))
    R.output(lv2)
  return lv2
}
```

#### 4.2.2 `AnnotateTIROpPattern` — 标注算子融合模式

为每个 TIR 函数标注 pattern（如 `kElemWise`, `kBroadcast`, `kOutFusible`），为后续融合做准备。

#### 4.2.3 `FoldConstant` — 常量折叠

预计算图中所有常量表达式（如 shape 计算、transpose 等）。

#### 4.2.4 `FuseOps` + `FuseTIR` — 算子融合

将多个小算子融合为一个，减少 kernel 启动开销和中间内存分配：

```python
mod = relax.get_pipeline("zero")(mod)  # 包含以上所有 pass
```

**"zero" pipeline 完整 pass 序列**（`python/tvm/relax/pipeline.py:33-77`）：

| Pass | 作用 |
|------|------|
| `LegalizeOps` | Relax 算子 → `call_tir` + TIR 函数 |
| `AnnotateTIROpPattern` | 标注融合模式 |
| `FoldConstant` | 常量折叠 |
| `FuseOps` | 基于 pattern 融合 Relax 算子 |
| `FuseTIR` | 合并 TIR 函数 |

### 4.3 阶段三：编译为可执行代码

#### 4.3.1 `tvm.compile` — 编译

```python
target = tvm.target.Target("llvm")  # CPU 目标
executable = tvm.compile(mod, target=target)
```

`tvm.compile` 内部执行 **default_build_pipeline**（`pipeline.py:80-107`）：

| Pass | 作用 |
|------|------|
| `DispatchSampling` / `DispatchSortScan` | 后端分发 |
| `LegalizeOps` | 再次合法化（幂等） |
| `RewriteDataflowReshape` | 重写 reshape |
| `ToNonDataflow` / `RemovePurityChecking` | 去除 dataflow 标记 |
| `CallTIRRewrite` | 重写 call_tir |
| `StaticPlanBlockMemory` | 静态内存规划 |
| `LowerAllocTensor` | 降低内存分配 |
| `KillAfterLastUse` | 插入内存释放 |
| `LowerRuntimeBuiltin` | 降低运行时内置函数 |
| `VMShapeLower` | VM shape 降低 |
| `AttachGlobalSymbol` | 附加全局符号 |

#### 4.3.2 导出为共享库

```python
executable.export_library("mlp_cpu.so")
```

生成的 `mlp_cpu.so` 包含：
- VM Bytecode — Relax 虚拟机指令
- Compiled Kernels — TIR 函数经 LLVM/CodeGen 生成的机器码
- Constants — 嵌入的常量数据

#### 4.3.3 加载执行

```python
import numpy as np

loaded_lib = tvm.runtime.load_module("mlp_cpu.so")
dev = tvm.cpu()
vm = relax.VirtualMachine(loaded_lib, dev)

data = np.random.randn(1, 784).astype("float32")
tvm_data = tvm.runtime.tensor(data, dev)
tvm_params = [tvm.runtime.tensor(p, dev) for p in params["main"]]

output = vm["main"](tvm_data, *tvm_params)
print(output.numpy())
```

### 4.4 完整流程图

```
┌─────────────────────────────────────────────────────────────────┐
│                     阶段一：输入读取                              │
│                                                                 │
│  PyTorch / ONNX / TFLite                                        │
│       ↓                                                         │
│  Frontend Import (from_exported_program / from_onnx)            │
│       ↓                                                         │
│  IRModule (Relax IR) ← 计算图 + 参数                             │
└─────────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────────┐
│                   阶段二：IR 图变换优化                           │
│                                                                 │
│  IRModule                                                       │
│       ↓                                                         │
│  LegalizeOps → Relax 算子 → call_tir + TensorIR 函数             │
│       ↓                                                         │
│  AnnotateTIROpPattern → 标注融合模式                             │
│       ↓                                                         │
│  FoldConstant → 常量折叠                                         │
│       ↓                                                         │
│  FuseOps + FuseTIR → 算子融合（减少 kernel 启动 + 中间内存）       │
│       ↓                                                         │
│  [可选] MetaScheduleTuneIRMod → 自动调优                          │
│       ↓                                                         │
│  [可选] DLight GPU Schedule → 生成 GPU 调度                       │
│       ↓                                                         │
│  优化后的 IRModule                                               │
└─────────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────────┐
│                   阶段三：编译与执行                              │
│                                                                 │
│  IRModule                                                       │
│       ↓                                                         │
│  default_build_pipeline (内存规划 / VM 降低 / ...)               │
│       ↓                                                         │
│  tvm.compile(target="llvm/cuda/...")                            │
│       ↓                                                         │
│  Executable = VM Bytecode + Compiled Kernels + Constants        │
│       ↓                                                         │
│  export_library("model.so")                                     │
│       ↓                                                         │
│  load_module + VirtualMachine → 推理执行                         │
└─────────────────────────────────────────────────────────────────┘
```

---

## 5. 自定义算子实现

TVM 提供了**多层次**的自定义算子实现方式，从高层到低层：

### 5.1 方法一：TIR 级别直接写 Kernel（推荐用于 GPU 算子）

这是 TVM 实现 Flash Attention 等复杂算子的主要方式。用 **TVMScript** 直接编写 CUDA kernel，包含线程绑定、共享内存、向量化等。

#### 示例：简化版 Flash Attention TIR Kernel

```python
from tvm.script import tirx as T
import tvm
from tvm import s_tir

@T.prim_func(s_tir=True)
def flash_attention_kernel(
    var_q: T.handle,      # [batch, seq_len, num_heads, head_dim]
    var_k: T.handle,      # [batch, seq_len, num_heads, head_dim]
    var_v: T.handle,      # [batch, seq_len, num_heads, head_dim]
    var_output: T.handle, # [batch, seq_len, num_heads, head_dim]
    sm_scale: T.float32,
):
    T.func_attr({"global_symbol": "flash_attention"})
    
    batch = T.int32(is_size_var=True)
    seq_len = T.int32(is_size_var=True)
    num_heads = T.int32(is_size_var=True)
    head_dim = T.int32(is_size_var=True)
    
    Q = T.match_buffer(var_q, (batch, seq_len, num_heads, head_dim), "float16")
    K = T.match_buffer(var_k, (batch, seq_len, num_heads, head_dim), "float16")
    V = T.match_buffer(var_v, (batch, seq_len, num_heads, head_dim), "float16")
    Output = T.match_buffer(var_output, (batch, seq_len, num_heads, head_dim), "float16")
    
    # 线程绑定
    for bx in T.thread_binding(batch * num_heads, thread="blockIdx.x"):
        for by in T.thread_binding(T.ceildiv(seq_len, 64), thread="blockIdx.y"):
            for ty in T.thread_binding(4, thread="threadIdx.y"):  # warps
                for tx in T.thread_binding(32, thread="threadIdx.x"):  # threads
                    
                    batch_idx = bx // num_heads
                    head_idx = bx % num_heads
                    
                    # 共享内存分配
                    Q_smem = T.sblock_alloc_buffer((64, head_dim), "float16", scope="shared")
                    K_smem = T.sblock_alloc_buffer((64, head_dim), "float16", scope="shared")
                    V_smem = T.sblock_alloc_buffer((64, head_dim), "float16", scope="shared")
                    S_smem = T.sblock_alloc_buffer((64, 64), "float32", scope="shared")
                    
                    # Online softmax 状态
                    m_max = T.sblock_alloc_buffer((64,), "float32", scope="shared")
                    d_sum = T.sblock_alloc_buffer((64,), "float32", scope="shared")
                    O_local = T.sblock_alloc_buffer((64, head_dim), "float32", scope="local")
                    
                    # ... 计算逻辑 ...
```

### 5.2 方法二：将 TIR Kernel 绑定到 Relax 算子

```python
from tvm import relax
from tvm.relax import call_tir
import tvm
from tvm.script import ir as I, relax as R

@I.ir_module
class ModuleWithCustomOp:
    @R.function
    def main(
        x: R.Tensor((1, 1024, 32, 128), "float16"),
        k: R.Tensor((1, 1024, 32, 128), "float16"),
        v: R.Tensor((1, 1024, 32, 128), "float16"),
    ) -> R.Tensor((1, 1024, 32, 128), "float16"):
        with R.dataflow():
            output = R.call_tir(
                ModuleWithCustomOp["flash_attention"],
                (x, k, v, R.prim_value(0.125)),
                out_sinfo=R.Tensor((1, 1024, 32, 128), "float16")
            )
            R.output(output)
        return output
    
    @T.prim_func(s_tir=True)
    def flash_attention(var_q, var_k, var_v, var_output, sm_scale):
        # ... kernel 实现 ...
        pass
```

### 5.3 方法三：BYOC（Bring Your Own Codegen）集成外部库

如果想调用已有的 Flash Attention 库（如 FlashInfer、CUTLASS），用 BYOC 模式：

```python
from tvm.relax.dpl.pattern import is_op, wildcard
from tvm.relax.backend.pattern_registry import register_patterns
from tvm.relax.transform import FuseOpsByPattern, RunCodegen

# 1. 定义模式匹配
def flash_attention_pattern():
    q = wildcard()
    k = wildcard()
    v = wildcard()
    qk = is_op("relax.matmul")(q, is_op("relax.permute_dims")(k))
    attn = is_op("relax.nn.softmax")(qk)
    output = is_op("relax.matmul")(attn, v)
    return ("flashinfer.attention", output, {"q": q, "k": k, "v": v})

# 2. 注册模式
register_patterns([flash_attention_pattern()])

# 3. 在优化 pipeline 中使用
patterns = [("flashinfer.attention", flash_attention_pattern())]
mod = FuseOpsByPattern(patterns, annotate_codegen=True)(mod)
mod = RunCodegen({"flashinfer": {"sm_scale": 0.125}})(mod)
```

### 5.4 方法四：Runtime PackedFunc 注册（最简单）

```python
# Python 端调用
flash_attn_func = tvm.get_global_func("my.flash_attention_forward")

# 在 Relax kernel 中调用
@T.prim_func(s_tir=True)
def call_flash_attn(var_q, var_k, var_v, var_output):
    T.func_attr({"global_symbol": "call_flash_attn"})
    T.call_packed("my.flash_attention_forward", var_q, var_k, var_v, var_output)
```

```cpp
// C++ 端注册
TVM_REGISTER_GLOBAL("my.flash_attention_forward")
    .set_body_typed([](NDArray q, NDArray k, NDArray v, NDArray output) {
        FlashAttentionForward(q, k, v, output);
    });
```

### 5.5 TVM 内置 Flash Attention 的实际位置

| 文件 | 作用 |
|------|------|
| `python/tvm/relax/frontend/nn/llm/_prefill_kernels.py` | TIR 实现的 Prefill kernel |
| `python/tvm/relax/frontend/nn/llm/_decode_kernels.py` | TIR 实现的 Decode kernel |
| `python/tvm/relax/frontend/nn/llm/_kernel_common.py` | 共享宏（`@T.macro`）、调度、配置 |
| `python/tvm/contrib/cutlass/attention_operation.py` | CUTLASS Flash Attention 模板 |
| `python/tvm/relax/backend/cuda/flashinfer.py` | FlashInfer JIT 集成 |
| `src/runtime/extra/contrib/cudnn/cudnn_frontend/attention.cc` | cuDNN Attention runtime |

---

## 6. Tensor Core 实现 Flash Attention

Flash Attention 的核心计算包含两个 GEMM：
1. **S = Q @ K^T** (计算 attention scores)
2. **O = S @ V** (加权求和)

使用 Tensor Core 的关键是将这两个 16x16 的矩阵块乘法替换为 `tvm_mma_sync` intrinsic。

### 6.1 关键 TIR Intrinsics

| Intrinsic | CUDA WMMA API | 作用 |
|-----------|---------------|------|
| `T.tvm_fill_fragment` | `nvcuda::wmma::fill_fragment` | 初始化 accumulator 为 0 |
| `T.tvm_load_matrix_sync` | `nvcuda::wmma::load_matrix_sync` | shared → fragment |
| `T.tvm_mma_sync` | `nvcuda::wmma::mma_sync` | 16x16x16 矩阵乘加 |
| `T.tvm_store_matrix_sync` | `nvcuda::wmma::store_matrix_sync` | fragment → shared/global |

### 6.2 内存 Scope 说明

| Scope | 含义 |
|-------|------|
| `shared.dyn` | 动态共享内存 |
| `wmma.matrix_a` | WMMA A 矩阵 fragment (寄存器) |
| `wmma.matrix_b` | WMMA B 矩阵 fragment (寄存器) |
| `wmma.accumulator` | WMMA 累加器 fragment (float32) |

### 6.3 使用 DLight 自动 Tensorize

TVM 推荐使用 **DLight 自动调度** 来完成 Tensor Core 的 tensorization：

```python
import tvm
from tvm import relax
from tvm.s_tir import dlight as dl
from tvm.script import tirx as T
from tvm.target import Target

# 1. 定义基础 GEMM-like Flash Attention TIR
@T.prim_func(s_tir=True)
def flash_attn_base(var_q, var_k, var_v, var_output, sm_scale):
    # ... 计算逻辑 ...
    pass

# 2. 使用 DLight 自动应用 Tensor Core 调度
mod = tvm.IRModule({"main": flash_attn_base})

with Target("nvidia/geforce-rtx-3090"):
    mod = dl.ApplyDefaultSchedule(
        dl.gpu.Matmul(),      # 识别 matmul 并 tensorize
        dl.gpu.GEMV(),
        dl.gpu.Reduction(),
        dl.gpu.Fallback(),    # 无法识别的 fallback 到普通 schedule
    )(mod)

# 3. 编译
target = tvm.target.Target("cuda")
with tvm.transform.PassContext(opt_level=3):
    lib = tvm.compile(mod, target=target)
```

### 6.4 手动 Tensorize 的完整调度流程

```python
import tvm
from tvm import s_tir
from tvm.s_tir.tensor_intrin.cuda import get_wmma_intrin_group
from tvm.script import tirx as T

# 1. 定义基础 matmul
@T.prim_func(s_tir=True)
def matmul_16x16(A, B, C):
    for i, j, k in T.grid(256, 256, 256):
        with T.sblock("C"):
            vi, vj, vk = T.axis.remap("SSR", [i, j, k])
            with T.init():
                C[vi, vj] = T.float32(0)
            C[vi, vj] += A[vi, vk] * B[vj, vk]

# 2. 创建 Schedule
mod = tvm.IRModule({"main": matmul_16x16})
sch = s_tir.Schedule(mod)

# 3. Split loops to match WMMA 16x16x16 tile size
block = sch.get_sblock("C")
i, j, k = sch.get_loops(block)
i_outer, i_inner = sch.split(i, factors=[None, 16])
j_outer, j_inner = sch.split(j, factors=[None, 16])
k_outer, k_inner = sch.split(k, factors=[None, 16])
sch.reorder(i_outer, j_outer, k_outer, i_inner, j_inner, k_inner)

# 4. Blockize and create caches
block_outer = sch.blockize(i_inner)
A_shared = sch.cache_read(block_outer, 0, "shared.dyn")
B_shared = sch.cache_read(block_outer, 1, "shared.dyn")
A_wmma = sch.cache_read(block_outer, 0, "wmma.matrix_a")
B_wmma = sch.cache_read(block_outer, 1, "wmma.matrix_b")
C_wmma = sch.cache_write(block_outer, 0, "wmma.accumulator")

# 5. Get WMMA intrinsic group and apply tensorization
intrin_group = get_wmma_intrin_group(
    load_scope="shared.dyn", store_scope="shared.dyn",
    in_dtype="float16", out_dtype="float32", trans_b=True
)
sch.tensorize(sch.get_loops(A_wmma)[-2], intrin_group["load_a"])
sch.tensorize(sch.get_loops(B_wmma)[-2], intrin_group["load_b"])
sch.tensorize(sch.get_loops(block_inner)[-3], intrin_group["compute"])
sch.tensorize(sch.get_loops(C_wmma)[-2], intrin_group["store"])
```

---

## 7. 图优化在 Flash Attention 中的体现

TVM 的图优化在 Flash Attention 中体现在两个层面：
- **Relax 层**：模式匹配 → 算子融合 → 后端分发 → 内存优化
- **TIR 层**：通过 `AnalyzeOpPatternKind` 识别 kernel 内部计算模式，指导调度决策

### 7.1 图优化的完整流程

```
原始计算图（PyTorch 导入）
    ↓
[Pass 1: 模式重写] Q @ K^T -> Softmax -> @ V  识别为 attention 模式
    ↓
[Pass 2: 算子融合] 融合为 R.nn.attention() 复合算子
    ↓
[Pass 3: 后端分发] 分发给 CUTLASS / FlashInfer / TIR kernel
    ↓
[Pass 4: TIR 模式标注] AnalyzeOpPatternKind 识别 Matmul/Reduce 模式
    ↓
[Pass 5: 内存优化] 消除中间 S 矩阵 (batch × heads × seq × seq) 的分配
    ↓
优化后的计算图
```

### 7.2 代码示例：图级 Flash Attention 优化

#### 步骤 1：原始计算图（未优化）

```python
@I.ir_module
class UnfusedAttention:
    @R.function
    def main(q, k, v):
        with R.dataflow():
            k_transposed = R.permute_dims(k, axes=[0, 1, 3, 2])
            scores = R.matmul(q, k_transposed)          # [1, 32, 1024, 1024] ← 中间 tensor!
            scale = R.const(0.088388, "float32")
            scaled_scores = R.multiply(scores, scale)
            softmax_scores = R.nn.softmax(scaled_scores, axis=-1)  # ← 中间 tensor!
            output = R.matmul(softmax_scores, v)
            R.output(output)
        return output
```

**问题**：
- 中间产生 `scores` 和 `softmax_scores` 两个 `[1, 32, 1024, 1024]` 的 tensor
- 每个占用 64 MB，总共 **128 MB** 的中间内存分配
- 4 次独立的 kernel 启动

#### 步骤 2：模式匹配与重写

```python
from tvm.relax.backend.patterns import make_attention_rewrite_pattern
from tvm.relax.transform import rewrite_call

pattern, _, _, rewriter = make_attention_rewrite_pattern(
    qkv_layout="separate", out_layout="same",
    with_bias=False, with_cast=False, with_kv_repeat=False
)

rewritten_mod = rewrite_call(pattern, rewriter, UnfusedAttention)
```

**重写后的 IR**：

```python
@I.ir_module
class RewrittenAttention:
    @R.function
    def main(q, k, v):
        with R.dataflow():
            output = R.nn.attention(
                query=q, key=k, value=v, scale=0.088388, causal_mask=None
            )
            R.output(output)
        return output
```

#### 步骤 3：后端模式匹配与分区

```python
from tvm.relax.backend.cuda.cutlass import partition_for_cutlass
from tvm.relax.transform import FuseOpsByPattern, RunCodegen

patterns = [
    ("cutlass.attention", *make_attention_pattern()),
    ("cutlass.attention_bias", *make_attention_pattern(with_bias=True)),
]

mod = FuseOpsByPattern(patterns, annotate_codegen=True)(rewritten_mod)
mod = RunCodegen({"cutlass": {"use_flash": True}})(mod)
```

### 7.3 图优化的具体收益

| 优化项 | 优化前 | 优化后 | 收益 |
|--------|--------|--------|------|
| **Kernel 数量** | 4 个 (matmul, mul, softmax, matmul) | 1 个 (flash_attention) | 减少 75% kernel 启动 |
| **中间内存** | 128 MB (S + softmax_S) | 0 MB (online softmax) | 节省 100% 中间内存 |
| **Global Memory 访问** | 读 6 次写 2 次 | 读 3 次写 1 次 | 减少 62.5% 带宽 |
| **数值精度** | softmax 有 round-off | online softmax 更稳定 | 提高精度 |

### 7.4 TVM 内置的 Attention 模式定义

#### 7.4.1 Relax 层模式（图级）

```python
from tvm.relax.dpl.pattern import is_op, wildcard, is_constant

def make_attention_rewrite_pattern():
    q = wildcard()
    k = wildcard()
    v = wildcard()
    scale = is_constant()

    k_transposed = is_op("relax.permute_dims")(k)
    matmul_qk = is_op("relax.matmul")(q, k_transposed)
    scaled = is_op("relax.multiply")(matmul_qk, scale)
    softmax_out = is_op("relax.nn.softmax")(scaled)
    output = is_op("relax.matmul")(softmax_out, v)

    def rewriter(extract_match, ctx):
        return R.nn.attention(
            query=extract_match.map[q],
            key=extract_match.map[k],
            value=extract_match.map[v],
            scale=extract_match.map[scale],
        )

    return output, rewriter
```

#### 7.4.2 TIR 层模式（Kernel 内部）

当 Attention 算子被 Lower 到 TIR 后，`AnnotateTIROpPattern` pass 会调用 `AnalyzeOpPatternKind` 对每个 TIR PrimFunc 进行模式分析：

```cpp
// src/relax/analysis/tir_op_pattern_kind.cc:349-353
OpPatternKind AnalyzeOpPatternKind(const PrimFunc& func) {
  PatternKindAnalyzer analyzer(func);
  analyzer(func->body);  // 遍历 TIR AST
  return analyzer.GetResult();
}
```

**分析流程**：
1. 遍历每个 TIR `Block`，收集 `BufferLoad` 和 `BufferStore`
2. 分析 load/store 索引关系，判断模式：
   - `IsElemwisePattern`: 索引完全相同（如 `A[i,j] = B[i,j]`）
   - `IsBroadcastPattern`: load 索引是 store 索引的有序子集
   - `IsInjectivePattern`: load 索引变量都在 store 索引中（顺序无关）
3. 检查是否包含归约轴（`kCommReduce`）
4. 检查是否为 FMA（乘加模式，`C += A * B`）→ `kOutEWiseFusable`

**Flash Attention Kernel 各 Block 的模式标注**：

| Block | 计算内容 | 检测到的 Pattern | 属性值 |
|-------|----------|-----------------|--------|
| `load_q/k/v` | 共享内存加载 | `kElemWise` | `op_pattern = 0` |
| `matmul_qk` | Q@K^T | `kOutEWiseFusable` (FMA) | `op_pattern = 4` |
| `row_max` | Softmax max | `kCommReduce` | `op_pattern = 3` |
| `softmax_exp_sum` | Exp + Sum | `kOutEWiseFusable` | `op_pattern = 4` |
| `matmul_pv` | P@V | `kOutEWiseFusable` (FMA) | `op_pattern = 4` |

**关键检测逻辑**（`IsFMA` 函数）：

```cpp
// 识别 C[i,j] += A[i,k] * B[j,k] 模式
static bool IsFMA(const Stmt& body) {
  // 模式匹配：Store = Add(StoreLoad, Mul(Load, Load))
  if (const auto* store = body.as<BufferStoreNode>()) {
    if (const auto* add = RemoveCast(store->value).as<tir::AddNode>()) {
      if (const auto* mul = RemoveCast(add->b).as<tir::MulNode>()) {
        // 检查是否为累加模式
        const auto* store_lhs = RemoveCast(add->a).as<tir::BufferLoadNode>();
        if (!store_lhs || !store->buffer.same_as(store_lhs->buffer)) {
          return false;
        }
        // 检查数据重用（存在 reduce 维度）
        return IsAllowReusePattern(store, lhs) &&
               IsAllowReusePattern(store, rhs);
      }
    }
  }
  return false;
}
```

这些模式标注将被后续优化使用：
- **FuseOps**: 基于 pattern 决定 TIR 函数间的融合策略
- **DLight**: `kOutEWiseFusable` 触发 Matmul/GEMM schedule 模板
- **内存规划**: `kCommReduce` 提示需要保留中间结果

---

## 8. TIR 级优化 Pass

TIR 级优化分为两个维度：
1. **横向（图级）**：通过 `FuseOps` 融合多个 TIR 函数
2. **纵向（Kernel 内部）**：通过 `PatternKindAnalyzer` 识别计算模式，指导调度优化

### 8.1 层次一：TIR 模式分析与标注

`AnalyzeOpPatternKind` 是 TIR 优化的核心，它通过分析每个 TIR Block 的访存模式，为后续优化提供决策依据。

#### 8.1.1 PatternKindAnalyzer 工作流程

```cpp
// src/relax/analysis/tir_op_pattern_kind.cc:34-160
class PatternKindAnalyzer : public StmtExprVisitor {
  void VisitStmt_(const BlockNode* op) final {
    // Step 1: 清空之前收集的 load/store
    loads_.clear();
    store_ = std::nullopt;

    // Step 2: 访问 block body，收集所有的 BufferLoad 和 BufferStore
    StmtVisitor::VisitStmt(op->body);

    // Step 3: 分析 load/store 索引模式
    // - IsElemwisePattern: store.indices == load.indices
    // - IsBroadcastPattern: load 索引是 store 索引的有序子集
    // - IsInjectivePattern: load 索引变量都在 store 索引中
    OpPatternKind index_pattern = AnalyzeIndexPattern(store_, loads_);

    // Step 4: 检查是否包含归约轴
    bool has_reduction = CheckReduceAxis(op->iter_vars);

    // Step 5: 综合判断
    if (has_reduction) {
      if (IsFMA(op->body)) {
        kind_ = kOutEWiseFusable;  // Matmul/Conv
      } else if (IsPureReducePattern(...)) {
        kind_ = kCommReduce;        // sum/max
      }
    }
  }
};
```

#### 8.1.2 手写 Flash Attention Kernel 的模式分析示例

以手写 Flash Attention TIR Kernel 为例，展示各 Block 的模式检测：

**Block 1: 共享内存加载（kElemWise）**

```python
for b, h, s, d in T.grid(B, H, S, D):
    with T.block("load_q"):
        vb, vh, vs, vd = T.axis.remap("SSSS", [b, h, s, d])
        Q_shared[vb, vh, vs, vd] = Q[vb, vh, vs, vd]  # 索引完全相同
```

分析过程：
- `store.indices = [vb, vh, vs, vd]`
- `load.indices = [vb, vh, vs, vd]`
- `IsElemwisePattern()` → `true`
- **判定**: `kElemWise`

**Block 2: Q@K^T 矩阵乘（kOutEWiseFusable）**

```python
for b, h, i, j, k in T.grid(B, H, S, S, D):
    with T.block("matmul_qk"):
        vb, vh, vi, vj, vk = T.axis.remap("SSSSR", [b, h, i, j, k])
        with T.init():
            S_scores[vb, vh, vi, vj] = T.float32(0)
        # FMA 模式：C += A * B
        S_scores[vb, vh, vi, vj] += Q_shared[vb, vh, vi, vk] * K_shared[vb, vh, vj, vk]
```

分析过程：
- 包含归约轴 `vk`（`kCommReduce`）
- body 结构：`BufferStore = Add(BufferLoad, Mul(BufferLoad, BufferLoad))`
- `IsFMA()` 检查：
  - `add->a` 是对同一 buffer 的 load（累加模式）✓
  - `add->b` 是乘法，两个操作数都是 BufferLoad ✓
  - `IsAllowReusePattern`：load 有额外的 reduce 维度 ✓
- **判定**: `kOutEWiseFusable`

**Block 3: Row-wise Softmax Max（kCommReduce）**

```python
for b, h, i, j in T.grid(B, H, S, S):
    with T.block("row_max"):
        vb, vh, vi = T.axis.remap("SSS", [b, h, i])
        vj = T.axis.reduce(axis=S)  # 归约轴
        m_max[vb, vh, vi] = T.max(m_max[vb, vh, vi], S_scores[vb, vh, vi, vj])
```

分析过程：
- 包含归约轴 `vj`
- body 是 `Max` 而不是 `Add/Mul` → `IsFMA()` 返回 `false`
- `IsPureReducePattern()`：归约轴直接使用，无表达式变换 → `true`
- **判定**: `kCommReduce`

| Block | 计算内容 | Pattern | 后续优化 |
|-------|----------|---------|----------|
| `load_q/k/v` | 共享内存加载 | `kElemWise` | VectorizeLoop, InjectPTXAsyncCopy |
| `matmul_qk` | Q@K^T | `kOutEWiseFusable` | DLight Matmul Schedule, Tensorization |
| `row_max` | Softmax max | `kCommReduce` | Reduction Schedule, Warp Shuffle |
| `softmax_exp_sum` | Exp + Sum | `kOutEWiseFusable` | 融合计算 |
| `matmul_pv` | P@V | `kOutEWiseFusable` | DLight Matmul Schedule |

### 8.2 层次二：Kernel 内部的自动调度优化（TIR Pass）

基于模式标注，TVM 应用以下优化：

| TIR Pass | 适用 Pattern | 优化内容 | 效果 |
|----------|-------------|---------|------|
| **LoopPartition** | All | 根据条件分割循环 | `if col <= row` → 两个无分支循环 |
| **VectorizeLoop** | `kElemWise` | 向量化内存访问 | 标量 load → `T.vload4()` (128 bit) |
| **UnrollLoop** | All | 展开小循环 | `for k in range(16)` → 16 条指令 |
| **InjectSoftwarePipeline** | `kOutEWiseFusable` | 软件流水线 | 重叠 load[k+1] + compute[k] |
| **CompactBufferAllocation** | All | 压缩 buffer | 分配实际访问的 tile 而非完整 tensor |
| **MergeSharedMemoryAllocations** | All | 合并共享内存 | Q/K/V_smem 复用同一块内存 |
| **InjectPermutedLayout** | `kOutEWiseFusable` | 消除 bank conflict | 交错布局避免冲突 |
| **InjectPTXAsyncCopy** | `kElemWise` | 异步拷贝 | `cp.async` 隐藏延迟 |
| **StorageRewrite** | All | 存储复用 | m_max/d_sum 复用 local memory |

### 8.2 层次二：Kernel 间的图级优化（Relax Pass）

即使手写了 Flash Attention kernel，它在计算图中仍可能与其他算子组合：

```python
@I.ir_module
class LLMWithHandwrittenFA:
    @R.function
    def forward(hidden, q_weight, k_weight, v_weight, o_weight):
        with R.dataflow():
            q = R.matmul(hidden, q_weight)
            k = R.matmul(hidden, k_weight)
            v = R.matmul(hidden, v_weight)
            
            q = R.reshape(q, (1, 1024, 32, 128))
            q = R.permute_dims(q, (0, 2, 1, 3))
            
            # 调用手写的 Flash Attention kernel
            attn_out = R.call_tir(flash_attention_handwritten, (q, k, v, R.prim_value(0.088388)))
            
            attn_out = R.permute_dims(attn_out, (0, 2, 1, 3))
            attn_out = R.reshape(attn_out, (1, 1024, 4096))
            
            output = R.matmul(attn_out, o_weight)
            R.output(output)
        return output
```

**图优化 Pass 对上面的处理**：

| Pass | 优化内容 |
|------|---------|
| `FoldConstant` | 折叠 `1/sqrt(dim)` 等常量计算 |
| `DeadCodeElimination` | 删除未使用的中间 tensor |
| `FuseOps` | 融合 reshape/permute_dims 到相邻 matmul |
| `StaticPlanBlockMemory` | 规划内存分配（消除中间 buffer） |
| `KillAfterLastUse` | q 在 attention 用完后立即释放，内存复用 |

### 8.3 层次三：DLight 基于模式的自动 Tensorization

DLight 利用 `AnalyzeOpPatternKind` 的标注结果，自动应用 GPU 特定的调度策略。

#### 8.3.1 模式识别与 Schedule 映射

```python
# python/tvm/s_tir/dlight/gpu/matmul.py
class Matmul(GPUSchedule):
    def apply(self, sch: Schedule, target: Target) -> bool:
        for block in sch.get_blocks():
            # 检查 block 是否有 FMA 模式 (kOutEWiseFusable)
            if self._is_matmul_block(sch, block):
                # 应用 Tensor Core schedule
                self._apply_tensor_core_schedule(sch, block)
```

**手写时只需写普通 matmul**：

```python
for b, h, i, j, k in T.grid(batch, heads, seq, seq, dim):
    with T.sblock("S_gemm"):
        vi, vj, vk = T.axis.remap("SSR", [i, j, k])
        with T.init():
            S[vi, vj] = T.float32(0)
        S[vi, vj] += Q[vi, vk] * K[vj, vk]  # FMA 模式
```

**DLight 自动转换流程**：

1. **Loop Split** (匹配 WMMA tile 大小)
   ```
   i → i_outer, i_inner(16)
   j → j_outer, j_inner(16)
   k → k_outer, k_inner(16)
   ```

2. **Shared Memory Cache**
   ```python
   Q_shared = sch.cache_read(block, 0, "shared")
   K_shared = sch.cache_read(block, 1, "shared")
   ```

3. **WMMA Tensorization**
   ```python
   # 自动插入 Tensor Core intrinsic
   T.tvm_load_matrix_sync(Q_frag, ..., "wmma.matrix_a")
   T.tvm_load_matrix_sync(K_frag, ..., "wmma.matrix_b")
   T.tvm_mma_sync(S_frag, Q_frag, K_frag, ...)
   T.tvm_store_matrix_sync(S_frag, S_shared, ...)
   ```

#### 8.3.2 不同 Pattern 的 Schedule 策略

| Pattern | DLight Schedule | 适用场景 |
|---------|----------------|---------|
| `kOutEWiseFusable` (FMA) | `gpu.Matmul()` | 矩阵乘、卷积 → Tensor Core |
| `kCommReduce` | `gpu.Reduction()` | 归约 → Warp Shuffle |
| `kElemWise` | `gpu.Fallback()` | 逐元素 → 向量化 |
| `kInjective` | `gpu.Fallback()` | 转置/reshape → 并行化 |

### 8.4 TIR Transformation Passes 完整列表

#### 模式分析相关 (`src/relax/analysis/`)

| 函数/Pass | 作用 | 输出 |
|-----------|------|------|
| **AnalyzeOpPatternKind** | 分析 TIR Block 的访存模式 | `OpPatternKind` 枚举值 |
| **AnnotateTIROpPattern** | 将模式标注为 TIR 函数属性 | `"op_pattern": int` |
| **HasReshapePattern** | 检测 reshape/transpose 模式 | 用于内存布局优化 |

**模式检测详细逻辑**：

```cpp
// src/relax/analysis/tir_op_pattern_kind.cc
enum OpPatternKind {
  kElemWise = 0,        // 逐元素：A[i,j] = B[i,j]
  kBroadcast = 1,       // 广播：A[i,j] = B[i]
  kInjective = 2,       // 单射：A[i,j] = B[j,i]
  kCommReduce = 3,      // 归约：A[i] = sum(B[i,j])
  kOutEWiseFusable = 4, // FMA：A[i,j] += B[i,k] * C[j,k]
  kOpaque = 8           // 不透明
};
```

#### 基础 TIR Passes (`src/tirx/transform/`)

| Pass | 作用 |
|------|------|
| **VectorizeLoop** | 将循环转换为向量化 SIMD 指令 |
| **UnrollLoop** | 展开标记的循环或小循环 |
| **StorageRewrite** | 重写存储分配，复用非重叠 buffer |
| **StmtSimplify** | 语句级算术简化 |
| **CommonSubexprElim** | 公共子表达式消除 |
| **NarrowDataType** | 缩窄整数类型（64→32） |
| **FlattenBuffer** | 多维 buffer 访问展平为 1D |
| **RemoveNoOp** | 消除空操作 |
| **SplitHostDevice** | 分离 host/device 代码 |
| **LowerWarpMemory** | 降低 warp 级内存访问 |
| **LowerIntrin** | 降低目标特定函数 intrinsic |
| **MakePackedAPI** | 生成外部 API wrapper |

#### 调度相关 Passes (`src/s_tir/transform/`)

**内存优化**：
| Pass | 作用 |
|------|------|
| **CompactBufferAllocation** | 压缩 buffer 形状到实际访问区域 |
| **MergeSharedMemoryAllocations** | 合并多个共享内存分配 |
| **ManifestSharedMemoryLocalStage** | 添加 shared memory 的 local staging |
| **PlanAndUpdateBufferAllocationLocation** | 移动 buffer 分配到精确的 LCA 位置 |

**循环优化**：
| Pass | 作用 |
|------|------|
| **LoopPartition** | 根据条件分析分割循环 |
| **CanonicalizeLoop** | 规范化循环从 0 开始 |
| **HoistIfThenElse** | 提升循环不变条件到循环外 |
| **InjectDoubleBuffer** | 注入双缓冲重叠计算和内存 |

**Tensorization 和 GPU 特定**：
| Pass | 作用 |
|------|------|
| **InjectSoftwarePipeline** | 将注释循环转换为流水线执行 |
| **InjectPermutedLayout** | 注入无 bank conflict 的交错布局 |
| **TransformMmaBufferLayout** | 转换 MMA buffer scope |
| **InferFragment** | 推断 TensorCore fragment 信息 |
| **InjectPTXAsyncCopy** | 重写 global-to-shared 为 CUDA async copy |
| **LowerAsyncDMA** | 降低 async TIR 原语为 DMA copy/wait |

### 8.5 完整的编译流程（手写 kernel 场景）

```
手写 TIR PrimFunc
    ↓
[Pass 1: TIR 模式分析]
  - AnalyzeOpPatternKind: 遍历每个 Block
    * 分析 load/store 索引关系
    * 检测 FMA 模式 (Matmul)
    * 检测 Reduce 模式 (Softmax)
  - AnnotateTIROpPattern: 标注 op_pattern 属性
    ↓
[Pass 2: DLight 自动调度]
  - 基于 op_pattern 选择 schedule 策略
    * kOutEWiseFusable → Matmul/GEMM schedule → Tensor Core
    * kCommReduce → Reduction schedule → Warp Shuffle
    * kElemWise → Fallback schedule → 向量化
  - 应用 loop tiling, thread binding, shared memory cache
  - 自动 tensorize (WMMA/MMA)
    ↓
[Pass 3: TIR 优化 Pass]
  - LoopPartition: 根据条件分割循环
  - VectorizeLoop: 向量化内存访问 (kElemWise)
  - InjectSoftwarePipeline: 软件流水线 (kOutEWiseFusable)
  - CompactBufferAllocation: 压缩 buffer
  - MergeSharedMemoryAllocations: 合并共享内存
  - InjectPTXAsyncCopy: 异步拷贝
    ↓
[Pass 4: Lowering]
  - LowerIntrin: TIR intrinsic → PTX/LLVM IR
  - SplitHostDevice: 分离 host/device 代码
  - MakePackedAPI: 生成 Python 可调用的 API
    ↓
[Pass 5: 图级优化（在 Relax 层）]
  - FuseOps: 基于 pattern 融合相邻算子
  - StaticPlanBlockMemory: 全局内存规划
  - KillAfterLastUse: 及时释放内存
    ↓
编译后的 .so 库
```

**关键洞察**：

1. **TIR 层模式**（`AnalyzeOpPatternKind`）主要用于**识别**手写 kernel 中的计算模式，而非直接进行融合。这是因为手写 kernel 已经人为完成了最优融合。

2. **模式标注**（`op_pattern` 属性）在 Relax 层的 `FuseOps` 中被读取，用于决定是否可以将该 TIR 函数与其他算子融合：
   ```cpp
   // fuse_ops.cc:191-229
   tir::PrimFunc func = ...;
   auto opt_pattern = func->GetAttr<Integer>("op_pattern");
   OpPatternKind pattern = static_cast<OpPatternKind>(opt_pattern->value);
   // 基于 pattern 决定融合策略
   ```

3. **DLight** 重新分析 TIR block 的访存模式（而非直接读取 `op_pattern`），这是因为：
   - `AnalyzeOpPatternKind` 给出的是整体函数的模式
   - DLight 需要对每个 block 进行更细粒度的分析
   - 两者使用相似的检测逻辑（`IsFMA` 等）

---

## 9. 总结

### 9.1 核心架构

| 层次 | 表示 | 作用 |
|------|------|------|
| **Relax** | 图级 IR | 计算图表示、算子融合、后端分发 |
| **TensorIR** | 张量级 IR | 循环级程序、硬件映射、自动调度 |
| **Runtime** | 虚拟机 | 执行编译后的模块、内存管理 |

### 9.2 关键设计特点

1. **双层 IR 设计**：Relax（图级）+ TensorIR（张量级）
2. **Python-first**：大多数编译变换可在 Python 中自定义
3. **多后端支持**：通过 `src/target/` 模块化后端实现
4. **自动调度**：DLight 启发式调度 + MetaSchedule 搜索调优
5. **BYOC 机制**：集成外部库（CUTLASS、FlashIner、cuDNN）

### 9.3 图优化的核心价值

#### 双层优化体系

| 层次 | 优化目标 | 关键机制 | 代表 Pass/组件 |
|------|----------|----------|----------------|
| **Relax 图级** | 算子融合、后端分发 | 模式匹配 + 后支配树分析 | `FuseOps`, `FuseOpsByPattern` |
| **TIR 级** | Kernel 内部调度、硬件映射 | 访存模式分析 (PatternKindAnalyzer) | `AnalyzeOpPatternKind`, DLight |

#### 模式驱动的优化链

```
用户代码（分解写法）
    ↓
Relax 模式匹配（make_attention_rewrite_pattern）
    ↓
TIR 模式分析（AnalyzeOpPatternKind）
    ↓
    ├─ kElemWise → VectorizeLoop, InjectPTXAsyncCopy
    ├─ kCommReduce → Reduction Schedule, Warp Shuffle
    ├─ kOutEWiseFusable → Matmul Schedule, Tensor Core
    └─ kInjective → Parallelization
    ↓
硬件友好的融合写法
```

#### 关键优化收益

| 优化项 | 技术手段 | 收益 |
|--------|----------|------|
| **消除中间内存** | Online Softmax + StaticPlanBlockMemory | 节省 100% 中间 tensor 内存 |
| **减少 kernel 启动** | FuseOps 融合 4 个 kernel 为 1 个 | 减少 75% 启动开销 |
| **提升计算效率** | DLight Matmul Schedule + Tensor Core | 峰值算力利用率 80%+ |
| **隐藏内存延迟** | InjectSoftwarePipeline + AsyncCopy | 计算与访存重叠 |

**核心观点**：TVM 的图编译器建立了**从高层语义到低层硬件的桥梁**：
1. **Relax 层**通过模式匹配识别高层语义（如 Attention），进行粗粒度融合
2. **TIR 层**通过 `AnalyzeOpPatternKind` 识别计算模式（如 FMA/Reduce），指导细粒度调度
3. **DLight** 基于模式自动应用硬件特定的优化（如 Tensor Core）

用户只需要关注算法正确性和数据流，TVM 会自动完成从**分解写法**到**融合写法**再到**硬件指令**的转换。
