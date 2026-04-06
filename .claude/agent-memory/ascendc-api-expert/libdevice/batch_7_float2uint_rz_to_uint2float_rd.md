# Libdevice 到 AscendC API 映射分析 - Batch 7

## 分析概述

本批次分析10个 libdevice 类型转换函数在 AscendC 中的对应实现，涵盖 float 与 uint/int 之间的相互转换，支持多种舍入模式。

**关键发现**：
- 所有10个函数在 AscendC 中均有**完全对应的同名实现**
- 这些 API 属于 **SIMT 编程模型**，仅在 **Ascend 950PR/Ascend 950DT** 产品上支持
- **Atlas A2/A3 系列不支持**这些 API
- 所有函数都需要包含头文件 `"simt_api/device_functions.h"`

---

## 1. float2uint_rz - Float to uint round toward zero

### Libdevice 描述
将浮点数转换为无符号整数，向零方向舍入（截断小数部分）。

### AscendC 对应实现

| 属性 | 详情 |
|------|------|
| **函数名** | `__float2uint_rz` |
| **函数原型** | `__simt_callee__ inline unsigned int __float2uint_rz(const float x)` |
| **头文件** | `"simt_api/device_functions.h"` |
| **产品支持** | Ascend 950PR/Ascend 950DT: ✓<br>Atlas A3 训练/推理: ✗<br>Atlas A2 训练/推理: ✗ |

### 参数说明
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| x | 输入 | 源操作数（float类型） |

### 返回值说明
- 将输入转换为向零舍入的无符号整数
- **特殊场景**：
  - 当 x 为 nan 时，返回值为 0
  - 当 x 为 inf 时，返回值为 4294967295 (UINT_MAX)
  - 当 x 为 -inf 时，返回值为 0

### 使用示例
```cpp
#include "simt_api/device_functions.h"

__simt_vf__ __launch_bounds__(1024) inline void kernel_float2uint_rz(
    __gm__ uint32_t* dst,
    __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = __float2uint_rz(x[idx]);
}
```

---

## 2. float2uint_rd - Float to uint round downward

### Libdevice 描述
将浮点数转换为无符号整数，向下舍入（floor）。

### AscendC 对应实现

| 属性 | 详情 |
|------|------|
| **函数名** | `__float2uint_rd` |
| **函数原型** | `__simt_callee__ inline unsigned int __float2uint_rd(const float x)` |
| **头文件** | `"simt_api/device_functions.h"` |
| **产品支持** | Ascend 950PR/Ascend 950DT: ✓<br>Atlas A3 训练/推理: ✗<br>Atlas A2 训练/推理: ✗ |

### 参数说明
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| x | 输入 | 源操作数（float类型） |

### 返回值说明
- 将输入转换为向下取整的无符号整数
- **特殊场景**：
  - 当 x 为 nan 时，返回值为 0
  - 当 x 为 inf 时，返回值为 4294967295
  - 当 x 为 -inf 时，返回值为 0

### 使用示例
```cpp
#include "simt_api/device_functions.h"

__simt_vf__ __launch_bounds__(1024) inline void kernel_float2uint_rd(
    __gm__ uint32_t* dst,
    __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = __float2uint_rd(x[idx]);
}
```

---

## 3. float2uint_ru - Float to uint round upward

### Libdevice 描述
将浮点数转换为无符号整数，向上舍入（ceil）。

### AscendC 对应实现

| 属性 | 详情 |
|------|------|
| **函数名** | `__float2uint_ru` |
| **函数原型** | `__simt_callee__ inline unsigned int __float2uint_ru(const float x)` |
| **头文件** | `"simt_api/device_functions.h"` |
| **产品支持** | Ascend 950PR/Ascend 950DT: ✓<br>Atlas A3 训练/推理: ✗<br>Atlas A2 训练/推理: ✗ |

### 参数说明
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| x | 输入 | 源操作数（float类型） |

### 返回值说明
- 与输入浮点数最接近的整数值（向上取整）
- **特殊场景**：
  - 当 x 为 nan 时，返回值为 0
  - 当 x 为 inf 时，返回值为 4294967295
  - 当 x 为 -inf 时，返回值为 0

### 使用示例
```cpp
#include "simt_api/device_functions.h"

__simt_vf__ __launch_bounds__(1024) inline void kernel_float2uint_ru(
    __gm__ uint32_t* dst,
    __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = __float2uint_ru(x[idx]);
}
```

---

## 4. int2float_rn - Int to float round to nearest

### Libdevice 描述
将 int32 类型数据转换为浮点数，四舍五入到最接近的偶数（round to nearest even）。

### AscendC 对应实现

| 属性 | 详情 |
|------|------|
| **函数名** | `__int2float_rn` |
| **函数原型** | `__simt_callee__ inline float __int2float_rn(const int x)` |
| **头文件** | `"simt_api/device_functions.h"` |
| **产品支持** | Ascend 950PR/Ascend 950DT: ✓<br>Atlas A3 训练/推理: ✗<br>Atlas A2 训练/推理: ✗ |

### 参数说明
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| x | 输入 | 源操作数（int32类型） |

### 返回值说明
- 输入的 int32 数据转换成的浮点数，四舍五入到最接近的偶数

### 使用示例
```cpp
#include "simt_api/device_functions.h"

__simt_vf__ __launch_bounds__(1024) inline void kernel_int2float_rn(
    __gm__ float* dst,
    __gm__ int32_t* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = __int2float_rn(x[idx]);
}
```

---

## 5. int2float_rz - Int to float round toward zero

### Libdevice 描述
将 int32 类型数据转换为浮点数，向零方向舍入。

### AscendC 对应实现

| 属性 | 详情 |
|------|------|
| **函数名** | `__int2float_rz` |
| **函数原型** | `__simt_callee__ inline float __int2float_rz(const int x)` |
| **头文件** | `"simt_api/device_functions.h"` |
| **产品支持** | Ascend 950PR/Ascend 950DT: ✓<br>Atlas A3 训练/推理: ✗<br>Atlas A2 训练/推理: ✗ |

### 参数说明
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| x | 输入 | 源操作数（int32类型） |

### 返回值说明
- 输入的 int32 数据转换成的向零舍入的浮点数

### 使用示例
```cpp
#include "simt_api/device_functions.h"

__simt_vf__ __launch_bounds__(1024) inline void kernel_int2float_rz(
    __gm__ float* dst,
    __gm__ int32_t* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = __int2float_rz(x[idx]);
}
```

---

## 6. int2float_rd - Int to float round downward

### Libdevice 描述
将 int32 类型数据转换为浮点数，向下舍入（floor）。

### AscendC 对应实现

| 属性 | 详情 |
|------|------|
| **函数名** | `__int2float_rd` |
| **函数原型** | `__simt_callee__ inline float __int2float_rd(const int x)` |
| **头文件** | `"simt_api/device_functions.h"` |
| **产品支持** | Ascend 950PR/Ascend 950DT: ✓<br>Atlas A3 训练/推理: ✗<br>Atlas A2 训练/推理: ✗ |

### 参数说明
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| x | 输入 | 源操作数（int32类型） |

### 返回值说明
- 输入的 int32 数据向下取整转换成的浮点数

### 使用示例
```cpp
#include "simt_api/device_functions.h"

__simt_vf__ __launch_bounds__(1024) inline void kernel_int2float_rd(
    __gm__ float* dst,
    __gm__ int32_t* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = __int2float_rd(x[idx]);
}
```

---

## 7. int2float_ru - Int to float round upward

### Libdevice 描述
将 int32 类型数据转换为浮点数，向上舍入（ceil）。

### AscendC 对应实现

| 属性 | 详情 |
|------|------|
| **函数名** | `__int2float_ru` |
| **函数原型** | `__simt_callee__ inline float __int2float_ru(const int x)` |
| **头文件** | `"simt_api/device_functions.h"` |
| **产品支持** | Ascend 950PR/Ascend 950DT: ✓<br>Atlas A3 训练/推理: ✗<br>Atlas A2 训练/推理: ✗ |

### 参数说明
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| x | 输入 | 源操作数（int32类型） |

### 返回值说明
- 输入的 int32 数据转换成的向上取整的浮点数

### 使用示例
```cpp
#include "simt_api/device_functions.h"

__simt_vf__ __launch_bounds__(1024) inline void kernel_int2float_ru(
    __gm__ float* dst,
    __gm__ int32_t* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = __int2float_ru(x[idx]);
}
```

---

## 8. uint2float_rn - Uint to float round to nearest

### Libdevice 描述
将 uint32 类型数据转换为浮点数，四舍五入到最接近的偶数。

### AscendC 对应实现

| 属性 | 详情 |
|------|------|
| **函数名** | `__uint2float_rn` |
| **函数原型** | `__simt_callee__ inline float __uint2float_rn(const unsigned int x)` |
| **头文件** | `"simt_api/device_functions.h"` |
| **产品支持** | Ascend 950PR/Ascend 950DT: ✓<br>Atlas A3 训练/推理: ✗<br>Atlas A2 训练/推理: ✗ |

### 参数说明
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| x | 输入 | 源操作数（uint32类型） |

### 返回值说明
- 输入的无符号整数转换成的浮点数，四舍五入到最接近的偶数

### 使用示例
```cpp
#include "simt_api/device_functions.h"

__simt_vf__ __launch_bounds__(1024) inline void kernel_uint2float_rn(
    __gm__ float* dst,
    __gm__ uint32_t* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = __uint2float_rn(x[idx]);
}
```

---

## 9. uint2float_rz - Uint to float round toward zero

### Libdevice 描述
将 uint32 类型数据转换为浮点数，向零方向舍入。

### AscendC 对应实现

| 属性 | 详情 |
|------|------|
| **函数名** | `__uint2float_rz` |
| **函数原型** | `__simt_callee__ inline float __uint2float_rz(const unsigned int x)` |
| **头文件** | `"simt_api/device_functions.h"` |
| **产品支持** | Ascend 950PR/Ascend 950DT: ✓<br>Atlas A3 训练/推理: ✗<br>Atlas A2 训练/推理: ✗ |

### 参数说明
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| x | 输入 | 源操作数（uint32类型） |

### 返回值说明
- 将输入的 uint32 类型数据转换成的向零舍入的浮点数

### 使用示例
```cpp
#include "simt_api/device_functions.h"

__simt_vf__ __launch_bounds__(1024) inline void kernel_uint2float_rz(
    __gm__ float* dst,
    __gm__ uint32_t* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = __uint2float_rz(x[idx]);
}
```

---

## 10. uint2float_rd - Uint to float round downward

### Libdevice 描述
将 uint32 类型数据转换为浮点数，向下舍入（floor）。

### AscendC 对应实现

| 属性 | 详情 |
|------|------|
| **函数名** | `__uint2float_rd` |
| **函数原型** | `__simt_callee__ inline float __uint2float_rd(const unsigned int x)` |
| **头文件** | `"simt_api/device_functions.h"` |
| **产品支持** | Ascend 950PR/Ascend 950DT: ✓<br>Atlas A3 训练/推理: ✗<br>Atlas A2 训练/推理: ✗ |

### 参数说明
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| x | 输入 | 源操作数（uint32类型） |

### 返回值说明
- 将输入的 uint32 数据向下取整转换成的浮点数

### 使用示例
```cpp
#include "simt_api/device_functions.h"

__simt_vf__ __launch_bounds__(1024) inline void kernel_uint2float_rd(
    __gm__ float* dst,
    __gm__ uint32_t* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = __uint2float_rd(x[idx]);
}
```

---

## 舍入模式后缀说明

| 后缀 | 舍入模式 | 说明 |
|------|----------|------|
| `_rn` | Round to Nearest | 四舍五入到最接近的偶数（银行家舍入） |
| `_rz` | Round toward Zero | 向零方向舍入（截断） |
| `_rd` | Round Downward | 向下舍入（floor） |
| `_ru` | Round Upward | 向上舍入（ceil） |
| `_rna` | Round Nearest Away | 向远离零方向舍入 |

---

## 重要兼容性提示

### ⚠️ 硬件兼容性警告

**这些 API 仅支持 Ascend 950PR/Ascend 950DT 产品！**

| 产品 | 支持状态 |
|------|----------|
| Ascend 950PR/Ascend 950DT | ✓ 支持 |
| Atlas A3 训练系列产品 | ✗ 不支持 |
| Atlas A3 推理系列产品 | ✗ 不支持 |
| Atlas A2 训练系列产品 | ✗ 不支持 |
| Atlas A2 推理系列产品 | ✗ 不支持 |

### 编程模型要求

这些函数属于 **SIMT（Single Instruction Multiple Threads）编程模型**，使用时需要注意：

1. 函数需要使用 `__simt_callee__` 修饰
2. 内核函数需要使用 `__simt_vf__` 修饰
3. 需要使用 `__launch_bounds__` 指定线程数

### 头文件要求

```cpp
#include "simt_api/device_functions.h"
```

### 替代方案（A2/A3 系列）

如果在 Atlas A2/A3 系列产品上需要类似功能，可以考虑：

1. 使用标准的 C++ 类型转换（可能无法精确控制舍入模式）
2. 使用 AscendC 的 `Cast` 类 API（需要查阅具体支持的类型转换组合）
3. 使用 `ScalarCast` 进行标量类型转换

---

## 总结

本批次分析的10个类型转换函数在 AscendC 中均有**完全对应的同名实现**，函数名和语义与 libdevice 完全一致。主要限制在于：

1. **硬件限制**：仅 Ascend 950PR/Ascend 950DT 支持
2. **编程模型**：需要使用 SIMT 编程模型
3. **头文件**：需要包含 `simt_api/device_functions.h`

对于需要精确控制浮点数舍入行为的场景，这些 API 提供了完整的解决方案。
