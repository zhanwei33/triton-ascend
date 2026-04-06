# Libdevice 函数 AscendC 对应实现分析 - Batch 3

## 分析概述

本批次分析 10 个 libdevice 函数，涉及除法、倒数和平方根运算的不同舍入模式：
1. div_rn - Divide round to nearest
2. div_rz - Divide round toward zero
3. div_rd - Divide round downward
4. div_ru - Divide round upward
5. rcp_rn - Reciprocal round to nearest
6. rcp_rz - Reciprocal round toward zero
7. rcp_rd - Reciprocal round downward
8. rcp_ru - Reciprocal round upward
9. sqrt_rn - Square root round to nearest
10. sqrt_rz - Square root round toward zero

---

## 重要发现：舍入模式支持情况

**关键结论**：AscendC API **不直接支持**通过函数参数或模板参数指定不同的 IEEE 754 舍入模式（RN/RZ/RD/RU）。

AscendC 的 Div、Reciprocal、Sqrt 等运算使用硬件默认的舍入模式，该模式通常是：
- **默认舍入模式：向最近偶数舍入（Round to Nearest, Ties to Even）**

这与 CUDA libdevice 中显式指定舍入模式的函数（如 div_rn, div_rz 等）有本质区别。

---

## 1. div_rn - Divide round to nearest

### Libdevice 描述
执行除法运算，结果向最近偶数舍入（IEEE 754 默认舍入模式）。

### AscendC 对应实现

#### 函数原型
```cpp
// 整个 tensor 参与计算（运算符重载）
dst = src0 / src1;

// tensor 前 n 个数据计算
template <typename T, const DivConfig& config = DEFAULT_DIV_CONFIG>
__aicore__ inline void Div(const LocalTensor<T>& dst, const LocalTensor<T>& src0,
                           const LocalTensor<T>& src1, const int32_t& count);

// tensor 高维切分计算 - mask 连续模式
template <typename T, bool isSetMask = true, const DivConfig& config = DEFAULT_DIV_CONFIG>
__aicore__ inline void Div(const LocalTensor<T>& dst, const LocalTensor<T>& src0,
                           const LocalTensor<T>& src1, uint64_t mask,
                           const uint8_t repeatTime, const BinaryRepeatParams& repeatParams);

// tensor 高维切分计算 - mask 逐 bit 模式
template <typename T, bool isSetMask = true, const DivConfig& config = DEFAULT_DIV_CONFIG>
__aicore__ inline void Div(const LocalTensor<T>& dst, const LocalTensor<T>& src0,
                           const LocalTensor<T>& src1, uint64_t mask[],
                           const uint8_t repeatTime, const BinaryRepeatParams& repeatParams);
```

#### C API 风格
```cpp
__aicore__ inline void asc_div(__ubuf__ half* dst, __ubuf__ half* src0,
                               __ubuf__ half* src1, uint32_t count);
__aicore__ inline void asc_div(__ubuf__ float* dst, __ubuf__ float* src0,
                               __ubuf__ float* src1, uint32_t count);
```

#### 精度配置
```cpp
enum class DivAlgo {
    INTRINSIC = 0,              // 默认：单指令，1 ulp 误差
    DIFF_COMPENSATION,          // 差值补偿，0 ulp 误差
    PRECISION_1ULP_FTZ_TRUE,    // 单指令，1 ulp，flush subnormal
    PRECISION_0ULP_FTZ_TRUE,    // 差值补偿，0 ulp，flush subnormal
    PRECISION_0ULP_FTZ_FALSE,   // 差值补偿，0 ulp，支持 subnormal
    PRECISION_1ULP_FTZ_FALSE    // 单指令，1 ulp，支持 subnormal
};

struct DivConfig {
    DivAlgo algo = DivAlgo::INTRINSIC;
};

constexpr DivConfig DEFAULT_DIV_CONFIG = { DivAlgo::INTRINSIC };
```

#### 产品支持
| 产品 | 支持情况 |
|------|----------|
| Atlas A2 训练/推理 | √ |
| Atlas A3 训练/推理 | √ |
| Ascend 950PR/950DT | √ |
| Kirin X90/9030 | √ |

#### 数据类型支持
- **A2/A3/X90/9030**: half, float
- **Ascend 950PR/950DT**: int16_t, uint16_t, half, int32_t, uint32_t, float, complex32, int64_t, uint64_t, complex64

#### 映射分析
| 属性 | CUDA libdevice | AscendC |
|------|---------------|---------|
| 舍入模式 | RN (round to nearest even) | 硬件默认（通常为 RN） |
| 精度控制 | 固定 | 可通过 DivConfig 配置 |
| 典型误差 | 0.5 ulp | 默认 1 ulp，可配置为 0 ulp |

---

## 2. div_rz - Divide round toward zero

### Libdevice 描述
执行除法运算，结果向零舍入（截断舍入）。

### AscendC 对应实现

**⚠️ 重要限制**：AscendC 的 `Div` 函数**不支持**显式指定向零舍入（RZ）模式。

#### 可行替代方案
如果需要向零舍入的除法，需要手动实现：

```cpp
// 方案 1：使用符号判断 + 绝对值除法
template <typename T>
__aicore__ inline void DivRZ(const LocalTensor<T>& dst, const LocalTensor<T>& src0,
                             const LocalTensor<T>& src1, const int32_t& count) {
    // 1. 计算绝对值
    LocalTensor<T> abs0 = ...;  // 临时空间
    LocalTensor<T> abs1 = ...;
    Abs(abs0, src0, count);
    Abs(abs1, src1, count);

    // 2. 执行正数除法
    Div(dst, abs0, abs1, count);

    // 3. 恢复符号（异或逻辑）
    // 需要额外处理符号位
}
```

#### 映射建议
| 方案 | 说明 | 复杂度 |
|------|------|--------|
| 方案 A | 使用默认 Div（RN 模式） | 直接替换，精度略有差异 |
| 方案 B | 手动实现 RZ 舍入 | 需要额外指令和临时空间 |
| 方案 C | 使用整数除法后转换 | 仅适用于整数输入 |

---

## 3. div_rd - Divide round downward

### Libdevice 描述
执行除法运算，结果向下舍入（向负无穷）。

### AscendC 对应实现

**⚠️ 重要限制**：AscendC 的 `Div` 函数**不支持**显式指定向下舍入（RD）模式。

#### 可行替代方案
```cpp
// 方案：使用 Floor 函数辅助实现
// 对于正数：Div_RD = floor(Div_RN)
// 对于负数：Div_RD = floor(Div_RN) - epsilon
```

---

## 4. div_ru - Divide round upward

### Libdevice 描述
执行除法运算，结果向上舍入（向正无穷）。

### AscendC 对应实现

**⚠️ 重要限制**：AscendC 的 `Div` 函数**不支持**显式指定向上舍入（RU）模式。

#### 可行替代方案
```cpp
// 方案：使用 Ceil 函数辅助实现
// 对于正数：Div_RU = ceil(Div_RN)
// 对于负数：Div_RU = ceil(Div_RN) + epsilon
```

---

## 5. rcp_rn - Reciprocal round to nearest

### Libdevice 描述
计算倒数（1/x），结果向最近偶数舍入。

### AscendC 对应实现

#### 函数原型
```cpp
// tensor 前 n 个数据计算
template <typename T, const ReciprocalConfig& config = DEFAULT_RECIPROCAL_CONFIG>
__aicore__ inline void Reciprocal(const LocalTensor<T>& dst, const LocalTensor<T>& src,
                                  const int32_t& count);

// tensor 高维切分计算 - mask 连续模式
template <typename T, bool isSetMask = true, const ReciprocalConfig& config = DEFAULT_RECIPROCAL_CONFIG>
__aicore__ inline void Reciprocal(const LocalTensor<T>& dst, const LocalTensor<T>& src,
                                  uint64_t mask, const uint8_t repeatTime,
                                  const UnaryRepeatParams& repeatParams);

// tensor 高维切分计算 - mask 逐 bit 模式
template <typename T, bool isSetMask = true, const ReciprocalConfig& config = DEFAULT_RECIPROCAL_CONFIG>
__aicore__ inline void Reciprocal(const LocalTensor<T>& dst, const LocalTensor<T>& src,
                                  uint64_t mask[], const uint8_t repeatTime,
                                  const UnaryRepeatParams& repeatParams);
```

#### C API 风格
```cpp
__aicore__ inline void asc_rcp(__ubuf__ half* dst, __ubuf__ half* src, uint32_t count);
__aicore__ inline void asc_rcp(__ubuf__ float* dst, __ubuf__ float* src, uint32_t count);
```

#### Subnormal 配置
```cpp
enum class ReciprocalAlgo {
    INTRINSIC = 0,              // 默认：单指令，subnormal 近似为 0
    PRECISION_1ULP_FTZ_TRUE,    // 同上
    PRECISION_1ULP_FTZ_FALSE    // 支持 subnormal 计算
};

struct ReciprocalConfig {
    ReciprocalAlgo algo = ReciprocalAlgo::INTRINSIC;
};
```

#### 产品支持
| 产品 | 支持情况 |
|------|----------|
| Atlas A2 训练/推理 | √ |
| Atlas A3 训练/推理 | √ |
| Ascend 950PR/950DT | √ |
| Kirin X90/9030 | √ |

#### 数据类型支持
- **A2/A3/X90/9030**: half, float
- **Ascend 950PR/950DT**: half, float, int64_t, uint64_t

#### 精度说明
- half 类型：误差不满足双千分之一要求
- float 类型：误差不满足双万分之一要求
- **如需高精度，建议使用 Div 替代**：`dst = 1.0 / src`

---

## 6. rcp_rz - Reciprocal round toward zero

### Libdevice 描述
计算倒数（1/x），结果向零舍入。

### AscendC 对应实现

**⚠️ 重要限制**：AscendC 的 `Reciprocal` 函数**不支持**显式指定向零舍入（RZ）模式。

#### 可行替代方案
```cpp
// 方案：使用 Div 实现，配合符号处理
// dst = (src > 0) ? (1.0 / src) : -(1.0 / abs(src))
```

---

## 7. rcp_rd - Reciprocal round downward

### Libdevice 描述
计算倒数（1/x），结果向下舍入。

### AscendC 对应实现

**⚠️ 重要限制**：AscendC 的 `Reciprocal` 函数**不支持**显式指定向下舍入（RD）模式。

---

## 8. rcp_ru - Reciprocal round upward

### Libdevice 描述
计算倒数（1/x），结果向上舍入。

### AscendC 对应实现

**⚠️ 重要限制**：AscendC 的 `Reciprocal` 函数**不支持**显式指定向上舍入（RU）模式。

---

## 9. sqrt_rn - Square root round to nearest

### Libdevice 描述
计算平方根，结果向最近偶数舍入。

### AscendC 对应实现

#### 方案 1：使用 Sqrt 函数（A2/A3 专用）
```cpp
// 仅 Atlas A2/A3 支持
#include "reg_vector.h"

template <typename T = DefaultType, auto mode = MaskMergeMode::ZEROING, typename U>
__simd_callee__ inline void Sqrt(U& dstReg, U& srcReg, MaskReg& mask);
```

#### 方案 2：使用 Rsqrt + 倒数（推荐，更广泛支持）
```cpp
// Rsqrt 计算 1/sqrt(x)，然后取倒数得到 sqrt(x)
// 或者直接使用 Rsqrt 结果

template <typename T, const RsqrtConfig& config = DEFAULT_RSQRT_CONFIG>
__aicore__ inline void Rsqrt(const LocalTensor<T>& dst, const LocalTensor<T>& src,
                             const int32_t& count);
```

#### 方案 3：C API 风格
```cpp
__aicore__ inline void asc_sqrt(__ubuf__ half* dst, __ubuf__ half* src, uint32_t count);
__aicore__ inline void asc_sqrt(__ubuf__ float* dst, __ubuf__ float* src, uint32_t count);
```

#### Rsqrt 精度配置
```cpp
enum class RsqrtAlgo {
    INTRINSIC = 0,              // 默认：单指令，1 ulp
    FAST_INVERSE,               // 快速求逆，0 ulp（特定范围）
    PRECISION_1ULP_FTZ_TRUE,    // 单指令，1 ulp
    PRECISION_0ULP_FTZ_FALSE,   // 快速求逆，0 ulp，支持 subnormal
    PRECISION_1ULP_FTZ_FALSE    // 单指令，1 ulp，支持 subnormal（half）
};
```

#### 产品支持
| 产品 | Sqrt | Rsqrt |
|------|------|-------|
| Atlas A2 训练/推理 | x | √ |
| Atlas A3 训练/推理 | x | √ |
| Ascend 950PR/950DT | √ | √ |
| Kirin X90/9030 | - | √ |

**注意**：A2/A3 不支持 Sqrt 函数，需要使用 Rsqrt 配合其他操作实现。

---

## 10. sqrt_rz - Square root round toward zero

### Libdevice 描述
计算平方根，结果向零舍入。

### AscendC 对应实现

**⚠️ 重要限制**：AscendC 的 `Sqrt`/`Rsqrt` 函数**不支持**显式指定向零舍入（RZ）模式。

---

## 总结与建议

### 直接映射表

| Libdevice 函数 | AscendC 对应函数 | 支持状态 | 说明 |
|---------------|-----------------|----------|------|
| div_rn | Div / asc_div | ✅ 支持 | 使用默认配置 |
| div_rz | Div + 手动处理 | ⚠️ 需适配 | 不支持显式 RZ 模式 |
| div_rd | Div + 手动处理 | ⚠️ 需适配 | 不支持显式 RD 模式 |
| div_ru | Div + 手动处理 | ⚠️ 需适配 | 不支持显式 RU 模式 |
| rcp_rn | Reciprocal / asc_rcp | ✅ 支持 | 使用默认配置 |
| rcp_rz | Reciprocal + 手动处理 | ⚠️ 需适配 | 不支持显式 RZ 模式 |
| rcp_rd | Reciprocal + 手动处理 | ⚠️ 需适配 | 不支持显式 RD 模式 |
| rcp_ru | Reciprocal + 手动处理 | ⚠️ 需适配 | 不支持显式 RU 模式 |
| sqrt_rn | Rsqrt / asc_sqrt | ✅ 部分支持 | A2/A3 需用 Rsqrt |
| sqrt_rz | Rsqrt + 手动处理 | ⚠️ 需适配 | 不支持显式 RZ 模式 |

### 精度配置建议

对于需要高精度计算的场景：

```cpp
// Div 高精度配置（0 ulp 误差）
static constexpr DivConfig highPrecisionDiv = { DivAlgo::DIFF_COMPENSATION };
Div<T, highPrecisionDiv>(dst, src0, src1, count);

// 或支持 subnormal 的高精度配置
static constexpr DivConfig highPrecisionSubnormal = { DivAlgo::PRECISION_0ULP_FTZ_FALSE };
Div<T, highPrecisionSubnormal>(dst, src0, src1, count);
```

### 舍入模式适配策略

由于 AscendC 不支持显式舍入模式选择，建议：

1. **对于 div_rn/rcp_rn/sqrt_rn**：直接使用 AscendC 默认函数
2. **对于其他舍入模式**：
   - 评估是否可以使用默认 RN 模式替代
   - 如需严格匹配，需要手动实现舍入逻辑（性能开销较大）
   - 考虑在算法层面适配 RN 舍入模式

### 头文件包含

```cpp
// 标准 AscendC 算子
#include "kernel_operator.h"

// C API 风格（如需要）
#include "c_api/vector_compute/asc_div.h"
#include "c_api/vector_compute/asc_rcp.h"
#include "c_api/vector_compute/asc_sqrt.h"
#include "c_api/vector_compute/asc_rsqrt.h"

// SIMT API（如需要）
#include "simt_api/math_functions.h"
```

---

## 参考文档

- `/gemini/code/huawei/asc-devkit/docs/api/context/Div.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/Reciprocal.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/Rsqrt.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/Sqrt-18.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/c_api/vector_compute/asc_div.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/c_api/vector_compute/asc_rcp.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/c_api/vector_compute/asc_sqrt.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/c_api/vector_compute/asc_rsqrt.md`
