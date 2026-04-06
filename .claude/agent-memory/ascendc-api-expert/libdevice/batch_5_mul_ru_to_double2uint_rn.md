# Libdevice 函数 AscendC 对应实现分析 - Batch 5

分析时间: 2026-03-31

## 概述

本文档分析 10 个 libdevice 函数在 AscendC 中的对应实现方式，涉及乘法舍入和双精度浮点类型转换。

**重要发现**: AscendC 对 `double` 类型的直接支持非常有限，大部分类型转换 API 仅支持 `float` 类型。`double` 类型主要在 Ascend 950PR/Ascend 950DT 上受支持，而 A2/A3 系列虽然支持 double 数据类型，但相关的类型转换和舍入控制 API 主要面向 float。

---

## 1. mul_ru - Multiply round upward

### libdevice 描述
乘法并向上舍入 (Multiply round upward)

### AscendC 对应实现

**结论**: AscendC 中没有直接的 `mul_ru` 单指令实现。需要通过组合指令实现。

#### 实现方案

**方案 1: 使用 Mul + Ceil 组合**
```cpp
// 先进行乘法，然后对结果向上取整
// dst = ceil(src0 * src1)
AscendC::LocalTensor<float> mulResult;
AscendC::Mul(mulResult, src0, src1, count);  // 普通乘法
AscendC::Ceil(dst, mulResult, count);        // 向上取整
```

**方案 2: 使用 Cast 的 RoundMode**
```cpp
// 对于浮点转整数的场景，可以使用 CAST_CEIL 模式
// 但这只适用于转整数，不适用于保留浮点结果
```

**方案 3: 使用 Truncate 的 CAST_CEIL 模式 (仅 Ascend 950PR/950DT)**
```cpp
// 仅适用于 Ascend 950PR/Ascend 950DT
// 使用 Truncate 函数的 CAST_CEIL 模式
AscendC::Reg::Truncate<T, AscendC::RoundMode::CAST_CEIL, AscendC::Reg::MaskMergeMode::ZEROING>(
    dstReg, srcReg, mask);
```

#### 产品支持情况
- **Ascend 950PR/Ascend 950DT**: 支持 Mul + Ceil 组合方案
- **Atlas A3 训练/推理系列**: 支持 Mul + Ceil 组合方案
- **Atlas A2 训练/推理系列**: 支持 Mul + Ceil 组合方案
- **Kirin X90/9030**: 支持 Mul + Ceil 组合方案

#### 限制说明
- AscendC 的 `Mul` 指令本身不支持指定舍入模式
- 需要先执行乘法，再对结果进行向上取整操作
- `Truncate` 函数的 `CAST_CEIL` 模式仅在 Ascend 950PR/950DT 上支持

---

## 2. double2float_rn - Double to float round to nearest

### libdevice 描述
将双精度浮点数转换为单精度浮点数，使用最近偶数舍入 (Round to nearest even)

### AscendC 对应实现

**结论**: AscendC 没有直接的 `double2float_rn` API。需要分步实现。

#### 实现方案

**方案 1: 使用 Cast 指令 (仅 Ascend 950PR/950DT)**
```cpp
// Ascend 950PR/950DT 支持 float 和 half 之间的转换
// 但没有明确的 double2float 转换 API
// 需要检查 Cast 指令是否支持 double 作为源类型

// Cast 指令原型
template <typename T = DefaultType, typename U = DefaultType, const CastTrait& trait = castTrait, typename S, typename V>
__simd_callee__ inline void Cast(S& dstReg, V& srcReg, MaskReg& mask);
```

**方案 2: 标量转换 (SIMT 模式)**
```cpp
// 在 SIMT 模式下，可以使用 C++ 标准类型转换
// 编译器会自动处理 double 到 float 的转换
__simt_vf__ inline void kernel_double2float(__gm__ float* dst, __gm__ double* src)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = (float)src[idx];  // 标准 C++ 转换
}
```

#### 产品支持情况
- **Ascend 950PR/Ascend 950DT**: 支持 double 数据类型，但类型转换 API 主要针对 float/half
- **Atlas A3 训练/推理系列**: 支持 double 数据类型
- **Atlas A2 训练/推理系列**: 支持 double 数据类型

#### 限制说明
- `Cast` API 文档中未明确列出 double 作为源类型的支持
- 表 4 "浮点转浮点" 中列出的转换类型: half<->float, bfloat16_t<->float 等，未包含 double
- 建议通过 C++ 标准转换或自定义实现

---

## 3. double2float_rz - Double to float round toward zero

### libdevice 描述
将双精度浮点数转换为单精度浮点数，向零舍入 (Round toward zero)

### AscendC 对应实现

**结论**: 与 `double2float_rn` 类似，没有直接 API。

#### 实现方案

**方案 1: 使用 Truncate 概念**
```cpp
// 向零舍入即截断小数部分
// 可以先转整数再转回浮点数（不推荐，会丢失小数信息）
// 或者使用 C++ 标准转换
```

**方案 2: C++ 标准转换**
```cpp
__simt_vf__ inline void kernel_double2float_rz(__gm__ float* dst, __gm__ double* src)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    double val = src[idx];
    // 向零舍入: 直接截断
    dst[idx] = (float)val;  // C++ 转换默认行为
}
```

#### 限制说明
- C++ 标准从 double 到 float 的转换通常采用向零舍入或最近偶数舍入，取决于编译器实现
- 如需精确控制舍入模式，需要自定义实现

---

## 4. double2float_rd - Double to float round downward

### libdevice 描述
将双精度浮点数转换为单精度浮点数，向下舍入 (Round downward/toward negative infinity)

### AscendC 对应实现

**结论**: 无直接 API，需要自定义实现。

#### 实现方案

**方案 1: 使用 Floor 函数**
```cpp
// 先转 float，然后使用 Floor 函数
// 但这会改变数值为整数
```

**方案 2: 自定义实现**
```cpp
// 需要检查 double 值是否超出 float 范围
// 然后使用适当的舍入逻辑
__simt_vf__ inline void kernel_double2float_rd(__gm__ float* dst, __gm__ double* src)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    double val = src[idx];
    float result = (float)val;
    // 如果需要向下舍入且结果大于原值，需要调整
    if ((double)result > val) {
        result = nextafterf(result, -INFINITY);
    }
    dst[idx] = result;
}
```

---

## 5. double2float_ru - Double to float round upward

### libdevice 描述
将双精度浮点数转换为单精度浮点数，向上舍入 (Round upward/toward positive infinity)

### AscendC 对应实现

**结论**: 无直接 API，需要自定义实现。

#### 实现方案

```cpp
__simt_vf__ inline void kernel_double2float_ru(__gm__ float* dst, __gm__ double* src)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    double val = src[idx];
    float result = (float)val;
    // 如果需要向上舍入且结果小于原值，需要调整
    if ((double)result < val) {
        result = nextafterf(result, INFINITY);
    }
    dst[idx] = result;
}
```

---

## 6. double2int_rn - Double to int round to nearest

### libdevice 描述
将双精度浮点数转换为有符号整数，最近偶数舍入

### AscendC 对应实现

**结论**: 无直接的 double 版本，但可以使用 float 版本的 `__float2int_rn` 作为参考。

#### 参考 API: `__float2int_rn`
```cpp
// 仅 Ascend 950PR/Ascend 950DT 支持
__simt_callee__ inline int __float2int_rn(const float x);
```

**功能**: 将浮点数转换为有符号整数，并四舍五入到最接近的偶数。

**特殊场景**:
- 当 x 为 nan 时，返回值为 0
- 当 x 为 inf 时，返回值为 2147483647
- 当 x 为 -inf 时，返回值为 -2147483648

#### 实现方案

**方案 1: SIMT 模式下的 C++ 转换**
```cpp
__simt_vf__ inline void kernel_double2int_rn(__gm__ int32_t* dst, __gm__ double* src)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    // C++ 的 (int) 转换默认向零舍入
    // 需要手动实现最近偶数舍入
    double val = src[idx];
    double rounded = round(val);  // round() 函数四舍五入到最近整数，中间值向偶数舍入
    dst[idx] = (int32_t)rounded;
}
```

**方案 2: 使用 Cast 指令 (如果支持 double)**
```cpp
// 检查 Cast 指令是否支持 double 到 int32_t 的转换
// RoundMode::CAST_RINT 或 RoundMode::CAST_ROUND
```

#### 产品支持情况
- **Ascend 950PR/Ascend 950DT**: 支持 `__float2int_rn`，double 版本需自定义
- **Atlas A3/A2 系列**: 不支持 `__float2int_rn`，需使用其他方法

---

## 7. double2int_rz - Double to int round toward zero

### libdevice 描述
将双精度浮点数转换为有符号整数，向零舍入

### AscendC 对应实现

**参考 API: `__float2int_rz`**
```cpp
// 仅 Ascend 950PR/Ascend 950DT 支持
__simt_callee__ inline int __float2int_rz(const float x);
```

**功能**: 将浮点数转换为向零舍入的有符号整数。

**特殊场景**:
- 当 x 为 nan 时，返回值为 0
- 当 x 为 inf 时，返回值为 2147483647
- 当 x 为 -inf 时，返回值为 -2147483648

#### 实现方案

**方案 1: C++ 标准转换**
```cpp
__simt_vf__ inline void kernel_double2int_rz(__gm__ int32_t* dst, __gm__ double* src)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    // C++ 的 (int) 转换默认向零舍入
    dst[idx] = (int32_t)src[idx];
}
```

**方案 2: 使用 Truncate 的 CAST_TRUNC 模式 (仅 950PR/950DT)**
```cpp
// RoundMode::CAST_TRUNC - 向零取整，截断小数位
```

---

## 8. double2int_rd - Double to int round downward

### libdevice 描述
将双精度浮点数转换为有符号整数，向下舍入 (向负无穷)

### AscendC 对应实现

**参考 API: `__float2int_rd`**
```cpp
// 仅 Ascend 950PR/Ascend 950DT 支持
__simt_callee__ inline int __float2int_rd(const float x);
```

**功能**: 将浮点数转换为向下取整的有符号整数。

**特殊场景**:
- 当 x 为 nan 时，返回值为 0
- 当 x 为 inf 时，返回值为 2147483647
- 当 x 为 -inf 时，返回值为 -2147483648

#### 实现方案

**方案 1: 使用 Floor 函数**
```cpp
__simt_vf__ inline void kernel_double2int_rd(__gm__ int32_t* dst, __gm__ double* src)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = (int32_t)floor(src[idx]);
}
```

**方案 2: 使用 Truncate 的 CAST_FLOOR 模式 (仅 950PR/950DT)**
```cpp
// RoundMode::CAST_FLOOR - floor 模式，向下取整
```

---

## 9. double2int_ru - Double to int round upward

### libdevice 描述
将双精度浮点数转换为有符号整数，向上舍入 (向正无穷)

### AscendC 对应实现

**参考 API: `__float2int_ru`**
```cpp
// 仅 Ascend 950PR/Ascend 950DT 支持
__simt_callee__ inline int __float2int_ru(const float x);
```

**功能**: 将浮点数转换为向上取整的有符号整数。

**特殊场景**:
- 当 x 为 nan 时，返回值为 0
- 当 x 为 inf 时，返回值为 2147483647
- 当 x 为 -inf 时，返回值为 -2147483648

#### 实现方案

**方案 1: 使用 Ceil 函数**
```cpp
__simt_vf__ inline void kernel_double2int_ru(__gm__ int32_t* dst, __gm__ double* src)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = (int32_t)ceil(src[idx]);
}
```

**方案 2: 使用 Truncate 的 CAST_CEIL 模式 (仅 950PR/950DT)**
```cpp
// RoundMode::CAST_CEIL - ceil 模式，向上取整
```

---

## 10. double2uint_rn - Double to uint round to nearest

### libdevice 描述
将双精度浮点数转换为无符号整数，最近偶数舍入

### AscendC 对应实现

**参考 API: `__float2uint_rn`**
```cpp
// 仅 Ascend 950PR/Ascend 950DT 支持
__simt_callee__ inline unsigned int __float2uint_rn(const float x);
```

**功能**: 将浮点数转换为四舍五入至最接近的偶数的无符号整数。

**特殊场景**:
- 当 x 为 nan 时，返回值为 0
- 当 x 为 inf 时，返回值为 4294967295
- 当 x 为 -inf 时，返回值为 0

#### 实现方案

**方案 1: C++ 标准转换**
```cpp
__simt_vf__ inline void kernel_double2uint_rn(__gm__ uint32_t* dst, __gm__ double* src)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    double val = src[idx];
    // 确保值为非负
    if (val < 0) val = 0;
    // 四舍五入
    dst[idx] = (uint32_t)round(val);
}
```

---

## 总结与建议

### 关键发现

1. **double 类型支持有限**
   - 虽然 AscendC 支持 `double` 数据类型（b64 位宽）
   - 但大部分类型转换和舍入控制 API 主要针对 `float` 和 `half`

2. **类型转换 API 主要集中在 SIMT 模式**
   - `__float2int_rn`, `__float2int_rz`, `__float2int_rd`, `__float2int_ru`
   - `__float2uint_rn`
   - 这些 API 仅支持 Ascend 950PR/Ascend 950DT

3. **舍入模式控制**
   - `RoundMode` 枚举定义了多种舍入模式：
     - `CAST_RINT`: 四舍六入五成双
     - `CAST_ROUND`: 四舍五入
     - `CAST_FLOOR`: 向负无穷舍入
     - `CAST_CEIL`: 向正无穷舍入
     - `CAST_TRUNC`: 向零舍入
   - 但这些主要用于 `Cast` 和 `Truncate` 指令

4. **乘法舍入**
   - AscendC 没有直接的 `mul_ru` 等带舍入控制的乘法指令
   - 需要通过 `Mul` + 取整函数组合实现

### 实现建议

1. **对于 double 类型转换**
   - 优先考虑使用 C++ 标准库函数（`round`, `floor`, `ceil`, `trunc`）
   - 在 SIMT 模式下使用 `__simt_vf__` 修饰符

2. **对于乘法舍入**
   - 使用 `Mul` 指令后接相应的取整函数
   - 注意处理溢出和特殊值（nan, inf）

3. **产品兼容性**
   - 如果目标硬件是 Ascend 950PR/950DT，可以使用更多 SIMT API
   - 如果目标硬件是 A2/A3 系列，主要依赖 C++ 标准实现

### 头文件要求

对于 SIMT API，需要包含：
```cpp
#include "simt_api/device_functions.h"
```

### 示例代码模板

```cpp
#include "simt_api/device_functions.h"

// double2float_rn - 最近偶数舍入
__simt_vf__ inline void double2float_rn_kernel(__gm__ float* dst, __gm__ double* src)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    double val = src[idx];
    // 检查特殊值
    if (isnan(val)) {
        dst[idx] = NAN;
    } else if (isinf(val)) {
        dst[idx] = val > 0 ? INFINITY : -INFINITY;
    } else {
        // 使用 round 实现最近偶数舍入
        dst[idx] = (float)round(val);
    }
}

// mul_ru - 乘法并向上舍入
__simt_vf__ inline void mul_ru_kernel(__gm__ float* dst, __gm__ float* a, __gm__ float* b)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    float product = a[idx] * b[idx];
    // 向上舍入
    dst[idx] = ceil(product);
}
```

---

## 参考文档

- `/gemini/code/huawei/asc-devkit/docs/api/context/__float2int_rn.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/__float2int_rz.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/__float2int_rd.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/__float2int_ru.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/__float2uint_rn.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/RoundMode.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/Cast-46.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/Truncate.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/内置数据类型.md`
