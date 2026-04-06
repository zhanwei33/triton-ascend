# Batch 10: libdevice 函数 AscendC 对应实现分析

## 函数列表
1. ll2float_rn - Long long to float round to nearest
2. ll2float_rz - Long long to float round toward zero
3. ll2float_rd - Long long to float round downward
4. ll2float_ru - Long long to float round upward
5. ull2float_rn - Unsigned long long to float round to nearest
6. ull2float_rz - Unsigned long long to float round toward zero
7. ull2float_rd - Unsigned long long to float round downward
8. ull2float_ru - Unsigned long long to float round upward
9. ll2double_rn - Long long to double round to nearest
10. ll2double_rz - Long long to double round toward zero

---

## 1. ll2float_rn - Long long to float round to nearest

### libdevice 函数
```c
float ll2float_rn(long long int x);
```

### AscendC SIMT API 对应实现

**函数名**: `__ll2float_rn`

**函数原型**:
```c++
__simt_callee__ inline float __ll2float_rn(const long long int x)
```

**功能说明**: 将int64类型数据转换为四舍五入到最接近偶数的浮点数。

**参数说明**:
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| x | 输入 | 源操作数 (int64/long long int) |

**返回值**: 输入的int64数据转换成的四舍五入到最接近偶数的浮点数。

**头文件**: `#include "simt_api/device_functions.h"`

**产品支持情况**:
| 产品 | 是否支持 |
|------|----------|
| Ascend 950PR/Ascend 950DT | √ |
| Atlas A3 训练/推理系列产品 | x |
| Atlas A2 训练/推理系列产品 | x |

**调用示例**:
```c++
__simt_vf__ __launch_bounds__(1024) inline void kernel__ll2float_rn(__gm__ float* dst, __gm__ int64_t* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = __ll2float_rn(x[idx]);
}
```

### AscendC C API 对应实现 (A2/A3 平台)

**函数名**: `asc_int642float_rn`

**函数原型**:
```c++
// 前n个数据计算
__aicore__ inline void asc_int642float_rn(__ubuf__ float* dst, __ubuf__ int64_t* src, uint32_t count)

// 高维切分计算
__aicore__ inline void asc_int642float_rn(__ubuf__ float* dst, __ubuf__ int64_t* src, uint8_t repeat,
    uint16_t dst_block_stride, uint16_t src_block_stride, uint16_t dst_repeat_stride, uint16_t src_repeat_stride)

// 同步计算
__aicore__ inline void asc_int642float_sync_rn(__ubuf__ float* dst, __ubuf__ int64_t* src, uint32_t count)
```

**功能说明**: 将int64_t类型数据转换为float类型，使用RINT舍入模式（四舍五入成双）。

**产品支持情况**:
| 产品 | 是否支持 |
|------|----------|
| Atlas A3 训练/推理系列产品 | √ |
| Atlas A2 训练/推理系列产品 | √ |
| Ascend 950PR/Ascend 950DT | x |

**头文件**: `#include "asc_simd.h"` 或 `#include "vector_compute.h"`

**调用示例**:
```cpp
constexpr uint64_t total_length = 128;
__ubuf__ int64_t src[total_length];
__ubuf__ float dst[total_length];
asc_int642float_rn(dst, src, total_length);
```

---

## 2. ll2float_rz - Long long to float round toward zero

### libdevice 函数
```c
float ll2float_rz(long long int x);
```

### AscendC SIMT API 对应实现

**函数名**: `__ll2float_rz`

**函数原型**:
```c++
__simt_callee__ inline float __ll2float_rz(const long long int x)
```

**功能说明**: 将int64类型数据转换为向零舍入的浮点数。

**参数说明**:
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| x | 输入 | 源操作数 (int64/long long int) |

**返回值**: 输入的int64数据转换成的向零舍入的浮点数。

**头文件**: `#include "simt_api/device_functions.h"`

**产品支持情况**: 同 ll2float_rn (仅 Ascend 950 支持)

### AscendC C API 对应实现

**函数名**: `asc_int642float_rz`

**函数原型**:
```c++
__aicore__ inline void asc_int642float_rz(__ubuf__ float* dst, __ubuf__ int64_t* src, uint32_t count)
```

**功能说明**: 将int64_t类型数据转换为float类型，使用TRUNC舍入模式（向零舍入）。

**产品支持情况**: 同 asc_int642float_rn (A2/A3 支持)

---

## 3. ll2float_rd - Long long to float round downward

### libdevice 函数
```c
float ll2float_rd(long long int x);
```

### AscendC SIMT API 对应实现

**函数名**: `__ll2float_rd`

**函数原型**:
```c++
__simt_callee__ inline float __ll2float_rd(const long long int x)
```

**功能说明**: 将int64类型数据向下取整转换为浮点数（向负无穷舍入）。

**参数说明**:
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| x | 输入 | 源操作数 (int64/long long int) |

**返回值**: 输入的int64数据向下取整转换成的浮点数。

**头文件**: `#include "simt_api/device_functions.h"`

**产品支持情况**: 同 ll2float_rn (仅 Ascend 950 支持)

### AscendC C API 对应实现

**函数名**: `asc_int642float_rd`

**函数原型**:
```c++
__aicore__ inline void asc_int642float_rd(__ubuf__ float* dst, __ubuf__ int64_t* src, uint32_t count)
```

**功能说明**: 将int64_t类型数据转换为float类型，使用FLOOR舍入模式（向负无穷舍入）。

**产品支持情况**: 同 asc_int642float_rn (A2/A3 支持)

---

## 4. ll2float_ru - Long long to float round upward

### libdevice 函数
```c
float ll2float_ru(long long int x);
```

### AscendC SIMT API 对应实现

**函数名**: `__ll2float_ru`

**函数原型**:
```c++
__simt_callee__ inline float __ll2float_ru(const long long int x)
```

**功能说明**: 将int64类型数据转换为向上取整的浮点数（向正无穷舍入）。

**参数说明**:
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| x | 输入 | 源操作数 (int64/long long int) |

**返回值**: 输入的int64数据转换成的向上取整的浮点数。

**头文件**: `#include "simt_api/device_functions.h"`

**产品支持情况**: 同 ll2float_rn (仅 Ascend 950 支持)

### AscendC C API 对应实现

**函数名**: `asc_int642float_ru`

**函数原型**:
```c++
__aicore__ inline void asc_int642float_ru(__ubuf__ float* dst, __ubuf__ int64_t* src, uint32_t count)
```

**功能说明**: 将int64_t类型数据转换为float类型，使用CEIL舍入模式（向正无穷舍入）。

**产品支持情况**: 同 asc_int642float_rn (A2/A3 支持)

---

## 5. ull2float_rn - Unsigned long long to float round to nearest

### libdevice 函数
```c
float ull2float_rn(unsigned long long int x);
```

### AscendC SIMT API 对应实现

**函数名**: `__ull2float_rn`

**函数原型**:
```c++
__simt_callee__ inline float __ull2float_rn(const unsigned long long int x)
```

**功能说明**: 将uint64类型数据转换为浮点数，并四舍五入到最接近的偶数。

**参数说明**:
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| x | 输入 | 源操作数 (uint64/unsigned long long int) |

**返回值**: 输入的uint64数据转换成的浮点数，并四舍五入到最接近的偶数。

**头文件**: `#include "simt_api/device_functions.h"`

**产品支持情况**:
| 产品 | 是否支持 |
|------|----------|
| Ascend 950PR/Ascend 950DT | √ |
| Atlas A3 训练/推理系列产品 | x |
| Atlas A2 训练/推理系列产品 | x |

**调用示例**:
```c++
__simt_vf__ __launch_bounds__(1024) inline void kernel__ull2float_rn(__gm__ float* dst, __gm__ uint64_t* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = __ull2float_rn(x[idx]);
}
```

### AscendC C API 对应实现

**说明**: 在C API中，没有直接提供 `asc_uint642float_rn` 函数。uint64到float的转换需要通过其他方式实现，如使用SIMT API或自定义实现。

---

## 6. ull2float_rz - Unsigned long long to float round toward zero

### libdevice 函数
```c
float ull2float_rz(unsigned long long int x);
```

### AscendC SIMT API 对应实现

**函数名**: `__ull2float_rz`

**函数原型**:
```c++
__simt_callee__ inline float __ull2float_rz(const unsigned long long int x)
```

**功能说明**: 将uint64类型数据转换为向零舍入的浮点数。

**参数说明**:
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| x | 输入 | 源操作数 (uint64/unsigned long long int) |

**返回值**: 输入的uint64数据转换成的向零舍入的浮点数。

**头文件**: `#include "simt_api/device_functions.h"`

**产品支持情况**: 同 ull2float_rn (仅 Ascend 950 支持)

---

## 7. ull2float_rd - Unsigned long long to float round downward

### libdevice 函数
```c
float ull2float_rd(unsigned long long int x);
```

### AscendC SIMT API 对应实现

**函数名**: `__ull2float_rd`

**函数原型**:
```c++
__simt_callee__ inline float __ull2float_rd(const unsigned long long int x)
```

**功能说明**: 将uint64类型数据向下取整转换为浮点数。

**参数说明**:
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| x | 输入 | 源操作数 (uint64/unsigned long long int) |

**返回值**: 输入的uint64数据向下取整转换成的浮点数。

**头文件**: `#include "simt_api/device_functions.h"`

**产品支持情况**: 同 ull2float_rn (仅 Ascend 950 支持)

---

## 8. ull2float_ru - Unsigned long long to float round upward

### libdevice 函数
```c
float ull2float_ru(unsigned long long int x);
```

### AscendC SIMT API 对应实现

**函数名**: `__ull2float_ru`

**函数原型**:
```c++
__simt_callee__ inline float __ull2float_ru(const unsigned long long int x)
```

**功能说明**: 将uint64类型数据向上取整转换为浮点数。

**参数说明**:
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| x | 输入 | 源操作数 (uint64/unsigned long long int) |

**返回值**: 输入的uint64数据向上取整转换成的浮点数。

**头文件**: `#include "simt_api/device_functions.h"`

**产品支持情况**: 同 ull2float_rn (仅 Ascend 950 支持)

---

## 9. ll2double_rn - Long long to double round to nearest

### libdevice 函数
```c
double ll2double_rn(long long int x);
```

### AscendC 对应实现

**分析结果**: **AscendC 未提供直接对应的 API 函数**

**说明**:
- AscendC 支持 `double` 数据类型（64位浮点数，b64位宽）
- 但是 AscendC API 文档中没有提供 `ll2double_rn` 或类似的 int64 到 double 转换函数
- `double` 类型在以下产品中得到支持：
  - Ascend 950PR/Ascend 950DT
  - Atlas A3 训练/推理系列产品
  - Atlas A2 训练/推理系列产品

**建议的替代方案**:
1. **使用 C++ 标准类型转换**:
   ```c++
   long long int x = ...;
   double d = static_cast<double>(x);  // 默认舍入模式为四舍五入到最接近
   ```

2. **使用 AscendC Cast 模板**:
   ```c++
   // 使用 AscendC::Cast 进行类型转换
   int64_t x = ...;
   double d = AscendC::Cast<int64_t, double, AscendC::RoundMode::CAST_RINT>(x);
   ```

3. **对于矢量数据**，可以使用通用的数据类型转换方法，通过中间类型转换实现。

---

## 10. ll2double_rz - Long long to double round toward zero

### libdevice 函数
```c
double ll2double_rz(long long int x);
```

### AscendC 对应实现

**分析结果**: **AscendC 未提供直接对应的 API 函数**

**说明**:
- 与 `ll2double_rn` 相同，AscendC 没有提供带特定舍入模式的 int64 到 double 转换函数

**建议的替代方案**:
1. **使用 C++ 标准类型转换**:
   ```c++
   long long int x = ...;
   double d = static_cast<double>(x);  // 对于大多数架构，向零舍入是默认行为
   ```

2. **使用 AscendC Cast 模板指定舍入模式**:
   ```c++
   int64_t x = ...;
   double d = AscendC::Cast<int64_t, double, AscendC::RoundMode::CAST_TRUNC>(x);
   ```

---

## 总结

### 支持的函数映射

| libdevice 函数 | AscendC SIMT API (Ascend 950) | AscendC C API (A2/A3) |
|----------------|-------------------------------|----------------------|
| ll2float_rn | `__ll2float_rn` | `asc_int642float_rn` |
| ll2float_rz | `__ll2float_rz` | `asc_int642float_rz` |
| ll2float_rd | `__ll2float_rd` | `asc_int642float_rd` |
| ll2float_ru | `__ll2float_ru` | `asc_int642float_ru` |
| ull2float_rn | `__ull2float_rn` | 无直接对应 |
| ull2float_rz | `__ull2float_rz` | 无直接对应 |
| ull2float_rd | `__ull2float_rd` | 无直接对应 |
| ull2float_ru | `__ull2float_ru` | 无直接对应 |
| ll2double_rn | 无直接对应 | 无直接对应 |
| ll2double_rz | 无直接对应 | 无直接对应 |

### 硬件平台兼容性说明

1. **SIMT API (`__ll2float_*`, `__ull2float_*`)**:
   - 仅支持 **Ascend 950PR/Ascend 950DT**
   - 不支持 Atlas A2/A3 系列产品
   - 头文件: `#include "simt_api/device_functions.h"`

2. **C API (`asc_int642float_*`)**:
   - 仅支持 **Atlas A2/A3 训练/推理系列产品**
   - 不支持 Ascend 950
   - 头文件: `#include "asc_simd.h"` 或 `#include "vector_compute.h"`

3. **ll2double / ull2double 系列**:
   - AscendC 目前**未提供**直接的对应 API
   - 需要使用标准 C++ 类型转换或 AscendC Cast 模板作为替代方案

### 舍入模式对应关系

| 后缀 | 舍入模式 | 说明 |
|------|----------|------|
| _rn | RINT/ROUND | 四舍五入到最接近的偶数 |
| _rz | TRUNC | 向零舍入 |
| _rd | FLOOR | 向下舍入（向负无穷） |
| _ru | CEIL | 向上舍入（向正无穷） |
| _rna | ROUND | 四舍五入（远离零） |

---

## 参考文档

- `/gemini/code/huawei/asc-devkit/docs/api/context/__ll2float_rn.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/__ll2float_rz.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/__ll2float_rd.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/__ll2float_ru.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/__ull2float_rn.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/__ull2float_rz.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/__ull2float_rd.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/__ull2float_ru.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/c_api/vector_compute/asc_int642float.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/c_api/reg/reg_vector/asc_int642float.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/内置数据类型.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/类型转换-141.md`
