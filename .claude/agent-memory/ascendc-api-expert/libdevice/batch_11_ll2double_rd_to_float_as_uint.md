# Libdevice 函数 AscendC 对应实现分析 - Batch 11

## 分析概述

本批次分析涵盖 10 个 libdevice 函数，主要涉及：
1. Long long / Unsigned long long 到 double 的类型转换（带舍入模式）
2. Int/Uint 与 Float 之间的位解释转换（bit cast）

## 关键发现摘要

### 1. ll2double_rd / ll2double_ru - Long long to double
**状态：无直接对应实现**

AscendC 提供了 `__ll2float_*` 系列函数（将 int64 转换为 float），但没有提供直接转换为 `double` 的对应函数。

| libdevice 函数 | 功能描述 | AscendC 对应 | 状态 |
|---------------|---------|-------------|------|
| `ll2double_rd` | Long long to double, round downward | 无直接对应 | 不支持 |
| `ll2double_ru` | Long long to double, round upward | 无直接对应 | 不支持 |

**替代方案：**
- 使用 C++ 标准类型转换：`static_cast<double>(value)`
- 或使用 `__ll2float_rn` 等函数转换为 float（会有精度损失）

### 2. ull2double_rn / ull2double_rz / ull2double_rd / ull2double_ru - Unsigned long long to double
**状态：无直接对应实现**

与 ll2double 类似，AscendC 只提供了 `__ull2float_*` 系列函数，没有直接转换为 double 的版本。

| libdevice 函数 | 功能描述 | AscendC 对应 | 状态 |
|---------------|---------|-------------|------|
| `ull2double_rn` | Unsigned long long to double, round to nearest | 无直接对应 | 不支持 |
| `ull2double_rz` | Unsigned long long to double, round toward zero | 无直接对应 | 不支持 |
| `ull2double_rd` | Unsigned long long to double, round downward | 无直接对应 | 不支持 |
| `ull2double_ru` | Unsigned long long to double, round upward | 无直接对应 | 不支持 |

**替代方案：**
- 使用 C++ 标准类型转换：`static_cast<double>(value)`

### 3. int_as_float - Int as float (bit cast)
**状态：有直接对应实现**

| libdevice 函数 | AscendC 对应函数 | 函数原型 |
|---------------|-----------------|----------|
| `int_as_float` | `__int_as_float` | `__simt_callee__ inline float __int_as_float(const int x)` |

**详细说明：**
- **功能**：将整数中的位重新解释为浮点数，即将整数存储的位按照 float 的格式进行读取
- **头文件**：`#include "simt_api/device_functions.h"`
- **产品支持**：仅 Ascend 950PR/Ascend 950DT 支持，A2/A3 不支持
- **返回值**：输入的整数中的位重新解释成的浮点数

**代码示例：**
```cpp
#include "simt_api/device_functions.h"

__simt_vf__ __launch_bounds__(1024) inline void kernel_int_as_float(__gm__ float* dst, __gm__ int32_t* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = __int_as_float(x[idx]);
}
```

### 4. float_as_int - Float as int (bit cast)
**状态：有直接对应实现**

| libdevice 函数 | AscendC 对应函数 | 函数原型 |
|---------------|-----------------|----------|
| `float_as_int` | `__float_as_int` | `__simt_callee__ inline int __float_as_int(const float x)` |

**详细说明：**
- **功能**：将浮点数中的位重新解释为有符号整数，即将浮点数存储的位按照有符号整数的格式进行读取
- **头文件**：`#include "simt_api/device_functions.h"`
- **产品支持**：仅 Ascend 950PR/Ascend 950DT 支持，A2/A3 不支持
- **特殊返回值**：
  - 当 x 为 nan 时，返回值为 2143289344
  - 当 x 为 inf 时，返回值为 2139095040
  - 当 x 为 -inf 时，返回值为 -8388608

**代码示例：**
```cpp
#include "simt_api/device_functions.h"

__simt_vf__ __launch_bounds__(1024) inline void kernel_float_as_int(__gm__ int32_t* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = __float_as_int(x[idx]);
}
```

### 5. uint_as_float - Uint as float (bit cast)
**状态：有直接对应实现**

| libdevice 函数 | AscendC 对应函数 | 函数原型 |
|---------------|-----------------|----------|
| `uint_as_float` | `__uint_as_float` | `__simt_callee__ inline float __uint_as_float(const unsigned int x)` |

**详细说明：**
- **功能**：将无符号整数中的位重新解释为浮点数，即将无符号整数存储的位按照 float 的格式进行读取
- **头文件**：`#include "simt_api/device_functions.h"`
- **产品支持**：仅 Ascend 950PR/Ascend 950DT 支持，A2/A3 不支持
- **返回值**：输入的无符号整数中的位重新解释成的浮点数

**代码示例：**
```cpp
#include "simt_api/device_functions.h"

__simt_vf__ __launch_bounds__(1024) inline void kernel_uint_as_float(__gm__ float* dst, __gm__ uint32_t* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = __uint_as_float(x[idx]);
}
```

### 6. float_as_uint - Float as uint (bit cast)
**状态：有直接对应实现**

| libdevice 函数 | AscendC 对应函数 | 函数原型 |
|---------------|-----------------|----------|
| `float_as_uint` | `__float_as_uint` | `__simt_callee__ inline unsigned int __float_as_uint(const float x)` |

**详细说明：**
- **功能**：将浮点数中的位重新解释为无符号整数，即将浮点数存储的位按照无符号整数的格式进行读取
- **头文件**：`#include "simt_api/device_functions.h"`
- **产品支持**：仅 Ascend 950PR/Ascend 950DT 支持，A2/A3 不支持
- **特殊返回值**：
  - 当 x 为 nan 时，返回值为 2143289344
  - 当 x 为 inf 时，返回值为 2139095040
  - 当 x 为 -inf 时，返回值为 4286578688

**代码示例：**
```cpp
#include "simt_api/device_functions.h"

__simt_vf__ __launch_bounds__(1024) inline void kernel_float_as_uint(__gm__ uint32_t* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = __float_as_uint(x[idx]);
}
```

## 相关 AscendC 类型转换函数

### 支持的 int64/uint64 到 float 转换

AscendC 提供了以下将 64 位整数转换为 float 的函数（仅 Ascend 950PR/950DT 支持）：

| 函数名 | 功能描述 |
|-------|---------|
| `__ll2float_rn` | int64 to float, round to nearest even |
| `__ll2float_rz` | int64 to float, round toward zero |
| `__ll2float_rd` | int64 to float, round downward |
| `__ll2float_ru` | int64 to float, round upward |
| `__ll2float_rna` | int64 to float, round to nearest away from zero |
| `__ull2float_rn` | uint64 to float, round to nearest even |
| `__ull2float_rz` | uint64 to float, round toward zero |
| `__ull2float_rd` | uint64 to float, round downward |
| `__ull2float_ru` | uint64 to float, round upward |
| `__ull2float_rna` | uint64 to float, round to nearest away from zero |

### 支持的 int64/uint64 到 half 转换

| 函数名 | 功能描述 |
|-------|---------|
| `__ll2half_rn` | int64 to half, round to nearest even |
| `__ll2half_rz` | int64 to half, round toward zero |
| `__ll2half_rd` | int64 to half, round downward |
| `__ll2half_ru` | int64 to half, round upward |
| `__ll2half_rna` | int64 to half, round to nearest away from zero |
| `__ull2half_rn` | uint64 to half, round to nearest even |
| `__ull2half_rz` | uint64 to half, round toward zero |
| `__ull2half_rd` | uint64 to half, round downward |
| `__ull2half_ru` | uint64 to half, round upward |
| `__ull2half_rna` | uint64 to half, round to nearest away from zero |

## 产品支持情况汇总

| 函数类型 | Ascend 950PR/950DT | Atlas A3 | Atlas A2 |
|---------|-------------------|----------|----------|
| `__int_as_float` | 支持 | 不支持 | 不支持 |
| `__float_as_int` | 支持 | 不支持 | 不支持 |
| `__uint_as_float` | 支持 | 不支持 | 不支持 |
| `__float_as_uint` | 支持 | 不支持 | 不支持 |
| `__ll2float_*` | 支持 | 不支持 | 不支持 |
| `__ull2float_*` | 支持 | 不支持 | 不支持 |
| `__ll2half_*` | 支持 | 不支持 | 不支持 |
| `__ull2half_*` | 支持 | 不支持 | 不支持 |

## 建议与注意事项

1. **ll2double / ull2double 系列**：AscendC 没有提供直接转换为 double 的函数，建议使用 C++ 标准类型转换 `static_cast<double>(value)`

2. **位解释转换函数**（`__int_as_float`、`__float_as_int` 等）：
   - 仅 Ascend 950PR/950DT 支持
   - 在 A2/A3 平台上需要使用替代方案，如 `reinterpret_cast` 或 `memcpy`

3. **头文件依赖**：
   - 位解释转换函数需要包含 `"simt_api/device_functions.h"`
   - ll2half 系列需要包含 `"simt_api/asc_fp16.h"`

4. **精度注意**：`__ll2float_*` 和 `__ull2float_*` 将 64 位整数转换为 32 位 float，可能会有精度损失。如需完整精度，应使用 double 类型并通过标准转换实现
