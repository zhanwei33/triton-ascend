# XLA 编译器架构与实现详解

## 目录

- [1. XLA 项目概述](#1-xla-项目概述)
- [2. 代码仓架构](#2-代码仓架构)
- [3. 核心架构组件](#3-核心架构组件)
- [4. 端到端工作流程](#4-端到端工作流程)
- [5. 定制化 Flash Attention 实现](#5-定制化-flash-attention-实现)
- [6. XLA 图优化在 Flash Attention 中的体现](#6-xla-图优化在-flash-attention-中的体现)
- [7. GPU Tensor Core 支持](#7-gpu-tensor-core-支持)
- [8. 关键文件索引](#8-关键文件索引)

---

## 1. XLA 项目概述

XLA (Accelerated Linear Algebra) 是一个开源的机器学习编译器，支持 PyTorch、TensorFlow、JAX 等框架，可将模型编译优化后在 GPU、CPU 和 ML 加速器上高效执行。

### 核心目标

- **提升执行速度**: 编译子图减少短生命周期 op 的执行时间，消除运行时开销
- **优化内存使用**: 分析和调度内存使用，消除中间存储缓冲区
- **减少自定义 op 依赖**: 通过自动融合提升性能，减少手写自定义 op 需求
- **提高可移植性**: 简化新硬件后端开发，使模型无需修改即可运行

---

## 2. 代码仓架构

### 顶层结构

```
xla/
├── build_tools/          # 构建工具和 CI 配置
├── configure.py          # 构建配置脚本
├── docs/                 # 项目文档
├── third_party/          # 第三方依赖 (tsl, rules_python 等)
├── tools/                # 工具链配置
└── xla/                  # 核心源代码
```

### 构建系统

- 使用 **Bazel** 构建 (`.bazelrc`, `BUILD.bazel`, `MODULE.bazel`)
- 支持 CMake (主要用于 mlir_hlo)

---

## 3. 核心架构组件

### 3.1 HLO IR 层 (`xla/service/`)

- **HLO (High Level Optimizer)** 是 XLA 的中间表示
- 包含 400+ 个优化 pass 文件 (algebraic_simplifier, fusion, CSE 等)
- 核心编译流程: `compiler.cc`, `service.cc`
- 包含后端特定优化: `cpu/`, `gpu/` 子目录

### 3.2 PJRT 运行时层 (`xla/pjrt/`)

- **PJRT (Plugin-based JAX Runtime)** - 插件化运行时接口
- 提供硬件无关的编译和执行抽象
- 插件系统: `plugin/xla_gpu/`, `plugin/xla_cpu/`, `plugin/xla_tpu/`
- 支持 C API: `c/`, `c_api_client/`

### 3.3 后端实现 (`xla/backends/`)

- `cpu/` - CPU 后端
- `gpu/` - GPU 后端 (NVIDIA/AMD)
- `interpreter/` - 解释器后端 (用于测试/参考)
- `autotuner/` - 自动调优
- `profiler/` - 性能分析

### 3.4 代码生成 (`xla/codegen/`)

- `emitters/` - IR 发射器
- `intrinsic/` - 内建函数库
- `tiling/` - 分块优化
- `xtile/` - 扩展分块

### 3.5 MLIR 集成 (`xla/mlir/`, `xla/mlir_hlo/`)

- MLIR 方言定义: MHLO, StableHLO, GML_ST
- 转换 passes: `transforms/`
- 工具: mlir_replay, mlir_interpreter, mlir_bisect

### 3.6 Stream Executor (`xla/stream_executor/`)

- 底层硬件抽象层
- 支持 CUDA, ROCm, SYCL, Host

---

## 4. 端到端工作流程

### 4.1 完整编译流程

```
前端框架 (PyTorch/JAX/TF)
    ↓ (导出为 StableHLO)
StableHLO IR
    ↓ (XLA 目标无关优化)
    - CSE (公共子表达式消除)
    - 目标无关操作融合
    - 缓冲分析
    ↓
HLO IR (XLA 内部表示)
    ↓ (后端特定优化)
    - GPU/CPU 定制融合
    - 模式匹配优化库调用
    ↓
LLVM IR 代码生成
    ↓ (LLVM 优化 + 代码生成)
目标机器码 (PTX/x86/ARM)
    ↓
可执行文件
```

### 4.2 AXPY 示例: α×x + y

#### 第1步: 定义计算 (StableHLO)

```mlir
func.func @main(
  %alpha: tensor<f32>, %x: tensor<4xf32>, %y: tensor<4xf32>
) -> tensor<4xf32> {
  %0 = stablehlo.broadcast_in_dim %alpha, dims = []
    : (tensor<f32>) -> tensor<4xf32>
  %1 = stablehlo.multiply %0, %x : tensor<4xf32>
  %2 = stablehlo.add %1, %y : tensor<4xf32>
  func.return %2
}
```

#### 第2步: 创建 PJRT Client

```cpp
std::unique_ptr<PjRtClient> client = GetCpuClient();
```

#### 第3步: 编译

```cpp
TF_ASSERT_OK_AND_ASSIGN(mlir::OwningOpRef<mlir::ModuleOp> program,
                        CreateStableHloProgram("stablehlo_axpy.mlir"));

TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<PjRtLoadedExecutable> executable,
                        client->CompileAndLoad(program, CompileOptions{}));
```

#### 第4步: 执行

```cpp
auto result = executable->Execute({{alpha_buf, x_buf, y_buf}});
// 结果: [13.64, 26.78, 39.92, 53.06]
```

---

## 5. 定制化 Flash Attention 实现

### 5.1 Flash Attention 核心思想

传统 Attention: `Q @ K^T → Softmax → @ V`，需要存储 O(N²) 的中间注意力矩阵。

Flash Attention: 通过分块计算 + 重计算，将内存复杂度降为 O(N)，利用 GPU SRAM 高速缓存。

### 5.2 核心机制: ScoreModFunc

XLA 提供 `ScoreModFunc` 机制，允许在 Flash Attention 的 `Q@K^T` 之后、`Softmax` 之前注入自定义运算。

**关键文件**: `xla/stream_executor/cuda/cudnn_sdpa_score_mod.h`

```cpp
class ScoreModFunc {
  ScoreModFunc(const xla::HloComputation* fwd_comp,  // 前向自定义计算
               const xla::HloComputation* bwd_comp); // 反向传播计算

  Tensor Forward(Graph graph, Tensor attention_score);  // 前向: score → modified_score
  Tensor Backward(Graph graph, Tensor grad);            // 反向: grad_score → grad
};
```

### 5.3 前端定义 (JAX)

#### 场景1: ALiBi (线性偏置)

```python
def alibi_mask(q, k, num_heads, max_positions=8192):
    """ALiBi - 每个头有不同的斜率"""
    seq_len_q, seq_len_k = q.shape[-2], k.shape[-2]
    q_pos = jnp.arange(seq_len_q)
    k_pos = jnp.arange(seq_len_k)
    distance = q_pos[:, None] - k_pos[None, :]
    slopes = jnp.array([2**(-8/h * (h_idx+1)) for h_idx in range(num_heads)])
    bias = slopes[:, None, None] * distance[None, :, :]
    return bias

def flash_attention_with_alibi(q, k, v, alibi_bias):
    attn = jnp.einsum('bhnd,bhmd->bhnm', q, k) / jnp.sqrt(q.shape[-1])
    attn = attn + alibi_bias  # ← 自定义 score 修改
    attn = jax.nn.softmax(attn, axis=-1)
    return jnp.einsum('bhnm,bhmd->bhnd', attn, v)
```

#### 场景2: Softcapping (Gemma)

```python
def flash_attention_with_softcap(q, k, v, softcap=30.0):
    """Gemma 风格: softcapping + causal mask"""
    d = q.shape[-1]
    attn = jnp.einsum('bhnd,bhmd->bhnm', q, k) / jnp.sqrt(d)
    attn = softcap * jnp.tanh(attn / softcap)  # ← 自定义 score 修改
    attn = jax.nn.softmax(attn, axis=-1)
    return jnp.einsum('bhnm,bhmd->bhnd', attn, v)
```

#### 场景3: 自定义稀疏 Mask

```python
def custom_sparse_attention(q, k, v, sparse_mask):
    """自定义稀疏模式: 只允许特定位置间 attention"""
    attn = jnp.einsum('bhnd,bhmd->bhnm', q, k) / jnp.sqrt(q.shape[-1])
    attn = jnp.where(sparse_mask > 0, attn, -1e9)  # ← 自定义 score 修改
    attn = jax.nn.softmax(attn, axis=-1)
    return jnp.einsum('bhnm,bhmd->bhnd', attn, v)
```

### 5.4 StableHLO 表示 (带 called_computations)

```mlir
// 主计算
func.func @main(%q: tensor<4x4x1024x64xbf16>,
                %k: tensor<4x4x1024x64xbf16>,
                %v: tensor<4x4x1024x64xbf16>) -> tensor<4x4x1024x64xbf16> {
  
  %result = "stablehlo.custom_call"(%q, %k, %v) {
    custom_call_target = "__cudnn$fmhaSoftmax",
    called_computations = [@softcap_score_mod],  // ← 关键!
    backend_config = {
      "cudnn_fmha_backend_config": {
        "is_flash_attention": true,
        "mask_type": "CAUSAL",
        "fmha_scale": 0.125,
        "bmm1_dot_dimension_numbers": {...},
        "bmm2_dot_dimension_numbers": {...}
      }
    }
  } : (tensor<4x4x1024x64xbf16>, ...) -> tensor<4x4x1024x64xbf16>

  return %result
}

// ← 自定义 score_mod 计算 (独立 HloComputation)
func.func @softcap_score_mod(%score: tensor<4x4x1024x1024xf32>) 
    -> tensor<4x4x1024x1024xf32> {
  %c30 = stablehlo.constant 30.0 : f32
  %broadcast = stablehlo.broadcast_in_dim %c30, dims = []
    : (tensor<f32>) -> tensor<4x4x1024x1024xf32>
  %div = stablehlo.divide %score, %broadcast
  %tanh = stablehlo.tanh %div
  %result = stablehlo.multiply %broadcast, %tanh
  return %result
}
```

### 5.5 完整 HLO 示例 (Softcapping + Causal + 反向传播)

```hlo
HloModule flash_attention_with_softcap

// 前向 score_mod: softcap
%softcap_fwd (Arg_0: f32[4,4,1024,1024]) -> f32[4,4,1024,1024] {
  %Arg_0 = f32[4,4,1024,1024]{3,2,1,0} parameter(0)
  %c30 = f32[] constant(30)
  %broadcast = f32[4,4,1024,1024]{3,2,1,0} broadcast(%c30), dimensions={}
  %div = f32[4,4,1024,1024]{3,2,1,0} divide(%Arg_0, %broadcast)
  %tanh = f32[4,4,1024,1024]{3,2,1,0} tanh(%div)
  ROOT %result = f32[4,4,1024,1024]{3,2,1,0} multiply(%broadcast, %tanh)
}

// 反向 score_mod: softcap 梯度
%softcap_bwd (Arg_0: f32[4,4,1024,1024], Arg_1: f32[4,4,1024,1024]) 
    -> f32[4,4,1024,1024] {
  %Arg_0 = f32[4,4,1024,1024]{3,2,1,0} parameter(0)  // 前向输出
  %Arg_1 = f32[4,4,1024,1024]{3,2,1,0} parameter(1)  // 上游梯度
  %c30 = f32[] constant(30)
  %broadcast = f32[4,4,1024,1024]{3,2,1,0} broadcast(%c30), dimensions={}
  %div = f32[4,4,1024,1024]{3,2,1,0} divide(%Arg_0, %broadcast)
  %tanh_sq = f32[4,4,1024,1024]{3,2,1,0} multiply(%div, %div)
  %one_minus = f32[4,4,1024,1024]{3,2,1,0} subtract(f32[] constant(1), %tanh_sq)
  %grad_scale = f32[4,4,1024,1024]{3,2,1,0} divide(%one_minus, %broadcast)
  ROOT %grad = f32[4,4,1024,1024]{3,2,1,0} multiply(%Arg_1, %grad_scale)
}

// 主计算
ENTRY %main (q: bf16[4,4,1024,64], k: bf16[4,4,1024,64], v: bf16[4,4,1024,64]) 
    -> bf16[4,4,1024,64] {
  
  %custom-call = (bf16[4,4,1024,64], f32[4,4,1024]{2,1,0}, u8[0]) 
      custom-call(q, k, v),
      custom_call_target = "__cudnn$fmhaSoftmax",
      called_computations = {%softcap_fwd, %softcap_bwd},  // ← 前向+反向
      backend_config = {
        "cudnn_fmha_backend_config": {
          "is_flash_attention": true,
          "mask_type": "CAUSAL",
          "fmha_scale": 0.125,
          ...
        }
      }
  
  ROOT %output = bf16[4,4,1024,64] get-tuple-element(%custom-call), index=0
}
```

### 5.6 XLA 编译 - ScoreModFunc 构建

**文件**: `xla/backends/gpu/transforms/cudnn_custom_call_compiler.cc:183-200`

```cpp
absl::StatusOr<se::gpu::CudnnGraph> BuildGraphForCustomCallToForwardFMHA(
    se::dnn::DnnSupport &dnn_support, HloCustomCallInstruction *custom_call) {
  
  // 1. 提取 called_computations 中的 score_mod
  auto computations = custom_call->called_computations();
  const HloComputation *score_mod_fwd_comp = nullptr;
  std::optional<stream_executor::gpu::ScoreModFunc> score_mod;
  
  if (computations.size() == 1) {
    score_mod_fwd_comp = computations[0];  // @softcap_score_mod
    score_mod.emplace(
        stream_executor::gpu::ScoreModFunc(score_mod_fwd_comp, nullptr));
  }
  
  auto score_mod_ptr = score_mod.has_value() ? &score_mod.value() : nullptr;
  
  // 2. 构建 cuDNN Flash Attention 图，传入 score_mod
  return se::gpu::GetCudnnFlashAttentionOperationGraph(
      dnn_support, q, k, v, output, bias, activation,
      page_table_k, page_table_v,
      static_cast<float>(config.fmha_scale()),
      dropout_rate > 0.0, dropout_rate,
      dnn_mask_type,
      sliding_window_length,
      max_seg_per_batch,
      score_mod_ptr  // ← 自定义运算传入 cuDNN
  );
}
```

### 5.7 cuDNN 层 - ScoreModFunc 编译

**文件**: `xla/stream_executor/cuda/cudnn_sdpa_score_mod.cc`

```cpp
// 将 HloComputation 编译为 cuDNN graph 节点
absl::Status ScoreModFunc::UpdateCudnnMap(
    cudnn_frontend::graph::Graph& graph, UidGenerator next_uid) {
  
  // 遍历 HLO 指令，映射到 cuDNN 操作
  for (const HloInstruction* instr : fwd_comp_->instructions()) {
    switch (instr->opcode()) {
      case HloOpcode::kMultiply:
        hlo_to_cudnn_[instr] = graph->tensor(...MUL...);
        break;
      case HloOpcode::kTanh:
        hlo_to_cudnn_[instr] = graph->tensor(...TANH...);
        break;
      case HloOpcode::kDivide:
        hlo_to_cudnn_[instr] = graph->tensor(...DIV...);
        break;
    }
  }
  return absl::OkStatus();
}

// 前向传播: 接收 attention_score，返回修改后的 score
Tensor ScoreModFunc::Forward(Graph graph, Tensor attention_score) {
  fwd_hlo_to_cudnn_[fwd_comp_->parameter_instruction(0)] = attention_score;
  return Compile(graph, fwd_hlo_to_cudnn_, fwd_comp_);
}

// 反向传播: 接收梯度，返回修改后的梯度
Tensor ScoreModFunc::Backward(Graph graph, Tensor grad) {
  return Compile(graph, bwd_hlo_to_cudnn_, bwd_comp_);
}
```

### 5.8 支持的 Mask 类型

| Mask 类型 | backend_config | 说明 |
|-----------|----------------|------|
| 无 Mask | `"mask_type": "NO_MASK"` | 全注意力 |
| Causal | `"mask_type": "CAUSAL"` | 上三角掩码 |
| Padding | `"mask_type": "PADDING"` | 序列长度掩码 |
| PADDING_CAUSAL | `"mask_type": "PADDING_CAUSAL"` | 两者结合 |
| 滑动窗口 | `"mask_type": "CAUSAL", "sliding_window_length": 4096` | Mistral 风格 |
| ALiBi | `"mask_type": "ALIBI"` | 线性偏置 |
| 自定义 (score_mod) | `called_computations = [@custom_mod]` | 任意函数 |

---

## 6. XLA 图优化在 Flash Attention 中的体现

### 6.1 优化前: 原始 HLO 图

前端框架（JAX/PyTorch）生成的原始计算图包含多个独立的 HLO 指令，每个指令对应一个单独的 GPU kernel：

```
前端生成的计算图:

  Q[4,4,1024,64]    K[4,4,1024,64]
        │                  │
        └────┬─────┬───────┘
             │     │ (transpose K)
             ▼     ▼
           ┌──────────┐
           │   dot    │  ← BMM1: Q @ K^T
           └────┬─────┘
                │ [4,4,1024,1024]  ← O(N²) 中间张量!
                ▼
           ┌──────────┐
           │ multiply │  ← scale (1/sqrt(d))
           └────┬─────┘
                ▼
           ┌──────────┐
           │ softmax  │  ← 按行 softmax
           └────┬─────┘
                │ [4,4,1024,1024]  ← 又一个 O(N²) 中间张量!
                ▼
         ┌──────┴──────┐
         │             ▼
         │        V[4,4,1024,64]
         │             │
         ▼             ▼
       ┌──────────────────┐
       │       dot        │  ← BMM2: attn @ V
       └────────┬─────────┘
                │ [4,4,1024,64]
                ▼
            输出结果

内存峰值: 2 × O(N²) = 2 × 4×4×1024×1024 × 2bytes ≈ 128MB (BF16)
Kernel 数量: 5+ (transpose, dot, multiply, softmax, dot)
全局内存读写: 每个中间结果都要写回 HBM 再读回
```

**问题分析**：
- **内存瓶颈**: 需要存储注意力矩阵 S = Q@K^T 和 softmax 输出两个 O(N²) 张量
- **Kernel 启动开销**: 5+ 个独立 kernel 的启动和同步开销
- **带宽瓶颈**: 每个中间结果都需要从 HBM 写入再读出（4 次 O(N²) 数据搬运）

### 6.2 优化阶段 1: 模式匹配与 FMHA 融合

**Pass**: `xla/service/gpu/cublas_cudnn.h:174-205` 定义了 FMHA 模式，实际的 pattern matching 和重写由前端（JAX/PyTorch）或专门的 rewriter pass 完成。

**支持的融合模式** (`CudnnfMHAKind` 枚举定义):
| 模式 | Call Target | 说明 |
|------|-------------|------|
| `kSoftmax` | `__cudnn$fmhaSoftmax` | BMM1 → Softmax → BMM2 |
| `kSoftmaxDropout` | `__cudnn$fmhaSoftmaxDropout` | 增加 Dropout |
| `kScaleBiasSoftmax` | `__cudnn$fmhaScaleBiasSoftmax` | 增加 Scale + Bias |
| `kScaleBiasSoftmaxDropout` | `__cudnn$fmhaScaleBiasSoftmaxDropout` | 完整版本 |
| `kSoftmaxF8` | `__cudnn$fmhaSoftmaxF8` | FP8 量化版本 |

**优化后 HLO 表示**:

```hlo
// 融合前 (8+ 个独立指令，5+ 个 kernel)
%dot1 = bf16[4,4,1024,1024] dot(Q, transpose(K))
%scaled = bf16[4,4,1024,1024] multiply(%dot1, scale)
%attn = bf16[4,4,1024,1024] softmax(%scaled)
%output = bf16[4,4,1024,64] dot(%attn, V)

// 融合后 (1 个 custom-call，1 个 kernel)
%fmha = (bf16[4,4,1024,64], u8[0]) custom-call(Q, K, V),
  custom_call_target="__cudnn$fmhaSoftmax",
  backend_config={
    "cudnn_fmha_backend_config": {
      "algorithm": {
        "algo_id": "0",
        "math_type": "TENSOR_OP_MATH",
        "tuning_knobs": {"17": "1", "24": "0"},
        "workspace_size": "0"
      },
      "fmha_scale": 0.125,
      "mask_type": "CAUSAL",
      "is_flash_attention": true,
      "is_causal_mask": false,
      "bmm1_dot_dimension_numbers": {...},
      "bmm2_dot_dimension_numbers": {...},
      "intermediate_tensor_shape": {...}
    }
  }
%output = bf16[4,4,1024,64] get-tuple-element(%fmha), index=0
```

**优化效果**:
- **内存**: O(N²) 中间张量变为虚拟，不分配 HBM 内存
- **Kernel**: 5+ → 1 (Flash Attention 融合 kernel)
- **内存带宽**: 消除 2 次 O(N²) 的全局内存读写，降至 ~2×O(N)

### 6.3 优化阶段 2: ScoreMod 自定义运算内联

**核心机制**: `ScoreModFunc` 类 (`xla/stream_executor/cuda/cudnn_sdpa_score_mod.h:32`) 实现了 HLO computation 到 cuDNN graph 的编译。

**源码分析**:

**A. ScoreModFunc 构造函数与成员变量** (`cudnn_sdpa_score_mod.h:32-64`):

```cpp
class ScoreModFunc {
 public:
  // fwd_comp: 前向传播 score modification 计算
  // bwd_comp: 反向传播梯度计算（可选）
  ScoreModFunc(const xla::HloComputation* fwd_comp,
               const xla::HloComputation* bwd_comp);

  // 将 HLO 参数和常量映射到 cuDNN tensor
  absl::Status UpdateCudnnMap(cudnn_frontend::graph::Graph& graph,
                              UidGenerator next_uid);

  // 前向: 接收 attention_score，返回修改后的 score
  Tensor Forward(Graph graph, Tensor attention_score);

  // 反向: 接收梯度，返回修改后的梯度
  Tensor Backward(Graph graph, Tensor grad);

 private:
  std::vector<Tensor> fwd_parameters_;  // 保存前向参数供反向使用
  const xla::HloComputation* fwd_comp_;
  const xla::HloComputation* bwd_comp_;
  absl::flat_hash_map<const xla::HloInstruction*, Tensor> fwd_hlo_to_cudnn_;
  absl::flat_hash_map<const xla::HloInstruction*, Tensor> bwd_hlo_to_cudnn_;
};
```

**B. ScoreMod 提取与构建** (`cudnn_custom_call_compiler.cc:182-201`):

```cpp
absl::StatusOr<se::gpu::CudnnGraph> BuildGraphForCustomCallToForwardFMHA(
    se::dnn::DnnSupport &dnn_support, HloCustomCallInstruction *custom_call) {
  // ... 提取 Q, K, V, output 等 tensor 描述 ...

  auto computations = custom_call->called_computations();
  const HloComputation *score_mod_fwd_comp = nullptr;
  std::optional<stream_executor::gpu::ScoreModFunc> score_mod;

  TF_RET_CHECK(computations.size() <= 1);
  if (computations.size() == 1) {
    score_mod_fwd_comp = computations[0];  // @softcap_score_mod
    score_mod.emplace(
        stream_executor::gpu::ScoreModFunc(score_mod_fwd_comp, nullptr));
    // 调整 input_index 以考虑 score_mod 的额外参数
    input_index += score_mod_fwd_comp->num_parameters() - 1;
  }
  auto score_mod_ptr = score_mod.has_value() ? &score_mod.value() : nullptr;

  // 构建 cuDNN Flash Attention 图，传入 score_mod
  return se::gpu::GetCudnnFlashAttentionOperationGraph(
      dnn_support, q, k, v, output, bias, activation,
      page_table_k, page_table_v,
      static_cast<float>(config.fmha_scale()),
      dropout_rate > 0.0, dropout_rate,
      dnn_mask_type, sliding_window_length, max_seg_per_batch,
      score_mod_ptr  // ← 自定义运算传入 cuDNN
  );
}
```

**C. HLO 到 cuDNN 的编译过程** (`cudnn_sdpa_score_mod.cc:267-373`):

```cpp
Tensor ScoreModFunc::Compile(
    Graph graph,
    absl::flat_hash_map<const xla::HloInstruction*, Tensor>& hlo_to_cudnn,
    const xla::HloComputation* computation) {
  // 后序遍历 HLO 指令
  std::vector<xla::HloInstruction*> instructions =
      computation->MakeInstructionPostOrder();

  for (const xla::HloInstruction* hlo : instructions) {
    auto operand = [&hlo_to_cudnn, &hlo](int i) {
      return hlo_to_cudnn[hlo->operand(i)];
    };

    // 处理常量参数
    if (xla::HloPredicateIsOp<xla::HloOpcode::kConstant,
                              xla::HloOpcode::kParameter>(hlo)) {
      continue;
    }

    // 处理广播和 bitcast（虚拟操作）
    if (xla::HloPredicateIsOp<xla::HloOpcode::kBitcast,
                              xla::HloOpcode::kBroadcast>(hlo)) {
      hlo_to_cudnn[hlo] = operand(0);
    }
    // 处理 Iota 操作（生成序列）
    else if (xla::HloPredicateIsOp<xla::HloOpcode::kIota>(hlo)) {
      auto attrs = cudnn_frontend::graph::Pointwise_attributes()
                       .set_mode(cudnn_frontend::PointwiseMode_t::GEN_INDEX)
                       .set_compute_data_type(cudnn_frontend::DataType_t::INT32)
                       .set_axis(iota->iota_dimension());
      hlo_to_cudnn[hlo] = graph->pointwise(..., attrs);
    }
    // 处理逐元素操作（核心）
    else if (hlo->IsElementwise()) {
      const auto mode = GetElementwiseMode(*hlo);  // HLO opcode → cuDNN mode
      auto attrs = cudnn_frontend::graph::Pointwise_attributes()
                       .set_mode(mode.value())
                       .set_compute_data_type(compute_dtype)
                       .set_name(std::string(hlo->name()));

      if (hlo->operand_count() == 1) {
        // 一元操作: tanh, exp, etc.
        hlo_to_cudnn[hlo] = graph->pointwise(operand(0), attrs);
      } else if (hlo->operand_count() == 2) {
        // 二元操作: add, multiply, divide, etc.
        // 确保第一个操作数是 virtual（cuDNN 限制）
        hlo_to_cudnn[hlo] = graph->pointwise(o0, o1, attrs);
      } else if (hlo->operand_count() == 3) {
        // 三元操作: select
        hlo_to_cudnn[hlo] = graph->pointwise(operand(1), operand(2), operand(0), attrs);
      }
    }
  }
  return hlo_to_cudnn[computation->root_instruction()];
}
```

**D. 支持的逐元素操作映射** (`cudnn_sdpa_score_mod.cc:45-117`):

| HLO Opcode | cuDNN Pointwise Mode | 说明 |
|------------|---------------------|------|
| `kAdd` | `ADD` | 加法 |
| `kMultiply` | `MUL` | 乘法 |
| `kDivide` | `DIV` | 除法 |
| `kTanh` | `TANH_FWD` | 双曲正切 |
| `kExp` | `EXP` | 指数 |
| `kLog` | `LOG` | 对数 |
| `kMaximum` | `MAX` | 最大值 |
| `kMinimum` | `MIN` | 最小值 |
| `kCompare` | `CMP_*` | 比较操作 |
| `kSelect` | `BINARY_SELECT` | 条件选择 |
| `kRsqrt` | `RSQRT` | 反平方根 |
| `kAnd`/`kOr` | `LOGICAL_AND`/`OR` | 逻辑运算 |

**E. ScoreMod 内联的核心优势**:

```
传统方式（无 ScoreMod 内联）:
┌─────────┐     ┌──────────────┐     ┌─────────┐
│ BMM1    │────→│ 全局内存存储 │────→│ ScoreMod│  ← 独立 kernel
│ (SRAM)  │     │ (HBM, O(N²)) │     │ (HBM)   │
└─────────┘     └──────────────┘     └────┬────┘
                                          │
                                     ┌────▼────┐
                                     │ 全局内存│
                                     │ (HBM)   │
                                     └────┬────┘
                                          │
┌─────────┐     ┌──────────────┐     ┌────▼────┐
│ BMM2    │←────│ Softmax      │←────│ 读取    │
│ (SRAM)  │     │ (SRAM/HBM)   │     │ (HBM)   │
└─────────┘     └──────────────┘     └─────────┘

ScoreMod 内联后（SRAM 内计算）:
┌─────────────────────────────────────────────────┐
│  for each tile in Q:                            │
│    for each tile in K:                          │
│      S_tile = Q_tile @ K_tile^T   ← BMM1 (SRAM) │
│      S_tile = S_tile * scale      ← scale       │
│      S_tile = softcap(S_tile)     ← ScoreMod!   │
│      S_tile = S_tile + mask       ← mask        │
│      P_tile = softmax(S_tile)     ← softmax     │
│      O_tile += P_tile @ V_tile    ← BMM2 (SRAM) │
│    end                                          │
│  end                                            │
└─────────────────────────────────────────────────┘
         ↓ 仅输出最终结果 O (HBM)
```

**内存访问对比**:

| 方案 | HBM 读取 | HBM 写入 | 中间存储 |
|------|---------|---------|---------|
| 无 ScoreMod 内联 | 2×O(N²) + O(N) | 2×O(N²) | 2×O(N²) |
| **ScoreMod 内联** | **O(N)** | **O(N)** | **0** |

### 6.4 优化阶段 3: CuDNN Custom Call → Fusion 转换

**Pass**: `xla/service/gpu/transforms/cudnn_custom_call_converter.cc:34-54`

此 pass 负责将 cuDNN custom-call 包装为 kCustom fusion，便于后续优化。

```cpp
class CustomCallVisitor : public DfsHloRewriteVisitor {
  absl::Status HandleCustomCall(HloInstruction *hlo) override {
    if (hlo->custom_call_target() != kCuDnnFusionKind) {
      return absl::OkStatus();
    }

    // 克隆 called_computation 作为 embedded computation
    HloComputation *computation = hlo->GetModule()->AddEmbeddedComputation(
        hlo->called_computations()[0]->Clone());

    // 创建 kCustom fusion 指令
    HloInstruction *fusion =
        hlo->parent()->AddInstruction(HloInstruction::CreateFusion(
            hlo->shape(), HloInstruction::FusionKind::kCustom,
            hlo->operands(), computation));

    // 设置 backend config
    GpuBackendConfig gpu_config;
    FusionBackendConfig &backend_config =
        *gpu_config.mutable_fusion_backend_config();
    backend_config.set_kind(hlo->custom_call_target());
    TF_RETURN_IF_ERROR(fusion->set_backend_config(gpu_config));

    // 替换原指令
    TF_RETURN_IF_ERROR(ReplaceInstruction(hlo, fusion));
    return absl::OkStatus();
  }
};
```

**执行时机**: 在 `pre_spmd_pipeline` 早期执行（`gpu_compiler.cc:449`）。

### 6.5 优化阶段 4: 自动调优 (Autotuning)

**Pass**: `xla/service/gpu/autotuning/autotuner_pass.cc:92-139`

自动调优器通过实际运行测量来选择最优的 cuDNN 算法配置。

**调优配置** (`CudnnfMHABackendConfig`):

```protobuf
message CudnnfMHABackendConfig {
  // cuDNN 算法选择和调优参数
  stream_executor.dnn.AlgorithmProto algorithm = 8;

  // Flash Attention 缩放因子
  double fmha_scale = 10;

  // Dropout 配置
  double dropout_rate = 13;
  int64 seed = 15;

  // BMM 维度配置
  xla.DotDimensionNumbers bmm1_dot_dimension_numbers = 11;
  xla.DotDimensionNumbers bmm2_dot_dimension_numbers = 12;

  // 中间张量形状（用于反向传播）
  xla.ShapeProto intermediate_tensor_shape = 14;

  // Flash Attention 标志
  bool is_flash_attention = 20;
  bool is_causal_mask = 21;

  // Mask 类型枚举
  enum MaskType {
    NO_MASK = 0;
    PADDING = 1;
    CAUSAL = 2;
    PADDING_CAUSAL = 3;
    ALIBI = 4;
  }
  MaskType mask_type = 22;

  // 性能调优参数
  bool force_deterministic = 23;
  int32 sliding_window_length = 24;  // Mistral 风格滑动窗口
  int32 max_seg_per_batch = 25;      // Packed layout 支持
  bool is_paged_attention = 26;      // vLLM 风格分页注意力
}
```

**算法配置示例**:

```json
{
  "algorithm": {
    "algo_id": "0",           // cuDNN 算法 ID
    "math_type": "TENSOR_OP_MATH",  // 使用 Tensor Core
    "tuning_knobs": {
      "17": "1",              // block size 配置
      "24": "0"               // pipeline 阶段数
    },
    "workspace_size": "0"     // 工作空间大小
  },
  "fmha_scale": 0.125,
  "mask_type": "CAUSAL",
  "is_flash_attention": true,
  "sliding_window_length": 4096  // 可选：滑动窗口
}
```

### 6.6 优化阶段 5: 布局优化 (Layout Assignment)

**Pass**: `xla/service/gpu/transforms/layout_assignment.cc`

针对 cuDNN Flash Attention 的要求，优化 tensor 布局以减少转置开销。

```hlo
// 优化前 (默认布局 {3,2,1,0} - minor-to-major)
Q: bf16[4,4,1024,64]{3,2,1,0}
K: bf16[4,4,1024,64]{3,2,1,0}

// 优化后 (针对 cuDNN 优化的布局)
Q: bf16[4,4,1024,64]{3,1,2,0}  // 调整维度顺序
K: bf16[4,4,1024,64]{3,1,2,0}  // 匹配 cuDNN 期望
```

### 6.7 优化阶段 6: 代数化简与常量折叠

**Pass**: `xla/service/gpu/transforms/algebraic_simplifier.cc`

```hlo
// 优化前：broadcast + multiply 组合
%scale = bf16[] constant(0.125)
%broadcast = bf16[4,4,1024,1024] broadcast(%scale), dimensions={}
%scaled = bf16[4,4,1024,1024] multiply(%dot1, %broadcast)

// 优化后：直接传递标量 scale 给 backend config
// broadcast + multiply 被完全消除
backend_config: {
  "cudnn_fmha_backend_config": {
    "fmha_scale": 0.125  // 标量直接传入 cuDNN
  }
}
```

### 6.8 完整优化流程与数据流

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         XLA 编译管线 (GPU Backend)                          │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  1. 前端 HLO                                                                 │
│     └── dot → multiply → softmax → dot (8+ 指令，5+ kernels)                │
│                                                                             │
│  2. Pattern Matching & Fusion                                               │
│     └── custom-call(target="__cudnn$fmhaSoftmax", ...)                       │
│                                                                             │
│  3. ScoreMod 提取与编译                                                      │
│     └── called_computations=[@score_mod]                                    │
│     └── ScoreModFunc::UpdateCudnnMap() → cuDNN graph nodes                  │
│                                                                             │
│  4. CuDNN Custom Call Converter                                             │
│     └── kCustom fusion (便于后续优化)                                        │
│                                                                             │
│  5. Layout Assignment                                                       │
│     └── 优化 tensor layout 匹配 cuDNN 要求                                   │
│                                                                             │
│  6. Autotuning                                                              │
│     └── 选择最优 algorithm + tuning_knobs                                   │
│                                                                             │
│  7. Algebraic Simplification                                                │
│     └── 消除冗余 broadcast/multiply                                         │
│                                                                             │
│  8. IR 发射与代码生成                                                         │
│     └── CuDNN thunk (Flash Attention kernel)                                │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 6.9 优化效果总结

| 指标 | 优化前 | 优化后 | 改善倍数 |
|------|--------|--------|---------|
| **HLO 指令数** | 8+ | 1 | **8×** |
| **GPU Kernel 数** | 5+ | 1 | **5×** |
| **中间内存** | 2 × O(N²) ≈ 128MB | 0 (虚拟张量) | **∞** |
| **HBM 读写** | ~4 × O(N²) | ~2 × O(N) | **~2000×** (seq=1024) |
| **自定义运算** | 独立 kernel | SRAM 内联 | **消除 kernel 启动开销** |
| **Mask 处理** | 独立 multiply/add | cuDNN 内置 + ScoreMod | **融合到 SRAM 循环** |

**注**: HBM 读写改善倍数与序列长度相关。对于 seq_len=1024，改善约为 (4×1024²)/(2×1024) = 2000×；更大的序列长度带来更显著的改善。

---

## 7. GPU Tensor Core 支持

### 7.1 Tensor Core 硬件检测

**文件**: `xla/stream_executor/cuda/cuda_compute_capability.h:190`

```cpp
enum class TensorCoreGeneration {
  kNone,        // 无 Tensor Core (sm_50 以下)
  kFirstGen,    // Volta (sm_70) - FP16 Tensor Core
  kSecondGen,   // Turing (sm_75) - INT8/FP16
  kThirdGen,    // Ampere (sm_80/86) - TF32/BF16/FP64
  kFourthGen,   // Hopper (sm_90) - FP8/WGMMA
  kFifthGen,    // Blackwell (sm_100) - tcgen05
};

CudaComputeCapability cc = stream->GetCudaComputeCapability();
if (cc.major < 7) {
  // Tensor Core 不可用 (sm_50/60/61)
} else if (cc.major >= 8) {
  // Ampere+ 支持 TF32 Tensor Core
}
```

### 7.2 cuBLAS 集成 - Math Mode 设置

**文件**: `xla/stream_executor/cuda/cuda_blas.cc:528-540`

```cpp
absl::Status CUDABlas::DoBlasGemm(..., blas::DataType dtype, ...) {
  cublasMath_t math_type = CUBLAS_DEFAULT_MATH;

#if CUDA_VERSION < 11000
  // CUDA 11 之前: FP16 自动使用 Tensor Core
  if (dtype == blas::DataType::kHalf) {
    math_type = CUBLAS_TENSOR_OP_MATH;  // ← 启用 Tensor Core
  }
#else
  // CUDA 11+: FP32 可使用 TF32 Tensor Core
  if (dtype == blas::DataType::kFloat) {
    math_type = CUBLAS_TF32_TENSOR_OP_MATH;  // ← TF32 Tensor Core
    if (!engine_options.allow_tf32) {
      math_type = CUBLAS_DEFAULT_MATH;  // 用户禁用 TF32
    }
  }
#endif

  return DoBlasInternalImpl(
      cublasSgemmEx, stream, true, math_type,  // ← math_type 传入
      transa, transb, m, n, k, alpha, a, CUDA_R_16F, lda, ...);
}
```

### 7.3 GEMM 算法选择与 Tensor Core

**文件**: `xla/stream_executor/cuda/cuda_blas.cc:650-700`

```cpp
cublasMath_t GetMathTypeForGemmEx(
    Stream* stream, blas::AlgorithmType algorithm, 
    blas::DataType type_a, blas::DataType type_b, ...) {
  
  bool algo_uses_tensor_ops = UsesTensorOps(algorithm);
  
  if (algo_uses_tensor_ops) {
    if (cc.major < 7) {
      return Error("Tensor ops not available on sm_6x");
    } else if (type_a == blas::DataType::kFloat) {
      math_type = CUBLAS_TF32_TENSOR_OP_MATH;  // FP32 → TF32 Tensor Core
    } else if (type_a == blas::DataType::kHalf) {
      math_type = CUBLAS_TENSOR_OP_MATH;       // FP16 → FP16 Tensor Core
    }
  }
  return math_type;
}
```

**支持的 Tensor Core 数据类型组合** (`matmul_utils.cc:1036-1042`):

```cpp
TYPED_GEMM(F32, BF16, BF16, BF16)   // BF16 输入, BF16 输出 (Tensor Core)
TYPED_GEMM(F32, F16, F16, F16)      // FP16 输入, FP16 输出 (Tensor Core)
TYPED_GEMM(F32, S8, S8, F32)        // INT8 输入, FP32 输出 (Tensor Core IMMA)
TYPED_GEMM(F32, BF16, BF16, F32)    // BF16 输入, FP32 累加 (Tensor Core)
TYPED_GEMM(F32, F16, F16, F32)      // FP16 输入, FP32 累加 (Tensor Core)
TYPED_GEMM(F32, F32, F32, F32)      // FP32 → TF32 Tensor Core (可选)
```

### 7.4 Dot Algorithm Rewriter - 精度提升技术

**文件**: `xla/backends/gpu/transforms/dot_algorithm_rewriter.cc`

XLA 提供多种精度算法，利用 Tensor Core 实现高精度计算：

#### TF32 单精度 (1x Tensor Core)

```cpp
// ALG_DOT_TF32_TF32_F32: F32 → TF32 → Tensor Core → F32
void RewriteF32ToTF32(HloInstruction* instr) {
  constexpr uint32_t kMaskTF32 = 0xFFFFE000;  // 清零低 13 位
  
  auto lhs_tf32 = Truncate(lhs, kMaskTF32);
  auto rhs_tf32 = Truncate(rhs, kMaskTF32);
  
  result = Dot(lhs_tf32, rhs_tf32, ALG_DOT_TF32_TF32_F32);
}
```

#### TF32 三倍精度 (3x Tensor Core)

```cpp
// ALG_DOT_TF32_TF32_F32_X3: 3 次 TF32 Tensor Core 调用
void RewriteF32ToTF32X3(HloInstruction* instr) {
  auto [lhs_high, lhs_low] = Split2xToTF32(lhs);
  auto [rhs_high, rhs_low] = Split2xToTF32(rhs);
  
  auto high_high = Dot(lhs_high, rhs_high);  // 主项
  auto low_high  = Dot(lhs_low, rhs_high);   // 交叉项
  auto high_low  = Dot(lhs_high, rhs_low);   // 交叉项
  
  result = high_high + low_high + high_low;  // F32 精度累加
}
```

#### BF16 多倍精度 (X3/X6/X9 Tensor Core)

```cpp
// ALG_DOT_BF16_BF16_F32_X3: 3 次 BF16 Tensor Core
void RewriteF32ToBF16X3(HloInstruction* instr) {
  auto [lhs_high, lhs_low] = Split2xToBF16(lhs);
  auto [rhs_high, rhs_low] = Split2xToBF16(rhs);
  
  auto high_high = Dot(lhs_high, rhs_high);
  auto low_high  = Dot(lhs_low, rhs_high);
  auto high_low  = Dot(lhs_high, rhs_low);
  
  result = high_high + low_high + high_low;
}

// ALG_DOT_BF16_BF16_F32_X9: 9 次 BF16 Tensor Core (最高精度)
void RewriteF32ToBF16X9(HloInstruction* instr) {
  auto [lhs_h, lhs_m, lhs_l] = Split3xToBF16(lhs);
  auto [rhs_h, rhs_m, rhs_l] = Split3xToBF16(rhs);
  
  // 9 次 BF16 Tensor Core GEMM (所有组合)
  result = Dot(l_h, r_h) + Dot(l_h, r_m) + Dot(l_h, r_l)
         + Dot(l_m, r_h) + Dot(l_m, r_m) + Dot(l_m, r_l)
         + Dot(l_l, r_h) + Dot(l_l, r_m) + Dot(l_l, r_l);
}
```

**精度对比**:

| 算法 | Tensor Core 调用次数 | 有效精度 | 性能 |
|------|---------------------|----------|------|
| `ALG_DOT_TF32_TF32_F32` | 1x | ~10 位 | 最快 |
| `ALG_DOT_TF32_TF32_F32_X3` | 3x | ~19 位 | 中等 |
| `ALG_DOT_BF16_BF16_F32` | 1x | ~7 位 | 快 |
| `ALG_DOT_BF16_BF16_F32_X3` | 3x | ~14 位 | 中等 |
| `ALG_DOT_BF16_BF16_F32_X6` | 6x | ~21 位 | 较慢 |
| `ALG_DOT_BF16_BF16_F32_X9` | 9x | ~28 位 | 最慢 |

### 7.5 TF32 允许性检查

**文件**: `xla/service/gpu/matmul_utils.cc:411-418`

```cpp
bool IsTf32Allowed(PrecisionConfig::Algorithm algorithm,
                   int64_t compute_precision) {
  if (algorithm == PrecisionConfig::ALG_UNSET) {
    return compute_precision <= 1;  // 默认行为: compute_precision <= 1 允许 TF32
  }
  return algorithm_util::HasTf32InputType(algorithm);
}
```

**环境变量控制**:

```bash
# 启用 TF32 (默认)
export XLA_FLAGS=--xla_gpu_enable_tf32

# 禁用 TF32 (需要完整 FP32 精度)
export XLA_FLAGS=--xla_gpu_disable_tf32
```

### 7.6 完整数据流: 从前端到 Tensor Core

```
前端 (JAX/PyTorch)
    ↓
HLO: dot(f32[4096,4096], f32[4096,4096])
    ↓
[DotAlgorithmRewriter]
    ↓
HLO: dot(bf16[4096,4096], bf16[4096,4096]) 
     precision_algorithm = ALG_DOT_BF16_BF16_F32
    ↓
[Autotuner] 选择最优 cuBLAS 算法
    ↓
GemmBackendConfig: {
  "selected_algorithm": "CUBLAS_GEMM_DFALT_TENSOR_OP",
  "precision_config": {"algorithm": "ALG_DOT_BF16_BF16_F32"}
}
    ↓
[cuda_blas.cc]
    math_type = CUBLAS_TENSOR_OP_MATH
    cublasSetMathMode(handle, CUBLAS_TENSOR_OP_MATH)
    ↓
[cuBLAS] cublasGemmEx(..., CUDA_R_16BF, ..., CUBLAS_COMPUTE_32F, ...)
    ↓
[NVIDIA Driver] → GPU Tensor Core (BF16 输入, FP32 累加)
    ↓
结果: f32[4096,4096] (FP32 精度, Tensor Core 加速)
```

---

## 8. 关键文件索引

### 8.1 核心编译

| 文件 | 作用 |
|------|------|
| `xla/service/compiler.cc` | 主编译流程 |
| `xla/service/service.cc` | XLA 服务入口 |
| `xla/backends/gpu/gpu_compiler.cc` | GPU 编译器 |

### 8.2 HLO 优化 Passes

| 文件 | 作用 |
|------|------|
| `xla/service/algebraic_simplifier.cc` | 代数化简 |
| `xla/service/hlo_cse.cc` | 公共子表达式消除 |
| `xla/backends/gpu/transforms/dot_algorithm_rewriter.cc` | Dot 精度算法重写 |
| `xla/backends/gpu/transforms/layout_assignment.cc` | 布局分配 |

### 8.3 Flash Attention / FMHA

| 文件 | 作用 |
|------|------|
| `xla/service/gpu/cublas_cudnn.h:205-238` | FMHA 模式定义 |
| `xla/stream_executor/cuda/cudnn_sdpa_score_mod.h` | ScoreModFunc 类定义 |
| `xla/stream_executor/cuda/cudnn_sdpa_score_mod.cc` | HLO→cuDNN 映射 |
| `xla/backends/gpu/transforms/cudnn_custom_call_compiler.cc` | 提取 called_computations |
| `xla/backends/gpu/transforms/cudnn_custom_call_converter.cc` | custom-call → fusion |
| `xla/stream_executor/cuda/cuda_dnn.cc:3959-4161` | cuDNN Flash Attention API |
| `xla/service/gpu/backend_configs.proto:352-382` | mask_type 等配置 |
| `xla/backends/gpu/tests/gpu_fused_mha_test.cc` | 各种 FA 变体测试 |

### 8.4 Tensor Core / GEMM

| 文件 | 作用 |
|------|------|
| `xla/stream_executor/cuda/cuda_compute_capability.h` | Tensor Core 代际检测 |
| `xla/stream_executor/cuda/cuda_blas.cc:528-700` | Math mode 设置 |
| `xla/backends/gpu/transforms/dot_algorithm_rewriter.cc` | TF32/BF16 多倍精度 |
| `xla/service/gpu/matmul_utils.cc:411-1042` | TF32 允许性、类型分发 |
| `xla/service/gpu/autotuning/autotuner_pass.cc` | 选择最优 Tensor Core 算法 |
| `xla/service/gpu/model/gpu_dot_fusion_cost_model.cc` | Tensor Core 性能估计 |

### 8.5 PJRT 运行时

| 文件 | 作用 |
|------|------|
| `xla/pjrt/pjrt_client.h` | PJRT Client 接口 |
| `xla/pjrt/pjrt_executable.h` | 可执行文件接口 |
| `xla/pjrt/plugin/xla_gpu/` | GPU 插件 |
| `xla/pjrt/plugin/xla_cpu/` | CPU 插件 |

### 8.6 示例

| 文件 | 作用 |
|------|------|
| `xla/examples/axpy/stablehlo_axpy.mlir` | AXPY StableHLO 示例 |
| `xla/examples/axpy/stablehlo_compile_test.cc` | AXPY 编译测试 |
