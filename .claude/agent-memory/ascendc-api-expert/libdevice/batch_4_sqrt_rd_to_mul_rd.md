# Libdevice 函数 AscendC 对应实现分析 - Batch 4

## 分析概述

本报告分析 10 个 libdevice 函数在 AscendC 中的对应实现方式，包括平方根函数（带不同舍入模式）和基本算术运算（带不同舍入模式）。

**分析日期**: 2026-03-31

---

## 1. sqrt_rd - Square root round downward

### libdevice 描述
- **功能**: 计算平方根，结果向负无穷方向舍入（向下舍入）
- **CUDA 语义**: sqrt with round-downward mode

### AscendC 对应实现

**主要 API**: `AscendC::Sqrt` + 精度配置

```cpp
template <typename T, const SqrtConfig& config = DEFAULT_SQRT_CONFIG>
__aicore__ inline void Sqrt(const LocalTensor<T>& dst, const LocalTensor<T>& src, const int32_t& count)
```

**配置选项**:
```cpp
enum class SqrtAlgo {
    INTRINSIC = 0,              // 默认，最大误差 1 ulp
    FAST_INVERSE,               // 快速求逆，最大误差 0 ulp
    PRECISION_1ULP_FTZ_TRUE,    // 1 ulp 精度
    PRECISION_0ULP_FTZ_FALSE,   // 0 ulp 精度，支持 Subnormal
    PRECISION_1ULP_FTZ_FALSE,   // 1 ulp 精度，支持 Subnormal (仅 half)
};
```

**向下舍入实现方案**:
- AscendC Sqrt 本身不提供显式舍入模式控制
- 要实现 sqrt_rd，需要结合 `floorf` 或 `truncf` 后处理

```cpp
// 方案 1: 使用 Sqrt + floorf (SIMT 风格)
#include "simt_api/math_functions.h"
float sqrt_rd(float x) {
    float sqrt_val = sqrtf(x);
    return floorf(sqrt_val);  // 向下舍入
}

// 方案 2: 使用 Sqrt + Truncate (Vector 风格)
AscendC::Sqrt(dstLocal, srcLocal, count);
AscendC::Truncate(dstLocal, dstLocal, count);  // 向零舍入，正数时等效向下
```

**产品支持**:
- Ascend 950PR/Ascend 950DT: 支持
- Atlas A2/A3 训练/推理: 支持

---

## 2. sqrt_ru - Square root round upward

### libdevice 描述
- **功能**: 计算平方根，结果向正无穷方向舍入（向上舍入）

### AscendC 对应实现

**主要 API**: `AscendC::Sqrt` + `ceilf`

**向上舍入实现方案**:

```cpp
// 方案 1: SIMT 风格
#include "simt_api/math_functions.h"
float sqrt_ru(float x) {
    float sqrt_val = sqrtf(x);
    return ceilf(sqrt_val);  // 向上舍入
}

// 方案 2: Vector 风格（需组合实现）
// AscendC 没有直接的 Ceil 接口，需要通过 Round 模式或自定义实现
```

**注意**: AscendC Vector API 中没有直接的 Ceil 函数，需要通过以下方式实现:
1. 使用 `Round` 接口（四舍五入到偶数）
2. 或使用 `Cast` 配合 `RoundMode::CAST_CEIL`

```cpp
// 使用 Cast 实现 Ceil
AscendC::Cast(dstLocal, srcLocal, AscendC::RoundMode::CAST_CEIL, count);
```

---

## 3. sqrt - Square root

### libdevice 描述
- **功能**: 计算平方根，使用默认舍入模式（通常是 round-to-nearest-even）

### AscendC 对应实现

**SIMT 风格**:
```cpp
#include "simt_api/math_functions.h"
__simt_callee__ inline float sqrtf(float x)
```

**Vector 风格**:
```cpp
// 前 n 个数据计算
template <typename T, const SqrtConfig& config = DEFAULT_SQRT_CONFIG>
__aicore__ inline void Sqrt(const LocalTensor<T>& dst, const LocalTensor<T>& src, const int32_t& count)

// 高维切分计算 - mask 连续模式
template <typename T, bool isSetMask = true, const SqrtConfig& config = DEFAULT_SQRT_CONFIG>
__aicore__ inline void Sqrt(const LocalTensor<T>& dst, const LocalTensor<T>& src, uint64_t mask, const uint8_t repeatTime, const UnaryRepeatParams& repeatParams)

// 高维切分计算 - mask 逐 bit 模式
template <typename T, bool isSetMask = true, const SqrtConfig& config = DEFAULT_SQRT_CONFIG>
__aicore__ inline void Sqrt(const LocalTensor<T>& dst, const LocalTensor<T>& src, uint64_t mask[], const uint8_t repeatTime, const UnaryRepeatParams& repeatParams)
```

**C API 风格**:
```cpp
__aicore__ inline void asc_sqrt(__ubuf__ half* dst, __ubuf__ half* src, uint32_t count)
__aicore__ inline void asc_sqrt(__ubuf__ float* dst, __ubuf__ float* src, uint32_t count)
```

**参数说明**:
- `dst`: 目的操作数（输出）
- `src`: 源操作数（输入，必须为非负数）
- `count`: 参与计算的元素个数
- `mask/mask[]`: 掩码控制
- `repeatTime`: 重复迭代次数
- `repeatParams`: 地址步长参数

**产品支持**:
- Ascend 950PR/Ascend 950DT: 支持
- Atlas A2/A3 训练/推理: 支持
- Kirin X90/9030: 支持

---

## 4. add_rn - Add round to nearest

### libdevice 描述
- **功能**: 加法运算，结果向最接近的值舍入（四舍六入五成双）

### AscendC 对应实现

**主要 API**: `AscendC::Add`

```cpp
// 前 n 个数据计算
template <typename T>
__aicore__ inline void Add(const LocalTensor<T>& dst, const LocalTensor<T>& src0, const LocalTensor<T>& src1, const int32_t& count)

// 高维切分计算
template <typename T, bool isSetMask = true>
__aicore__ inline void Add(const LocalTensor<T>& dst, const LocalTensor<T>& src0, const LocalTensor<T>& src1, uint64_t mask, const uint8_t repeatTime, const BinaryRepeatParams& repeatParams)
```

**运算符重载**:
```cpp
dst = src0 + src1;  // 整个 tensor 参与计算
```

**舍入模式说明**:
- AscendC Add 使用硬件默认舍入模式（通常是 round-to-nearest-even）
- 这与 libdevice 的 add_rn 语义一致

**支持数据类型**:
- Atlas A2/A3: half, int16_t, int32_t, float
- Ascend 950PR/950DT: int8_t, uint8_t, int16_t, uint16_t, half, bfloat16_t, int32_t, uint32_t, float, complex32, int64_t, uint64_t, complex64

---

## 5. add_rz - Add round toward zero

### libdevice 描述
- **功能**: 加法运算，结果向零方向舍入（截断）

### AscendC 对应实现

**实现方案**:

```cpp
// 方案 1: 使用 Add + Truncate
AscendC::Add(dstLocal, src0Local, src1Local, count);
AscendC::Truncate(dstLocal, dstLocal, count);  // 向零舍入

// 方案 2: SIMT 风格
#include "simt_api/math_functions.h"
float add_rz(float x, float y) {
    float sum = x + y;
    return truncf(sum);  // 向零截断
}
```

**Truncate 接口**:
```cpp
// 前 n 个数据计算
template <typename T>
__aicore__ inline void Truncate(const LocalTensor<T>& dst, const LocalTensor<T>& src, const int32_t& count)
```

---

## 6. add_rd - Add round downward

### libdevice 描述
- **功能**: 加法运算，结果向负无穷方向舍入（向下舍入）

### AscendC 对应实现

**实现方案**:

```cpp
// SIMT 风格
#include "simt_api/math_functions.h"
float add_rd(float x, float y) {
    float sum = x + y;
    return floorf(sum);  // 向下舍入
}
```

**Vector 风格**:
- AscendC Vector API 中没有直接的 Floor 函数
- 需要通过 `Cast` 配合 `RoundMode::CAST_FLOOR` 实现

```cpp
AscendC::Add(dstLocal, src0Local, src1Local, count);
// 使用 Cast 配合 FLOOR 模式
AscendC::Cast(dstLocal, dstLocal, AscendC::RoundMode::CAST_FLOOR, count);
```

**RoundMode 枚举**:
```cpp
enum class RoundMode {
    CAST_NONE = 0,      // 默认
    CAST_RINT,          // 四舍六入五成双
    CAST_FLOOR,         // 向负无穷舍入（floor）
    CAST_CEIL,          // 向正无穷舍入（ceil）
    CAST_ROUND,         // 四舍五入
    CAST_TRUNC,         // 向零舍入（trunc）
    CAST_ODD,           // 向奇数舍入
    CAST_HYBRID,        // 随机舍入（hif8 专用）
};
```

---

## 7. add_ru - Add round upward

### libdevice 描述
- **功能**: 加法运算，结果向正无穷方向舍入（向上舍入）

### AscendC 对应实现

**实现方案**:

```cpp
// SIMT 风格
#include "simt_api/math_functions.h"
float add_ru(float x, float y) {
    float sum = x + y;
    return ceilf(sum);  // 向上舍入
}
```

**Vector 风格**:
```cpp
AscendC::Add(dstLocal, src0Local, src1Local, count);
AscendC::Cast(dstLocal, dstLocal, AscendC::RoundMode::CAST_CEIL, count);
```

---

## 8. mul_rn - Multiply round to nearest

### libdevice 描述
- **功能**: 乘法运算，结果向最接近的值舍入

### AscendC 对应实现

**主要 API**: `AscendC::Mul`

```cpp
// 前 n 个数据计算
template <typename T>
__aicore__ inline void Mul(const LocalTensor<T>& dst, const LocalTensor<T>& src0, const LocalTensor<T>& src1, const int32_t& count)

// 高维切分计算
template <typename T, bool isSetMask = true>
__aicore__ inline void Mul(const LocalTensor<T>& dst, const LocalTensor<T>& src0, const LocalTensor<T>& src1, uint64_t mask, const uint8_t repeatTime, const BinaryRepeatParams& repeatParams)
```

**运算符重载**:
```cpp
dst = src0 * src1;  // 整个 tensor 参与计算
```

**舍入模式**:
- 默认使用 round-to-nearest-even，与 mul_rn 语义一致

---

## 9. mul_rz - Multiply round toward zero

### libdevice 描述
- **功能**: 乘法运算，结果向零方向舍入

### AscendC 对应实现

**实现方案**:

```cpp
// SIMT 风格
#include "simt_api/math_functions.h"
float mul_rz(float x, float y) {
    float prod = x * y;
    return truncf(prod);
}
```

**Vector 风格**:
```cpp
AscendC::Mul(dstLocal, src0Local, src1Local, count);
AscendC::Truncate(dstLocal, dstLocal, count);
```

---

## 10. mul_rd - Multiply round downward

### libdevice 描述
- **功能**: 乘法运算，结果向负无穷方向舍入

### AscendC 对应实现

**实现方案**:

```cpp
// SIMT 风格
#include "simt_api/math_functions.h"
float mul_rd(float x, float y) {
    float prod = x * y;
    return floorf(prod);
}
```

**Vector 风格**:
```cpp
AscendC::Mul(dstLocal, src0Local, src1Local, count);
AscendC::Cast(dstLocal, dstLocal, AscendC::RoundMode::CAST_FLOOR, count);
```

---

## 汇总表

| libdevice 函数 | AscendC 对应 API | 舍入模式实现 | 产品支持 |
|---------------|-----------------|-------------|---------|
| sqrt_rd | Sqrt + floorf/Cast(FLOOR) | 向下舍入 | A2, A3, 950PR/950DT |
| sqrt_ru | Sqrt + ceilf/Cast(CEIL) | 向上舍入 | A2, A3, 950PR/950DT |
| sqrt | Sqrt / sqrtf | 默认（最近偶数） | A2, A3, 950PR/950DT, Kirin |
| add_rn | Add / operator+ | 默认（最近偶数） | A2, A3, 950PR/950DT, Kirin |
| add_rz | Add + Truncate/truncf | 向零舍入 | A2, A3, 950PR/950DT |
| add_rd | Add + floorf/Cast(FLOOR) | 向下舍入 | A2, A3, 950PR/950DT |
| add_ru | Add + ceilf/Cast(CEIL) | 向上舍入 | A2, A3, 950PR/950DT |
| mul_rn | Mul / operator* | 默认（最近偶数） | A2, A3, 950PR/950DT, Kirin |
| mul_rz | Mul + Truncate/truncf | 向零舍入 | A2, A3, 950PR/950DT |
| mul_rd | Mul + floorf/Cast(FLOOR) | 向下舍入 | A2, A3, 950PR/950DT |

---

## 重要说明

### 1. 编程风格选择

**SIMT 风格** (推荐用于标量/逐元素操作):
```cpp
#include "simt_api/math_functions.h"
// sqrtf, floorf, ceilf, truncf, rintf, nearbyintf 等
// 适用于 __simt_vf__ 核函数
```

**Vector 风格** (推荐用于张量操作):
```cpp
// AscendC::Sqrt, AscendC::Add, AscendC::Mul 等
// 适用于 __aicore__ 核函数
```

### 2. 舍入模式限制

- AscendC Vector API 的 `Add` 和 `Mul` 本身不支持显式舍入模式控制
- 需要通过后处理（Truncate, Cast 等）实现特定舍入模式
- SIMT API 提供更直接的舍入控制（floorf, ceilf, truncf 等）

### 3. FMA 相关

对于融合乘加操作（FMA），AscendC 提供:
```cpp
// SIMT 风格
__simt_callee__ inline float fmaf(float x, float y, float z)  // x*y+z

// Vector 风格（仅 Ascend 950PR/950DT）
template <const FmaConfig& config = DEFAULT_FMA_CONFIG, typename T>
__aicore__ inline void Fma(const LocalTensor<T>& dst, const LocalTensor<T>& src0, const LocalTensor<T>& src1, const LocalTensor<T>& src2, const uint32_t count)
```

**注意**: Fma Vector API 仅在 Ascend 950PR/950DT 上支持，A2/A3 不支持。

### 4. 精度控制

对于 Sqrt，可以通过 `SqrtConfig` 配置精度:
- `SqrtAlgo::INTRINSIC`: 1 ulp 误差
- `SqrtAlgo::FAST_INVERSE`: 0 ulp 误差（特定范围）
- `SqrtAlgo::PRECISION_0ULP_FTZ_FALSE`: 0 ulp + Subnormal 支持

---

## 参考文档

- `/gemini/code/huawei/asc-devkit/docs/api/context/Sqrt.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/Add.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/Mul.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/sqrtf.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/floorf.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/ceilf.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/truncf.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/rintf.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/nearbyintf.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/RoundMode.md`
