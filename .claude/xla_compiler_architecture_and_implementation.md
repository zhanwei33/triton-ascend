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

### 6.2 优化阶段 1: 模式匹配与 FMHA 融合

**Pass**: `fused_mha_rewriter` (`xla/service/gpu/cublas_cudnn.h:205`)

**支持的融合模式**:
1. `BMM1 → Softmax → BMM2`
2. `BMM1 → Softmax → Dropout → BMM2`
3. `BMM1 → Scale → Bias → Softmax → BMM2`
4. `BMM1 → Scale → Bias → Softmax → Dropout → BMM2`

**优化后**:

```hlo
// 融合前 (5+ 个独立指令)
%dot1 = dot(Q, transpose(K))
%scaled = multiply(%dot1, scale)
%attn = softmax(%scaled)
%output = dot(%attn, V)

// 融合后 (1 个 custom-call)
%fmha = custom-call(Q, K, V) {
  custom_call_target = "__cudnn$fmhaSoftmax"
  backend_config = {
    "cudnn_fmha_backend_config": {
      "is_flash_attention": true,
      "mask_type": "CAUSAL",
      "fmha_scale": 0.125,
      ...
    }
  }
}
```

**优化效果**:
- **内存**: O(N²) 中间张量变为虚拟，不分配 HBM
- **Kernel**: 5+ → 1 (Flash Attention 融合 kernel)
- **内存带宽**: 消除 2 次 O(N²) 的全局内存读写

### 6.3 优化阶段 2: ScoreMod 自定义运算内联

**Pass**: `cudnn_custom_call_compiler.cc:183-200`

```cpp
// 提取 called_computations 中的自定义 score_mod
if (computations.size() == 1) {
  score_mod_fwd_comp = computations[0];
  score_mod.emplace(ScoreModFunc(score_mod_fwd_comp, nullptr));
}
```

**优化后** (Flash Attention Kernel 内部):

```
for each block:
  S_block = Q_block @ K_block^T        // SRAM 中计算
  S_block = S_block * scale            // SRAM 中 scale
  S_block = 30 * tanh(S_block / 30)    // ← ScoreMod 在 SRAM 中直接计算
  S_block = S_block + causal_mask      // mask 在 SRAM 中应用
  O_block += softmax(S_block) @ V_block // SRAM 中 softmax + BMM2
```

**关键**: `ScoreModFunc::UpdateCudnnMap` 将 HLO 计算映射为 cuDNN graph 节点，所有运算在 GPU SRAM 中完成，不产生中间 HBM 读写。

### 6.4 优化阶段 3: CuDNN Custom Call → Fusion 转换

**Pass**: `cudnn_custom_call_converter.cc:37-54`

```cpp
class CustomCallVisitor : public DfsHloRewriteVisitor {
  absl::Status HandleCustomCall(HloInstruction *hlo) override {
    if (hlo->custom_call_target() != kCuDnnFusionKind) {
      return absl::OkStatus();
    }
    // 将 custom-call 转换为 kCustom fusion
    HloInstruction *fusion = HloInstruction::CreateFusion(
        hlo->shape(), HloInstruction::FusionKind::kCustom, 
        hlo->operands(), computation);
    backend_config.set_kind(hlo->custom_call_target());
    fusion->set_backend_config(gpu_config);
    return ReplaceInstruction(hlo, fusion);
  }
};
```

### 6.5 优化阶段 4: 自动调优 (Autotuning)

**Pass**: `autotuner_pass.cc:92-154`

```cpp
if (backend_config.kind() == kCuDnnFusionKind) {
  // 尝试多种 cuDNN 算法配置
  for (auto& algo : available_algorithms) {
    config.mutable_cudnn_fmha_backend_config()
          .mutable_algorithm()
          ->set_algo_id(algo.id);
    config.mutable_cudnn_fmha_backend_config()
          .mutable_algorithm()
          ->mutable_tuning_knobs()
          ->insert({knob_id, knob_value});
    // 测量性能，选择最优配置
  }
}
```

**Backend Config 中的调优参数**:

```json
"algorithm": {
  "algo_id": "0",
  "math_type": "TENSOR_OP_MATH",
  "tuning_knobs": {
    "17": "1",   // block size 配置
    "24": "0"    // pipeline 配置
  },
  "workspace_size": "0"
}
```

### 6.6 优化阶段 5: 布局优化 (Layout Assignment)

**Pass**: `layout_assignment.cc`

```hlo
// 优化前 (默认布局)
Q: bf16[4,4,1024,64]{3,2,1,0}  // 默认 minor-to-major

// 优化后 (针对 cuDNN Flash Attention 优化的布局)
Q: bf16[4,4,1024,64]{3,1,2,0}  // 调整维度顺序以匹配 cuDNN 要求
```

### 6.7 优化阶段 6: 代数化简与常量折叠

**Pass**: `algebraic_simplifier.cc`

```hlo
// 优化前
%scale = constant(0.125)
%broadcast = broadcast(%scale)  // 广播到 [4,4,1024,1024]
%scaled = multiply(%dot1, %broadcast)

// 优化后
// broadcast + multiply 被吸收到 Flash Attention kernel 内部
// 只需要传递标量 scale 因子
backend_config.fmha_scale = 0.125
```

### 6.8 完整优化流程对比

| 指标 | 优化前 | 优化后 |
|------|--------|--------|
| **HLO 指令数** | 8+ | 1 (custom-call) |
| **GPU Kernel 数** | 5+ | 1 (Flash Attention) |
| **中间内存** | 2 × O(N²) ≈ 128MB | 0 (虚拟张量) |
| **HBM 读写** | ~4 × O(N²) | ~2 × O(N) |
| **自定义运算** | 独立 kernel | 内联到 Flash Attention SRAM 循环 |
| **Mask 处理** | 独立 multiply/add | cuDNN 内置 + ScoreMod 回调 |

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
