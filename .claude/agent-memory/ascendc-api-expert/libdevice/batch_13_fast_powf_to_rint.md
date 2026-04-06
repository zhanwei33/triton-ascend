# Libdevice 函数 AscendC 对应实现分析 - Batch 13

## 概述

本文档分析 10 个 libdevice 函数在 AscendC 中的对应实现方式，包括：fast_powf、hadd、rhadd、sub_rn、sub_rz、sub_rd、sub_ru、rsqrt_rn、ffs、rint。

---

## 1. fast_powf - Fast power function

### libdevice 描述
快速幂函数，计算 x 的 y 次幂。

### AscendC 对应实现

#### 方案一：SIMT API (仅 Ascend 950PR/Ascend 950DT)
```cpp
#include "simt_api/math_functions.h"

__simt_callee__ inline float powf(float x, float y)
```

**参数说明：**
- `x`: 幂计算的底数 (float)
- `y`: 幂计算的指数 (float)

**返回值：** x 的 y 次幂的结果

**特殊场景：**
- 若 x^y 超出 float 最大范围，返回值为 inf
- 底数小于 0 时返回 nan
- 底数为 1 或 -1，指数为 inf 时返回 nan
- 底数为 0，指数为 0 时返回 nan

#### 方案二：Tensor API (A2/A3/950 均支持)
```cpp
template <typename T, bool isReuseSource = false, const PowerConfig& config = defaultPowerConfig>
__aicore__ inline void Power(const LocalTensor<T>& dstTensor, const LocalTensor<T>& src0Tensor,
                             const LocalTensor<T>& src1Tensor, uint32_t calCount)
```

**支持的数据类型：**
- Ascend 950PR/Ascend 950DT: uint8_t、int8_t、uint16_t、int16_t、uint32_t、int32_t、half、bfloat16_t、float
- Atlas A2/A3: half、float、int32_t

**PowerConfig 配置：**
```cpp
enum class PowerAlgo {
    INTRINSIC = 0,        // 默认算法，浮点数使用 exp(y * ln(x)) 计算
    DOUBLE_FLOAT_TECH,    // 高精度算法，减少精度损失
};
```

### 产品支持情况

| 产品 | SIMT API (powf) | Tensor API (Power) |
|------|-----------------|-------------------|
| Ascend 950PR/Ascend 950DT | √ | √ |
| Atlas A3 训练/推理系列 | x | √ |
| Atlas A2 训练/推理系列 | x | √ |

### 迁移建议
- **Ascend 950PR/950DT**: 可直接使用 `powf` 或 `Power` API
- **Atlas A2/A3**: 使用 `Power` Tensor API
- 需要快速计算时可使用 `PowerAlgo::INTRINSIC`，需要高精度时使用 `DOUBLE_FLOAT_TECH`

---

## 2. hadd - Half add (average)

### libdevice 描述
计算两个数的平均值，即 (a + b) / 2。

### AscendC 对应实现

#### 方案一：基础 Add + Mul (通用方案)
```cpp
#include "kernel_operator.h"

// 使用 Add + Muls 组合实现
AscendC::Add(dst, src0, src1, count);        // dst = src0 + src1
AscendC::Muls(dst, dst, 0.5f, count);        // dst = dst * 0.5
```

#### 方案二：C API (A2/A3)
```cpp
// 使用 asc_add 后接标量乘法
__aicore__ inline void asc_add(__ubuf__ half* dst, __ubuf__ half* src0,
                               __ubuf__ half* src1, uint32_t count)
```

#### 方案三：Tensor 运算符重载
```cpp
dstLocal = (src0Local + src1Local) * 0.5f;
```

### 产品支持情况

| 产品 | Add + Muls | C API (asc_add) |
|------|-----------|-----------------|
| Ascend 950PR/Ascend 950DT | √ | x |
| Atlas A3 训练/推理系列 | √ | √ |
| Atlas A2 训练/推理系列 | √ | √ |

### 支持的数据类型
- **Ascend 950PR/950DT**: int8_t、uint8_t、int16_t、uint16_t、half、bfloat16_t、int32_t、uint32_t、float、complex32、int64_t、uint64_t、complex64
- **Atlas A2/A3**: half、int16_t、int32_t、float

### 迁移建议
- 使用 `Add` + `Muls` 组合实现 hadd 功能
- 注意整数除法与浮点除法的区别，hadd 通常用于整数平均

---

## 3. rhadd - Rounded half add

### libdevice 描述
带舍入的半加，计算 (a + b + 1) / 2，用于整数平均时的向上舍入。

### AscendC 对应实现

#### 方案：Add + Adds + Muls
```cpp
#include "kernel_operator.h"

// 实现 (a + b + 1) / 2
AscendC::Add(dst, src0, src1, count);        // dst = src0 + src1
AscendC::Adds(dst, dst, 1, count);           // dst = dst + 1
AscendC::Muls(dst, dst, 0.5f, count);        // dst = dst * 0.5
```

或使用标量运算：
```cpp
dstLocal = (src0Local + src1Local + 1) * 0.5f;
```

### 产品支持情况

| 产品 | 支持情况 |
|------|---------|
| Ascend 950PR/Ascend 950DT | √ |
| Atlas A3 训练/推理系列 | √ |
| Atlas A2 训练/推理系列 | √ |

### 迁移建议
- rhadd 需要通过组合指令实现：Add -> Adds(加1) -> Muls(乘0.5)
- 适用于整数平均需要向上舍入的场景

---

## 4-7. sub_rn / sub_rz / sub_rd / sub_ru - Subtract with rounding modes

### libdevice 描述
带不同舍入模式的减法：
- `sub_rn`: Round to nearest (四舍六入五成双)
- `sub_rz`: Round toward zero (向零舍入)
- `sub_rd`: Round downward (向下舍入)
- `sub_ru`: Round upward (向上舍入)

### AscendC 对应实现

#### 基础减法 API
```cpp
#include "kernel_operator.h"

// Tensor API
template <typename T>
__aicore__ inline void Sub(const LocalTensor<T>& dst, const LocalTensor<T>& src0,
                           const LocalTensor<T>& src1, const int32_t& count)

// 或运算符重载
dstLocal = src0Local - src1Local;
```

#### C API (A2/A3)
```cpp
__aicore__ inline void asc_sub(__ubuf__ half* dst, __ubuf__ half* src0,
                               __ubuf__ half* src1, uint32_t count)
```

### 舍入模式控制

AscendC 通过 `RoundMode` 枚举在类型转换时控制舍入模式：

```cpp
enum class RoundMode {
    CAST_NONE = 0,   // 默认舍入
    CAST_RINT,       // rint，四舍六入五成双 (对应 rn)
    CAST_FLOOR,      // floor，向负无穷舍入 (对应 rd)
    CAST_CEIL,       // ceil，向正无穷舍入 (对应 ru)
    CAST_ROUND,      // round，四舍五入
    CAST_TRUNC,      // trunc，向零舍入 (对应 rz)
    CAST_ODD,        // 最近邻奇数舍入
    CAST_HYBRID,     // 随机舍入
};
```

#### 带舍入的减法实现
```cpp
// 先减法，然后通过 Cast 进行舍入控制
AscendC::Sub(tmp, src0, src1, count);
AscendC::Cast(dst, tmp, RoundMode::CAST_RINT, count);  // rn 模式
```

### 产品支持情况

| 产品 | Sub API | RoundMode 控制 |
|------|---------|---------------|
| Ascend 950PR/Ascend 950DT | √ | √ |
| Atlas A3 训练/推理系列 | √ | √ |
| Atlas A2 训练/推理系列 | √ | √ |

### 迁移建议
- 基础减法使用 `Sub` API
- 舍入模式需要通过 `Cast` 接口配合 `RoundMode` 实现
- `sub_rn` -> `CAST_RINT`
- `sub_rz` -> `CAST_TRUNC`
- `sub_rd` -> `CAST_FLOOR`
- `sub_ru` -> `CAST_CEIL`

---

## 8. rsqrt_rn - Reciprocal sqrt round to nearest

### libdevice 描述
带最近舍入的平方根倒数计算。

### AscendC 对应实现

#### 方案一：SIMT API (仅 Ascend 950PR/Ascend 950DT)
```cpp
#include "simt_api/math_functions.h"

// float 版本
__simt_callee__ inline float rsqrtf(float x)

// half 版本
__simt_callee__ inline half hrsqrt(half x)

// half2 向量版本
__simt_callee__ inline half2 h2rsqrt(half2 x)
```

**特殊场景：**
- 当 x 为 0 时，返回值为 inf
- 当 x 为 inf 时，返回值为 0
- 当 x 为负数时，返回值为 nan

#### 方案二：Tensor API (通用)
```cpp
#include "kernel_operator.h"

template <typename T, const RsqrtConfig& config = DEFAULT_RSQRT_CONFIG>
__aicore__ inline void Rsqrt(const LocalTensor<T>& dst, const LocalTensor<T>& src,
                             const int32_t& count)
```

**RsqrtConfig 配置：**
```cpp
enum class RsqrtAlgo {
    INTRINSIC = 0,              // 默认，最大误差 1 ulp
    FAST_INVERSE,               // 快速求逆，0 ulp 精度
    PRECISION_1ULP_FTZ_TRUE,    // 1 ulp 精度
    PRECISION_0ULP_FTZ_FALSE,   // 0 ulp 精度，支持 Subnormal
    PRECISION_1ULP_FTZ_FALSE,   // 1 ulp 精度，仅 half
};
```

#### 方案三：C API (A2/A3)
```cpp
__aicore__ inline void asc_rsqrt(__ubuf__ half* dst, __ubuf__ half* src, uint32_t count)
__aicore__ inline void asc_rsqrt(__ubuf__ float* dst, __ubuf__ float* src, uint32_t count)
```

### 产品支持情况

| 产品 | SIMT API | Tensor API | C API |
|------|----------|-----------|-------|
| Ascend 950PR/Ascend 950DT | √ | √ | x |
| Atlas A3 训练/推理系列 | x | √ | √ |
| Atlas A2 训练/推理系列 | x | √ | √ |

### 迁移建议
- **高精度需求**: 使用 `RsqrtAlgo::PRECISION_0ULP_FTZ_FALSE`
- **性能优先**: 使用默认 `INTRINSIC` 或 `FAST_INVERSE`
- 需要舍入控制时，可配合 `Cast` 接口使用

---

## 9. ffs - Find first set bit

### libdevice 描述
查找输入数据二进制表示中从最低位开始的第一个值为 1 的位的位置。

### AscendC 对应实现

#### C API (A2/A3/950 均支持)
```cpp
__aicore__ inline int64_t asc_ffs(uint64_t value)
```

**功能说明：**
- 从最低位向最高位查找第一个值为 1 的位
- 返回其位置索引（从 0 开始）
- 如果没有找到则返回 -1

**流水类型：** PIPE_S

### 产品支持情况

| 产品 | 支持情况 |
|------|---------|
| Ascend 950PR/Ascend 950DT | √ |
| Atlas A3 训练/推理系列 | √ |
| Atlas A2 训练/推理系列 | √ |

### 调用示例
```cpp
uint64_t value = 10;  // 二进制: 1010
int64_t ret = asc_ffs(value);  // 返回 1，因为第 1 位是第一个 1
```

### 迁移建议
- 直接使用 `asc_ffs` API
- 注意返回值为 `int64_t`，未找到时返回 -1
- 该指令在标量流水上执行 (PIPE_S)

---

## 10. rint - Round to nearest integer

### libdevice 描述
将浮点数舍入到最接近的整数，若有两个同样接近的整数，则取其中的偶数（四舍六入五成双）。

### AscendC 对应实现

#### 方案一：SIMT API (仅 Ascend 950PR/Ascend 950DT)

**float 版本：**
```cpp
#include "simt_api/math_functions.h"

__simt_callee__ inline float rintf(float x)
```

**half 版本：**
```cpp
__simt_callee__ inline half hrint(half x)
__simt_callee__ inline half2 h2rint(half2 x)  // 向量版本
```

**返回整数版本：**
```cpp
__simt_callee__ inline long int lrintf(float x)
__simt_callee__ inline long long int llrintf(float x)
```

**特殊场景：**
- 当 x 为 0 时，返回值为 0
- 当 x 为 0.5 时，返回值为 0（取偶数）
- 当 x 为 1.5 时，返回值为 2（取偶数）
- 当 x 为 nan 时，返回值为 nan（或 0 对于 lrintf/llrintf）

#### 方案二：Tensor API (仅 Ascend 950PR/Ascend 950DT)
```cpp
#include "kernel_operator.h"

template <const RintConfing& config = DEFAULT_RINT_CONFIG, typename T>
__aicore__ inline void Rint(const LocalTensor<T>& dst, const LocalTensor<T>& src,
                            const LocalTensor<uint8_t>& sharedTmpBuffer, const uint32_t count)
```

**支持的数据类型：** half、float

#### 方案三：RoundMode 配合 Cast (通用)
```cpp
// 使用 CAST_RINT 模式进行舍入
AscendC::Cast(dst, src, RoundMode::CAST_RINT, count);
```

### 产品支持情况

| 产品 | SIMT API (rintf) | Tensor API (Rint) | Cast + RoundMode |
|------|-----------------|-------------------|------------------|
| Ascend 950PR/Ascend 950DT | √ | √ | √ |
| Atlas A3 训练/推理系列 | x | x | √ |
| Atlas A2 训练/推理系列 | x | x | √ |

### 迁移建议

- **Ascend 950PR/950DT**: 优先使用 `rintf` 或 `hrint` SIMT API
- **Atlas A2/A3**: 使用 `Cast` 接口配合 `RoundMode::CAST_RINT`
- 注意 `rint` 与 `round` 的区别：
  - `rint`: 四舍六入五成双（向偶数舍入）
  - `round`: 四舍五入

### 示例对比
```cpp
// rint 行为 (四舍六入五成双)
rint(3.5) = 4   // 4 是偶数
rint(2.5) = 2   // 2 是偶数
rint(3.4) = 3
rint(3.6) = 4

// round 行为 (四舍五入)
round(3.5) = 4
round(2.5) = 3
```

---

## 总结表

| libdevice 函数 | AscendC 对应 API | 支持产品 | 备注 |
|---------------|-----------------|----------|------|
| fast_powf | `powf` (SIMT) / `Power` (Tensor) | 950: √, A2/A3: Power only | 使用 PowerAlgo 控制精度 |
| hadd | `Add` + `Muls` | 全部 | (a+b)/2 |
| rhadd | `Add` + `Adds` + `Muls` | 全部 | (a+b+1)/2 |
| sub_rn | `Sub` + `Cast` (CAST_RINT) | 全部 | 需配合舍入模式 |
| sub_rz | `Sub` + `Cast` (CAST_TRUNC) | 全部 | 向零舍入 |
| sub_rd | `Sub` + `Cast` (CAST_FLOOR) | 全部 | 向下舍入 |
| sub_ru | `Sub` + `Cast` (CAST_CEIL) | 全部 | 向上舍入 |
| rsqrt_rn | `rsqrtf` / `Rsqrt` | 950: √, A2/A3: Rsqrt | 多种精度模式可选 |
| ffs | `asc_ffs` | 全部 | 标量操作 |
| rint | `rintf` / `Cast` (CAST_RINT) | 950: √, A2/A3: Cast | 四舍六入五成双 |

---

## 头文件汇总

```cpp
// SIMT API (仅 Ascend 950PR/950DT)
#include "simt_api/math_functions.h"
#include "simt_api/asc_fp16.h"    // half 类型
#include "simt_api/asc_bf16.h"    // bfloat16 类型

// Tensor API (通用)
#include "kernel_operator.h"

// C API (A2/A3)
// 自动包含在 kernel_operator.h 中
```

---

## 注意事项

1. **产品兼容性**: 部分 SIMT API 仅在 Ascend 950PR/950DT 上支持，Atlas A2/A3 需要使用 Tensor API 或 C API
2. **舍入模式**: AscendC 的舍入模式主要通过 `RoundMode` 枚举在 `Cast` 接口中控制
3. **精度控制**: 数学函数（如 Power、Rsqrt）通常提供多种精度算法供选择
4. **数据类型对齐**: Tensor API 要求操作数地址 32 字节对齐
5. **流水类型**: 标量操作（如 asc_ffs）在 PIPE_S 上执行，矢量操作在 PIPE_V 上执行
