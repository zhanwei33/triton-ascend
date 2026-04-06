# Libdevice 函数到 AscendC API 的对应实现分析 (Batch 14)

分析 10 个 libdevice 函数在 AscendC 中的对应实现方式。

**分析日期**: 2026-03-31
**API 文档路径**: `/gemini/code/huawei/asc-devkit/docs/api/context/`

---

## 1. llrint - Round to long long

### Libdevice 原型
```c
long long int llrint(double x);
long long int llrintf(float x);
```

### AscendC 对应实现
**函数名**: `llrintf`

**函数原型**:
```c
__simt_callee__ inline long long int llrintf(float x)
```

**头文件**:
```c
#include "simt_api/math_functions.h"
```

**功能说明**:
获取与输入数据最接近的整数，若存在两个同样接近的整数，则获取其中的偶数（银行家舍入法）。

**参数**:
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| x | 输入 | 源操作数（float 类型） |

**返回值**:
- 与输入浮点数最接近的整数值
- 当 x = 0 时，返回值为 0
- 当 x = 0.5 时，返回值为 0（偶数舍入）
- 当 x = 1.5 时，返回值为 2
- 当 x = NaN 时，返回值为 0

**产品支持**:
| 产品 | 支持情况 |
|------|----------|
| Ascend 950PR/Ascend 950DT | √ |
| Atlas A3 训练/推理系列产品 | × |
| Atlas A2 训练/推理系列产品 | × |

**调用示例**:
```c
__simt_vf__ __launch_bounds__(1024) inline void KernelRint(__gm__ long long int* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = llrintf(x[idx]);
}
```

**差异说明**:
- libdevice 提供 `llrint` (double) 和 `llrintf` (float) 两个版本
- AscendC 仅提供 `llrintf` (float) 版本
- 对于 double 类型输入，需要类型转换为 float 后再调用

---

## 2. nearbyint - Nearby integer (round without raising inexact)

### Libdevice 原型
```c
double nearbyint(double x);
float nearbyintf(float x);
```

### AscendC 对应实现
**函数名**: `nearbyintf`

**函数原型**:
```c
__simt_callee__ inline float nearbyintf(float x)
```

**头文件**:
```c
#include "simt_api/math_functions.h"
```

**功能说明**:
获取与输入浮点数最接近的整数，输入浮点数与左右整数的距离相等时，返回偶数（银行家舍入法）。
与 round 函数不同，nearbyint 不会引发浮点异常。

**参数**:
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| x | 输入 | 源操作数（float 类型） |

**返回值**:
- 最接近浮点数的整数
- 若 x = inf，返回值为 inf
- 若 x = -inf，返回值为 -inf
- 若 x = NaN，返回值为 NaN

**产品支持**:
| 产品 | 支持情况 |
|------|----------|
| Ascend 950PR/Ascend 950DT | √ |
| Atlas A3 训练/推理系列产品 | × |
| Atlas A2 训练/推理系列产品 | × |

**调用示例**:
```c
__simt_vf__ __launch_bounds__(1024) inline void KernelNearByInt(__gm__ float* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = nearbyintf(x[idx]);
}
```

**差异说明**:
- AscendC 仅提供 float 版本 `nearbyintf`
- 返回值类型为 float 而非整数类型

---

## 3. isnan - Check if NaN

### Libdevice 原型
```c
int isnan(double x);
int isnan(float x);
```

### AscendC 对应实现
**函数名**: `isnan`

**函数原型**:
```c
__simt_callee__ inline bool isnan(float x)
```

**头文件**:
```c
#include "simt_api/math_functions.h"
```

**功能说明**:
判断浮点数是否为 NaN（Not a Number）。

**参数**:
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| x | 输入 | 源操作数（float 类型） |

**返回值**:
- false：输入非 NaN
- true：输入为 NaN

**产品支持**:
| 产品 | 支持情况 |
|------|----------|
| Ascend 950PR/Ascend 950DT | √ |
| Atlas A3 训练/推理系列产品 | × |
| Atlas A2 训练/推理系列产品 | × |

**调用示例**:
```c
__simt_vf__ __launch_bounds__(1024) inline void KernelIsNan(__gm__ bool* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = isnan(x[idx]);
}
```

**差异说明**:
- 返回值类型为 `bool` 而非 `int`
- 仅支持 float 类型输入

---

## 4. signbit - Get sign bit

### Libdevice 原型
```c
int signbit(double x);
int signbit(float x);
```

### AscendC 对应实现
**状态**: 无直接对应实现

**替代方案**:
AscendC SIMT API 中没有提供 `signbit` 函数的直接实现。可以通过以下方式实现：

**方案 1**: 使用 `copysignf` 结合比较运算
```c
// 判断符号位：返回 true 表示负数，false 表示正数
inline bool signbit_impl(float x) {
    return x < 0.0f || (x == 0.0f && copysignf(1.0f, x) < 0);
}
```

**方案 2**: 使用位运算（推荐）
```c
// 直接检查 IEEE 754 符号位
inline bool signbit_impl(float x) {
    union { float f; uint32_t u; } val = {x};
    return (val.u >> 31) != 0;
}
```

**注意**: signbit 需要区分 +0 和 -0，简单的 x < 0 判断会失败。

---

## 5. copysign - Copy sign from one value to another

### Libdevice 原型
```c
double copysign(double x, double y);
float copysignf(float x, float y);
```

### AscendC 对应实现
**函数名**: `copysignf`

**函数原型**:
```c
__simt_callee__ inline float copysignf(float x, float y)
```

**头文件**:
```c
#include "simt_api/math_functions.h"
```

**功能说明**:
获取由第一个输入 x 的数值部分和第二个输入 y 的符号部分拼接得到的浮点数。

**参数**:
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| x | 输入 | 源操作数（提供数值部分） |
| y | 输入 | 源操作数（提供符号部分） |

**返回值**:
- y >= 0 时，返回 x 的绝对值 Abs(x)
- y < 0 时，返回 x 绝对值的相反数，-Abs(x)
- y = NaN 时，返回 -Abs(x)
- y = -inf 时，返回 -Abs(x)
- y = inf 时，返回 -Abs(x)

**产品支持**:
| 产品 | 支持情况 |
|------|----------|
| Ascend 950PR/Ascend 950DT | √ |
| Atlas A3 训练/推理系列产品 | × |
| Atlas A2 训练/推理系列产品 | × |

**调用示例**:
```c
__simt_vf__ __launch_bounds__(1024) inline void KerneCopySign(__gm__ float* dst, __gm__ float* x, __gm__ float* y)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = copysignf(x[idx], y[idx]);
}
```

---

## 6. finitef - Check if finite

### Libdevice 原型
```c
int finite(double x);
int finitef(float x);
```

### AscendC 对应实现
**函数名**: `isfinite`

**函数原型**:
```c
__simt_callee__ inline bool isfinite(float x)
```

**头文件**:
```c
#include "simt_api/math_functions.h"
```

**功能说明**:
判断浮点数是否为有限数（非 inf、非 NaN）。

**参数**:
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| x | 输入 | 源操作数（float 类型） |

**返回值**:
- false：输入为 NaN、inf、-inf
- true：输入为有限数

**产品支持**:
| 产品 | 支持情况 |
|------|----------|
| Ascend 950PR/Ascend 950DT | √ |
| Atlas A3 训练/推理系列产品 | × |
| Atlas A2 训练/推理系列产品 | × |

**调用示例**:
```c
__simt_vf__ __launch_bounds__(1024) inline void KernelIsFinite(__gm__ bool* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = isfinite(x[idx]);
}
```

**差异说明**:
- 函数名不同：libdevice 使用 `finitef`，AscendC 使用 `isfinite`
- 返回值类型为 `bool` 而非 `int`

---

## 7. isinf - Check if infinity

### Libdevice 原型
```c
int isinf(double x);
int isinf(float x);
```

### AscendC 对应实现
**函数名**: `isinf`

**函数原型**:
```c
__simt_callee__ inline bool isinf(float x)
```

**头文件**:
```c
#include "simt_api/math_functions.h"
```

**功能说明**:
判断浮点数是否为无穷（inf 或 -inf）。

**参数**:
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| x | 输入 | 源操作数（float 类型） |

**返回值**:
- false：输入不是无穷
- true：输入为 inf 或 -inf

**产品支持**:
| 产品 | 支持情况 |
|------|----------|
| Ascend 950PR/Ascend 950DT | √ |
| Atlas A3 训练/推理系列产品 | × |
| Atlas A2 训练/推理系列产品 | × |

**调用示例**:
```c
__simt_vf__ __launch_bounds__(1024) inline void KernelIsInf(__gm__ bool* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = isinf(x[idx]);
}
```

**差异说明**:
- 返回值类型为 `bool` 而非 `int`
- 仅支持 float 类型输入

---

## 8. nextafter - Next representable float

### Libdevice 原型
```c
double nextafter(double x, double y);
float nextafterf(float x, float y);
```

### AscendC 对应实现
**函数名**: `nextafterf`

**函数原型**:
```c
__simt_callee__ inline float nextafterf(float x, float y)
```

**头文件**:
```c
#include "simt_api/math_functions.h"
```

**功能说明**:
对于两个数据 x、y：
- 如果 y > x，返回比 x 大的下一个可表示的浮点值（浮点数二进制最低位加 1）
- 如果 y < x，返回比 x 小的下一个可表示的浮点值（浮点数二进制最低位减 1）
- 如果 y 等于 x，返回 x

**参数**:
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| x | 输入 | 源操作数（起始值） |
| y | 输入 | 源操作数（目标方向） |

**返回值**:
- x 不等于 y 时，返回 y 方向上 x 之后下一个可表示的浮点值
- x 等于 y 时，返回 x
- 若 x = +inf，y 不为 NaN，返回 3.4028235e+38（float 最大值）
- 若 x = -inf，y 不为 NaN，返回 -3.4028235e+38
- 若 x、y 任意一个为 NaN，返回 NaN

**产品支持**:
| 产品 | 支持情况 |
|------|----------|
| Ascend 950PR/Ascend 950DT | √ |
| Atlas A3 训练/推理系列产品 | × |
| Atlas A2 训练/推理系列产品 | × |

**调用示例**:
```c
__simt_vf__ __launch_bounds__(1024) inline void KernelNextAfter(__gm__ float* dst, __gm__ float* x, __gm__ float* y)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = nextafterf(x[idx], y[idx]);
}
```

---

## 9. sin - Sine function

### Libdevice 原型
```c
double sin(double x);
float sinf(float x);
```

### AscendC 对应实现
**函数名**: `sinf`

**函数原型**:
```c
__simt_callee__ inline float sinf(float x)
```

**头文件**:
```c
#include "simt_api/math_functions.h"
```

**功能说明**:
获取输入数据的三角函数正弦值。

**参数**:
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| x | 输入 | 源操作数（float 类型，弧度制） |

**返回值**:
- 输入数据的三角函数正弦值
- 当 x 为 inf 时，返回值为 NaN
- 当 x 为 -inf 时，返回值为 NaN
- 当 x 为 NaN 时，返回值为 NaN

**约束说明**:
- 使用本接口时，线程配置最大不超过 1024，否则有栈溢出风险

**产品支持**:
| 产品 | 支持情况 |
|------|----------|
| Ascend 950PR/Ascend 950DT | √ |
| Atlas A3 训练/推理系列产品 | × |
| Atlas A2 训练/推理系列产品 | × |

**调用示例**:
```c
__simt_vf__ __launch_bounds__(1024) inline void KernelSin(__gm__ float* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = sinf(x[idx]);
}
```

**相关 API**:
- `sincosf`: 同时计算正弦和余弦值，性能更优

---

## 10. cos - Cosine function

### Libdevice 原型
```c
double cos(double x);
float cosf(float x);
```

### AscendC 对应实现
**函数名**: `cosf`

**函数原型**:
```c
__simt_callee__ inline float cosf(float x)
```

**头文件**:
```c
#include "simt_api/math_functions.h"
```

**功能说明**:
获取输入数据的三角函数余弦值。

**参数**:
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| x | 输入 | 源操作数（float 类型，弧度制） |

**返回值**:
- 输入数据的三角函数余弦值
- 当 x 为 inf 时，返回值为 NaN
- 当 x 为 -inf 时，返回值为 NaN
- 当 x 为 NaN 时，返回值为 NaN

**约束说明**:
- 使用本接口时，线程配置最大不超过 1024，否则有栈溢出风险

**产品支持**:
| 产品 | 支持情况 |
|------|----------|
| Ascend 950PR/Ascend 950DT | √ |
| Atlas A3 训练/推理系列产品 | × |
| Atlas A2 训练/推理系列产品 | × |

**调用示例**:
```c
__simt_vf__ __launch_bounds__(1024) inline void KernelCos(__gm__ float* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = cosf(x[idx]);
}
```

**相关 API**:
- `sincosf`: 同时计算正弦和余弦值，性能更优

---

## 总结表

| # | Libdevice 函数 | AscendC 对应函数 | 支持状态 | 注意事项 |
|---|----------------|------------------|----------|----------|
| 1 | llrint/llrintf | `llrintf` | 完全支持 | 仅 float 版本 |
| 2 | nearbyint/nearbyintf | `nearbyintf` | 完全支持 | 仅 float 版本 |
| 3 | isnan | `isnan` | 完全支持 | 返回 bool 类型 |
| 4 | signbit | 无直接对应 | 需自行实现 | 可用位运算实现 |
| 5 | copysign/copysignf | `copysignf` | 完全支持 | 仅 float 版本 |
| 6 | finite/finitef | `isfinite` | 完全支持 | 函数名不同 |
| 7 | isinf | `isinf` | 完全支持 | 返回 bool 类型 |
| 8 | nextafter/nextafterf | `nextafterf` | 完全支持 | 仅 float 版本 |
| 9 | sin/sinf | `sinf` | 完全支持 | 仅 float 版本 |
| 10 | cos/cosf | `cosf` | 完全支持 | 仅 float 版本 |

## 通用限制

1. **硬件兼容性**: 所有上述 API 仅支持 Ascend 950PR/Ascend 950DT，不支持 Atlas A2/A3 系列
2. **数据类型**: 所有 API 仅提供 float 版本，double 类型需要类型转换
3. **返回值类型**: 布尔判断类函数（isnan, isinf, isfinite）返回 `bool` 而非 `int`
4. **线程限制**: sinf/cosf/sincosf 等三角函数线程配置最大不超过 1024

## 推荐的 signbit 实现

由于 AscendC 没有提供 signbit 函数，以下是推荐的实现方式：

```c
#include <cstdint>

// 方法 1: 使用位运算（推荐，无分支）
inline bool signbit_float(float x) {
    union { float f; uint32_t u; } val = {x};
    return (val.u >> 31) != 0;
}

// 方法 2: 使用 copysignf
inline bool signbit_copysign(float x) {
    return copysignf(1.0f, x) < 0.0f;
}

// 方法 3: 区分 +/-0 的完整实现
inline bool signbit_full(float x) {
    if (x != 0.0f) {
        return x < 0.0f;
    }
    // x 是 +0 或 -0，需要特殊处理
    union { float f; uint32_t u; } val = {x};
    return (val.u >> 31) != 0;
}
```
