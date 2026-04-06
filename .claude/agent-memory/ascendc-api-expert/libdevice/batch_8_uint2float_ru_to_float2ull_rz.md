# Libdevice 函数 AscendC 对应实现分析 - Batch 8

## 概述

本文档分析以下10个 libdevice 函数在 AscendC 中的对应实现：
1. uint2float_ru - Uint to float round upward
2. hiloint2double - High/low int to double
3. double2loint - Double to low int
4. double2hiint - Double to high int
5. float2ll_rn - Float to long long round to nearest
6. float2ll_rz - Float to long long round toward zero
7. float2ll_rd - Float to long long round downward
8. float2ll_ru - Float to long long round upward
9. float2ull_rn - Float to unsigned long long round to nearest
10. float2ull_rz - Float to unsigned long long round toward zero

---

## 1. uint2float_ru - Uint to float round upward

### Libdevice 函数信息
- **函数名**: `__uint2float_ru`
- **功能**: 将 uint32 类型数据转换为向上取整的浮点数
- **CUDA 函数**: `__nv_uint2float_ru`

### AscendC 对应实现

| 属性 | 说明 |
|------|------|
| **函数名** | `__uint2float_ru` |
| **头文件** | `simt_api/device_functions.h` |
| **函数原型** | `__simt_callee__ inline float __uint2float_ru(const unsigned int x)` |
| **参数** | `x` - 输入的 uint32 类型源操作数 |
| **返回值** | 输入的 uint32 数据转换成的向上取整的浮点数 |

### 产品支持情况

| 产品 | 是否支持 |
|------|----------|
| Ascend 950PR/Ascend 950DT | √ |
| Atlas A3 训练/推理系列产品 | × |
| Atlas A2 训练/推理系列产品 | × |

### 使用示例
```cpp
#include "simt_api/device_functions.h"

__simt_vf__ __launch_bounds__(1024) inline void kernel__uint2float_ru(__gm__ float* dst, __gm__ uint32_t* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = __uint2float_ru(x[idx]);
}
```

### 注意事项
- 仅支持 Ascend 950PR/Ascend 950DT 产品
- 属于 SIMT 编程模型 API

---

## 2. hiloint2double - High/low int to double

### Libdevice 函数信息
- **函数名**: `hiloint2double`
- **功能**: 将两个 int32 值（高位和低位）组合成 double 类型
- **CUDA 函数**: `__nv_hiloint2double(int hi, int lo)`

### AscendC 对应实现

**重要发现**: 在 AscendC API 文档中**未找到**与 `hiloint2double` 直接对应的函数。

### 替代实现方案

由于 AscendC 支持 `double` 数据类型（b64 位宽），可以通过以下方式实现：

1. **使用联合体 (Union) 方式**:
```cpp
union DoubleInt {
    double d;
    int64_t i;
    struct {
        int32_t lo;
        int32_t hi;
    } parts;
};

__aicore__ inline double hiloint2double(int32_t hi, int32_t lo) {
    DoubleInt di;
    di.parts.hi = hi;
    di.parts.lo = lo;
    return di.d;
}
```

2. **使用位运算方式**:
```cpp
__aicore__ inline double hiloint2double(int32_t hi, int32_t lo) {
    int64_t val = ((int64_t)hi << 32) | (uint32_t)lo;
    return *reinterpret_cast<double*>(&val);
}
```

### 产品支持情况
- double 类型在 Ascend 950PR/Ascend 950DT、Atlas A2、Atlas A3 上均支持

---

## 3. double2loint - Double to low int

### Libdevice 函数信息
- **函数名**: `double2loint`
- **功能**: 提取 double 值的低 32 位作为 int
- **CUDA 函数**: `__nv_double2loint(double x)`

### AscendC 对应实现

**重要发现**: 在 AscendC API 文档中**未找到**与 `double2loint` 直接对应的函数。

### 替代实现方案

```cpp
union DoubleInt {
    double d;
    int64_t i;
    struct {
        int32_t lo;
        int32_t hi;
    } parts;
};

__aicore__ inline int32_t double2loint(double x) {
    DoubleInt di;
    di.d = x;
    return di.parts.lo;
}
```

或使用位运算：
```cpp
__aicore__ inline int32_t double2loint(double x) {
    int64_t val = *reinterpret_cast<int64_t*>(&x);
    return (int32_t)(val & 0xFFFFFFFF);
}
```

---

## 4. double2hiint - Double to high int

### Libdevice 函数信息
- **函数名**: `double2hiint`
- **功能**: 提取 double 值的高 32 位作为 int
- **CUDA 函数**: `__nv_double2hiint(double x)`

### AscendC 对应实现

**重要发现**: 在 AscendC API 文档中**未找到**与 `double2hiint` 直接对应的函数。

### 替代实现方案

```cpp
union DoubleInt {
    double d;
    int64_t i;
    struct {
        int32_t lo;
        int32_t hi;
    } parts;
};

__aicore__ inline int32_t double2hiint(double x) {
    DoubleInt di;
    di.d = x;
    return di.parts.hi;
}
```

或使用位运算：
```cpp
__aicore__ inline int32_t double2hiint(double x) {
    int64_t val = *reinterpret_cast<int64_t*>(&x);
    return (int32_t)(val >> 32);
}
```

---

## 5. float2ll_rn - Float to long long round to nearest

### Libdevice 函数信息
- **函数名**: `float2ll_rn`
- **功能**: 将浮点数转换为有符号64位整数，四舍五入到最接近的偶数
- **CUDA 函数**: `__nv_float2ll_rn`

### AscendC 对应实现

| 属性 | 说明 |
|------|------|
| **函数名** | `__float2ll_rn` |
| **头文件** | `simt_api/device_functions.h` |
| **函数原型** | `__simt_callee__ inline long long int __float2ll_rn(const float x)` |
| **参数** | `x` - 输入的 float 类型源操作数 |
| **返回值** | 将输入转换为有符号64位整数，并四舍五入到最接近的偶数 |

### 特殊场景返回值

| 输入场景 | 返回值 |
|----------|--------|
| x 为 nan | 0 |
| x 为 inf | 9223372036854775807 (LLONG_MAX) |
| x 为 -inf | -9223372036854775808 (LLONG_MIN) |

### 产品支持情况

| 产品 | 是否支持 |
|------|----------|
| Ascend 950PR/Ascend 950DT | √ |
| Atlas A3 训练/推理系列产品 | × |
| Atlas A2 训练/推理系列产品 | × |

### 使用示例
```cpp
#include "simt_api/device_functions.h"

__simt_vf__ __launch_bounds__(1024) inline void kernel__float2ll_rn(__gm__ int64_t* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = __float2ll_rn(x[idx]);
}
```

### 替代方案 (C API)
对于 A2/A3 产品，可以使用 C API：
```cpp
// 使用 asc_float2int64_rn
__ubuf__ int64_t dst[256];
__ubuf__ float src[256];
asc_float2int64_rn(dst, src, 256);
```

---

## 6. float2ll_rz - Float to long long round toward zero

### Libdevice 函数信息
- **函数名**: `float2ll_rz`
- **功能**: 将浮点数转换为向零舍入的64位有符号整数
- **CUDA 函数**: `__nv_float2ll_rz`

### AscendC 对应实现

| 属性 | 说明 |
|------|------|
| **函数名** | `__float2ll_rz` |
| **头文件** | `simt_api/device_functions.h` |
| **函数原型** | `__simt_callee__ inline long long int __float2ll_rz(const float x)` |
| **参数** | `x` - 输入的 float 类型源操作数 |
| **返回值** | 将浮点数转换为向零舍入的64位有符号整数 |

### 特殊场景返回值

| 输入场景 | 返回值 |
|----------|--------|
| x 为 nan | 0 |
| x 为 inf | 9223372036854775807 (LLONG_MAX) |
| x 为 -inf | -9223372036854775808 (LLONG_MIN) |

### 产品支持情况

| 产品 | 是否支持 |
|------|----------|
| Ascend 950PR/Ascend 950DT | √ |
| Atlas A3 训练/推理系列产品 | × |
| Atlas A2 训练/推理系列产品 | × |

### 使用示例
```cpp
#include "simt_api/device_functions.h"

__simt_vf__ __launch_bounds__(1024) inline void kernel__float2ll_rz(__gm__ int64_t* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = __float2ll_rz(x[idx]);
}
```

### 替代方案 (C API)
```cpp
asc_float2int64_rz(dst, src, count);
```

---

## 7. float2ll_rd - Float to long long round downward

### Libdevice 函数信息
- **函数名**: `float2ll_rd`
- **功能**: 将浮点数转换为向下取整的64位有符号整数
- **CUDA 函数**: `__nv_float2ll_rd`

### AscendC 对应实现

| 属性 | 说明 |
|------|------|
| **函数名** | `__float2ll_rd` |
| **头文件** | `simt_api/device_functions.h` |
| **函数原型** | `__simt_callee__ inline long long int __float2ll_rd(const float x)` |
| **参数** | `x` - 输入的 float 类型源操作数 |
| **返回值** | 将浮点数转换为向下取整的64位有符号整数 |

### 特殊场景返回值

| 输入场景 | 返回值 |
|----------|--------|
| x 为 nan | 0 |
| x 为 inf | 9223372036854775807 (LLONG_MAX) |
| x 为 -inf | -9223372036854775808 (LLONG_MIN) |

### 产品支持情况

| 产品 | 是否支持 |
|------|----------|
| Ascend 950PR/Ascend 950DT | √ |
| Atlas A3 训练/推理系列产品 | × |
| Atlas A2 训练/推理系列产品 | × |

### 使用示例
```cpp
#include "simt_api/device_functions.h"

__simt_vf__ __launch_bounds__(1024) inline void kernel__float2ll_rd(__gm__ int64_t* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = __float2ll_rd(x[idx]);
}
```

### 替代方案 (C API)
```cpp
asc_float2int64_rd(dst, src, count);
```

---

## 8. float2ll_ru - Float to long long round upward

### Libdevice 函数信息
- **函数名**: `float2ll_ru`
- **功能**: 将浮点数转换为向上取整的64位有符号整数
- **CUDA 函数**: `__nv_float2ll_ru`

### AscendC 对应实现

| 属性 | 说明 |
|------|------|
| **函数名** | `__float2ll_ru` |
| **头文件** | `simt_api/device_functions.h` |
| **函数原型** | `__simt_callee__ inline long long int __float2ll_ru(const float x)` |
| **参数** | `x` - 输入的 float 类型源操作数 |
| **返回值** | 将浮点数转换为向上取整的64位有符号整数 |

### 特殊场景返回值

| 输入场景 | 返回值 |
|----------|--------|
| x 为 nan | 0 |
| x 为 inf | 9223372036854775807 (LLONG_MAX) |
| x 为 -inf | -9223372036854775808 (LLONG_MIN) |

### 产品支持情况

| 产品 | 是否支持 |
|------|----------|
| Ascend 950PR/Ascend 950DT | √ |
| Atlas A3 训练/推理系列产品 | × |
| Atlas A2 训练/推理系列产品 | × |

### 使用示例
```cpp
#include "simt_api/device_functions.h"

__simt_vf__ __launch_bounds__(1024) inline void kernel__float2ll_ru(__gm__ int64_t* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = __float2ll_ru(x[idx]);
}
```

### 替代方案 (C API)
```cpp
asc_float2int64_ru(dst, src, count);
```

---

## 9. float2ull_rn - Float to unsigned long long round to nearest

### Libdevice 函数信息
- **函数名**: `float2ull_rn`
- **功能**: 将浮点数转换为四舍五入至最接近偶数的64位无符号整数
- **CUDA 函数**: `__nv_float2ull_rn`

### AscendC 对应实现

| 属性 | 说明 |
|------|------|
| **函数名** | `__float2ull_rn` |
| **头文件** | `simt_api/device_functions.h` |
| **函数原型** | `__simt_callee__ inline unsigned long long int __float2ull_rn(const float x)` |
| **参数** | `x` - 输入的 float 类型源操作数 |
| **返回值** | 将输入转换为四舍五入到最接近偶数的64位无符号整数 |

### 特殊场景返回值

| 输入场景 | 返回值 |
|----------|--------|
| x 为 nan | 0 |
| x 为 inf | 18446744073709551615 (ULLONG_MAX) |
| x 为 -inf | 0 |

### 产品支持情况

| 产品 | 是否支持 |
|------|----------|
| Ascend 950PR/Ascend 950DT | √ |
| Atlas A3 训练/推理系列产品 | × |
| Atlas A2 训练/推理系列产品 | × |

### 使用示例
```cpp
#include "simt_api/device_functions.h"

__simt_vf__ __launch_bounds__(1024) inline void kernel__float2ull_rn(__gm__ uint64_t* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = __float2ull_rn(x[idx]);
}
```

---

## 10. float2ull_rz - Float to unsigned long long round toward zero

### Libdevice 函数信息
- **函数名**: `float2ull_rz`
- **功能**: 将浮点数转换为向零舍入的64位无符号整数
- **CUDA 函数**: `__nv_float2ull_rz`

### AscendC 对应实现

| 属性 | 说明 |
|------|------|
| **函数名** | `__float2ull_rz` |
| **头文件** | `simt_api/device_functions.h` |
| **函数原型** | `__simt_callee__ inline unsigned long long int __float2ull_rz(const float x)` |
| **参数** | `x` - 输入的 float 类型源操作数 |
| **返回值** | 将浮点数转换为向零舍入的64位无符号整数 |

### 特殊场景返回值

| 输入场景 | 返回值 |
|----------|--------|
| x 为 nan | 0 |
| x 为 inf | 18446744073709551615 (ULLONG_MAX) |
| x 为 -inf | 0 |

### 产品支持情况

| 产品 | 是否支持 |
|------|----------|
| Ascend 950PR/Ascend 950DT | √ |
| Atlas A3 训练/推理系列产品 | × |
| Atlas A2 训练/推理系列产品 | × |

### 使用示例
```cpp
#include "simt_api/device_functions.h"

__simt_vf__ __launch_bounds__(1024) inline void kernel__float2ull_rz(__gm__ uint64_t* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = __float2ull_rz(x[idx]);
}
```

---

## 总结

### 函数对应情况汇总

| 序号 | Libdevice 函数 | AscendC 对应函数 | 支持情况 | 备注 |
|------|---------------|------------------|----------|------|
| 1 | uint2float_ru | `__uint2float_ru` | 仅 Ascend 950 | SIMT API |
| 2 | hiloint2double | **无直接对应** | - | 需使用 union 或位运算实现 |
| 3 | double2loint | **无直接对应** | - | 需使用 union 或位运算实现 |
| 4 | double2hiint | **无直接对应** | - | 需使用 union 或位运算实现 |
| 5 | float2ll_rn | `__float2ll_rn` | 仅 Ascend 950 | SIMT API |
| 6 | float2ll_rz | `__float2ll_rz` | 仅 Ascend 950 | SIMT API |
| 7 | float2ll_rd | `__float2ll_rd` | 仅 Ascend 950 | SIMT API |
| 8 | float2ll_ru | `__float2ll_ru` | 仅 Ascend 950 | SIMT API |
| 9 | float2ull_rn | `__float2ull_rn` | 仅 Ascend 950 | SIMT API |
| 10 | float2ull_rz | `__float2ull_rz` | 仅 Ascend 950 | SIMT API |

### 重要发现

1. **SIMT API 限制**: 本文档中的类型转换函数（除 hiloint2double/double2loint/double2hiint 外）均属于 SIMT 编程模型，且**仅支持 Ascend 950PR/Ascend 950DT 产品**。

2. **A2/A3 产品替代方案**: 对于 Atlas A2/A3 产品，需要使用 C API 进行替代：
   - `asc_float2int64_*` 系列函数用于 float 到 int64 的转换
   - 但没有直接的标量类型转换函数

3. **double 位操作函数缺失**: AscendC 中没有直接对应 `hiloint2double`、`double2loint`、`double2hiint` 的函数，需要使用 C++ 的 union 或位运算自行实现。

4. **头文件差异**:
   - SIMT API: `#include "simt_api/device_functions.h"`
   - C API: 使用 `asc_*` 前缀的函数

### 迁移建议

1. 对于 Ascend 950 产品，可以直接使用对应的 SIMT API 函数。
2. 对于 A2/A3 产品，float 到 int64 的转换需要使用 C API 的矢量操作函数。
3. double 的位操作需要自定义实现，使用 union 或 reinterpret_cast 进行类型转换。
