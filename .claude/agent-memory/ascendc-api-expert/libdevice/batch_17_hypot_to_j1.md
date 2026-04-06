# Batch 17: libdevice 函数 AscendC 对应实现分析

## 概述

本文档分析 10 个 libdevice 数学函数在 AscendC 中的对应实现：
1. hypot - Hypotenuse (sqrt(x²+y²))
2. rhypot - Reciprocal hypotenuse
3. norm3d - 3D vector norm
4. rnorm3d - Reciprocal 3D norm
5. norm4d - 4D vector norm
6. rnorm4d - Reciprocal 4D norm
7. cbrt - Cube root
8. rcbrt - Reciprocal cube root
9. j0 - Bessel function of first kind (order 0)
10. j1 - Bessel function of first kind (order 1)

---

## 1. hypot / hypotf

### libdevice 函数
- `hypot(double x, double y)` - 双精度
- `hypotf(float x, float y)` - 单精度

### AscendC 对应实现

| 属性 | 说明 |
|------|------|
| **函数名** | `hypotf` |
| **函数原型** | `__simt_callee__ inline float hypotf(float x, float y)` |
| **头文件** | `#include "simt_api/math_functions.h"` |
| **计算** | sqrt(x² + y²) |

### 产品支持情况
- ✅ Ascend 950PR/Ascend 950DT
- ❌ Atlas A3 训练/推理系列产品
- ❌ Atlas A2 训练/推理系列产品

### 返回值说明
- 若 x² + y² 的平方根超出 float 最大范围，返回值为 inf
- 若 x, y 任意一个为 ±inf，返回值为 inf
- 若 x, y 任意一个为 nan，另一个不为 ±inf，返回值为 nan

### 调用示例
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelHypot(__gm__ float* dst, __gm__ float* x, __gm__ float* y)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = hypotf(x[idx], y[idx]);
}
```

---

## 2. rhypot / rhypotf

### libdevice 函数
- `rhypot(double x, double y)` - 双精度
- `rhypotf(float x, float y)` - 单精度

### AscendC 对应实现

| 属性 | 说明 |
|------|------|
| **函数名** | `rhypotf` |
| **函数原型** | `__simt_callee__ inline float rhypotf(float x, float y)` |
| **头文件** | `#include "simt_api/math_functions.h"` |
| **计算** | 1 / sqrt(x² + y²) |

### 产品支持情况
- ✅ Ascend 950PR/Ascend 950DT
- ❌ Atlas A3 训练/推理系列产品
- ❌ Atlas A2 训练/推理系列产品

### 返回值说明
- 若 x² + y² 的平方根超出 float 最大范围，返回值为 0
- 若 x² + y² 平方根的倒数超出 float 最大范围，返回值为 inf
- 若 x 和 y 都为 0，返回值为 inf
- 若 x, y 任意一个为 ±inf，返回值为 0
- 若 x, y 任意一个为 nan，另一个不为 ±inf，返回值为 nan

### 调用示例
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelPow(__gm__ float* dst, __gm__ float* x, __gm__ float* y)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = rhypotf(x[idx], y[idx]);
}
```

---

## 3. norm3d / norm3df

### libdevice 函数
- `norm3d(double a, double b, double c)` - 双精度
- `norm3df(float a, float b, float c)` - 单精度

### AscendC 对应实现

| 属性 | 说明 |
|------|------|
| **函数名** | `norm3df` |
| **函数原型** | `__simt_callee__ inline float norm3df(float a, float b, float c)` |
| **头文件** | `#include "simt_api/math_functions.h"` |
| **计算** | sqrt(a² + b² + c²) |

### 产品支持情况
- ✅ Ascend 950PR/Ascend 950DT
- ❌ Atlas A3 训练/推理系列产品
- ❌ Atlas A2 训练/推理系列产品

### 返回值说明
- 当 a² + b² + c² 的平方根超出 float 最大范围，返回值为 inf
- 若 a, b, c 任意一个或多个为 ±inf，返回值为 inf
- 若 a, b, c 任意一个或多个为 nan 同时不是 ±inf，返回值为 nan

### 调用示例
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelNorm3d(__gm__ float* dst, __gm__ float* a, __gm__ float* b, __gm__ float* c)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = norm3df(a[idx], b[idx], c[idx]);
}
```

---

## 4. rnorm3d / rnorm3df

### libdevice 函数
- `rnorm3d(double a, double b, double c)` - 双精度
- `rnorm3df(float a, float b, float c)` - 单精度

### AscendC 对应实现

| 属性 | 说明 |
|------|------|
| **函数名** | `rnorm3df` |
| **函数原型** | `__simt_callee__ inline float rnorm3df(float a, float b, float c)` |
| **头文件** | `#include "simt_api/math_functions.h"` |
| **计算** | 1 / sqrt(a² + b² + c²) |

### 产品支持情况
- ✅ Ascend 950PR/Ascend 950DT
- ❌ Atlas A3 训练/推理系列产品
- ❌ Atlas A2 训练/推理系列产品

### 返回值说明
- 若 a² + b² + c² 的平方根超出 float 最大范围，返回值为 0
- 若 a² + b² + c² 平方根的倒数超出 float 最大范围，返回值为 inf
- 若 a, b, c 都为 0，返回值为 inf
- 若 a, b, c 任意一个或多个为 ±inf，返回值为 0
- 若 a, b, c 任意一个或多个为 nan 同时不是 ±inf，返回值为 nan

### 调用示例
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelRnorm3d(__gm__ float* dst, __gm__ float* a, __gm__ float* b, __gm__ float* c)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = rnorm3df(a[idx], b[idx], c[idx]);
}
```

---

## 5. norm4d / norm4df

### libdevice 函数
- `norm4d(double a, double b, double c, double d)` - 双精度
- `norm4df(float a, float b, float c, float d)` - 单精度

### AscendC 对应实现

| 属性 | 说明 |
|------|------|
| **函数名** | `norm4df` |
| **函数原型** | `__simt_callee__ inline float norm4df(float a, float b, float c, float d)` |
| **头文件** | `#include "simt_api/math_functions.h"` |
| **计算** | sqrt(a² + b² + c² + d²) |

### 产品支持情况
- ✅ Ascend 950PR/Ascend 950DT
- ❌ Atlas A3 训练/推理系列产品
- ❌ Atlas A2 训练/推理系列产品

### 返回值说明
- 若 a² + b² + c² + d² 的平方根超出 float 最大范围，返回值为 inf
- 若 a, b, c, d 任意一个或多个为 ±inf，返回值为 inf
- 若 a, b, c, d 任意一个或多个为 nan 同时不是 ±inf，返回值为 nan

### 调用示例
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelNorm4d(__gm__ float* dst, __gm__ float* a, __gm__ float* b, __gm__ float* c, __gm__ float* d)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = norm4df(a[idx], b[idx], c[idx], d[idx]);
}
```

---

## 6. rnorm4d / rnorm4df

### libdevice 函数
- `rnorm4d(double a, double b, double c, double d)` - 双精度
- `rnorm4df(float a, float b, float c, float d)` - 单精度

### AscendC 对应实现

| 属性 | 说明 |
|------|------|
| **函数名** | `rnorm4df` |
| **函数原型** | `__simt_callee__ inline float rnorm4df(float a, float b, float c, float d)` |
| **头文件** | `#include "simt_api/math_functions.h"` |
| **计算** | 1 / sqrt(a² + b² + c² + d²) |

### 产品支持情况
- ✅ Ascend 950PR/Ascend 950DT
- ❌ Atlas A3 训练/推理系列产品
- ❌ Atlas A2 训练/推理系列产品

### 返回值说明
- 若 a² + b² + c² + d² 的平方根超出 float 最大范围，返回值为 0
- 若 a² + b² + c² + d² 平方根的倒数超出 float 最大范围，返回值为 inf
- 若 a, b, c, d 都为 0，返回值为 inf
- 若 a, b, c, d 任意一个或多个为 ±inf，返回值为 0
- 若 a, b, c, d 任意一个或多个为 nan 同时不是 ±inf，返回值为 nan

### 调用示例
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelRnorm4d(__gm__ float* dst, __gm__ float* a, __gm__ float* b, __gm__ float* c, __gm__ float* d)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = rnorm4df(a[idx], b[idx], c[idx], d[idx]);
}
```

---

## 7. cbrt / cbrtf

### libdevice 函数
- `cbrt(double x)` - 双精度
- `cbrtf(float x)` - 单精度

### AscendC 对应实现

| 属性 | 说明 |
|------|------|
| **函数名** | `cbrtf` |
| **函数原型** | `__simt_callee__ inline float cbrtf(float x)` |
| **头文件** | `#include "simt_api/math_functions.h"` |
| **计算** | ³√x (x 的立方根) |

### 产品支持情况
- ✅ Ascend 950PR/Ascend 950DT
- ❌ Atlas A3 训练/推理系列产品
- ❌ Atlas A2 训练/推理系列产品

### 返回值说明
- 当 x 为 0 时，返回值为 0
- 当 x 为 nan 时，返回值为 nan
- 当 x 为 inf 时，返回值为 inf
- 当 x 为 -inf 时，返回值为 -inf

### 调用示例
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelCbrt(__gm__ float* x, __gm__ float* y)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    y[idx] = cbrtf(x[idx]);
}
```

---

## 8. rcbrt / rcbrtf

### libdevice 函数
- `rcbrt(double x)` - 双精度
- `rcbrtf(float x)` - 单精度

### AscendC 对应实现

| 属性 | 说明 |
|------|------|
| **函数名** | `rcbrtf` |
| **函数原型** | `__simt_callee__ inline float rcbrtf(float x)` |
| **头文件** | `#include "simt_api/math_functions.h"` |
| **计算** | 1 / ³√x (x 的立方根的倒数) |

### 产品支持情况
- ✅ Ascend 950PR/Ascend 950DT
- ❌ Atlas A3 训练/推理系列产品
- ❌ Atlas A2 训练/推理系列产品

### 返回值说明
- 当 x 为 0 时，返回值为 inf
- 当 x 为 nan 时，返回值为 nan
- 当 x 为 inf 时，返回值为 0
- 当 x 为 -inf 时，返回值为 0

### 调用示例
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelRcbrt(__gm__ float* x, __gm__ float* y)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    y[idx] = rcbrtf(x[idx]);
}
```

---

## 9. j0 / j0f

### libdevice 函数
- `j0(double x)` - 双精度
- `j0f(float x)` - 单精度

### AscendC 对应实现

| 属性 | 说明 |
|------|------|
| **函数名** | `j0f` |
| **函数原型** | `__simt_callee__ inline float j0f(float x)` |
| **头文件** | `#include "simt_api/math_functions.h"` |
| **计算** | J₀(x) - 0 阶第一类贝塞尔函数 |

### 产品支持情况
- ✅ Ascend 950PR/Ascend 950DT
- ❌ Atlas A3 训练/推理系列产品
- ❌ Atlas A2 训练/推理系列产品

### 返回值说明
- 当 x = inf 或 -inf 时，返回值为 0
- 当 x = nan 时，返回值为 nan

### 约束说明
⚠️ **重要**: 使用本接口时，配置的线程数不应超过 256，否则有栈溢出风险。

### 调用示例
```cpp
__simt_vf__ __launch_bounds__(256) inline void KernelJ0(__gm__ float* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = j0f(x[idx]);
}
```

---

## 10. j1 / j1f

### libdevice 函数
- `j1(double x)` - 双精度
- `j1f(float x)` - 单精度

### AscendC 对应实现

| 属性 | 说明 |
|------|------|
| **函数名** | `j1f` |
| **函数原型** | `__simt_callee__ inline float j1f(float x)` |
| **头文件** | `#include "simt_api/math_functions.h"` |
| **计算** | J₁(x) - 1 阶第一类贝塞尔函数 |

### 产品支持情况
- ✅ Ascend 950PR/Ascend 950DT
- ❌ Atlas A3 训练/推理系列产品
- ❌ Atlas A2 训练/推理系列产品

### 返回值说明
- 当 x = 0，返回值为 0
- 当 x = inf 或 -inf，返回值为 0
- 当 x = nan，返回值为 nan

### 约束说明
⚠️ **重要**: 使用本接口时，配置的线程数不应超过 256，否则有栈溢出风险。

### 调用示例
```cpp
__simt_vf__ __launch_bounds__(256) inline void KernelJ1(__gm__ float* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = j1f(x[idx]);
}
```

---

## 总结

### 函数映射表

| libdevice 函数 | AscendC 函数 | 参数数量 | 特殊约束 |
|---------------|--------------|----------|----------|
| hypot/hypotf | hypotf | 2 (x, y) | 无 |
| rhypot/rhypotf | rhypotf | 2 (x, y) | 无 |
| norm3d/norm3df | norm3df | 3 (a, b, c) | 无 |
| rnorm3d/rnorm3df | rnorm3df | 3 (a, b, c) | 无 |
| norm4d/norm4df | norm4df | 4 (a, b, c, d) | 无 |
| rnorm4d/rnorm4df | rnorm4df | 4 (a, b, c, d) | 无 |
| cbrt/cbrtf | cbrtf | 1 (x) | 无 |
| rcbrt/rcbrtf | rcbrtf | 1 (x) | 无 |
| j0/j0f | j0f | 1 (x) | 线程数 ≤ 256 |
| j1/j1f | j1f | 1 (x) | 线程数 ≤ 256 |

### 硬件兼容性警告

⚠️ **所有这 10 个函数仅支持 Ascend 950PR/Ascend 950DT 平台**。

- 不支持 Atlas A2 训练/推理系列产品
- 不支持 Atlas A3 训练/推理系列产品

在使用这些 API 时，必须确保目标硬件是 Ascend 950PR/Ascend 950DT，否则会导致编译或运行时错误。

### 头文件

所有函数都需要包含：
```cpp
#include "simt_api/math_functions.h"
```

### 实现说明

1. **SIMT 模式**: 所有这些函数都是 SIMT（Single Instruction Multiple Threads）模式下的设备函数
2. **函数修饰符**: 使用 `__simt_callee__` 修饰符
3. **内核修饰符**: 内核函数使用 `__simt_vf__` 和 `__launch_bounds__`
4. **精度**: 目前只找到单精度 (float) 版本，双精度版本可能需要通过组合其他 API 实现

### 相关 API 文档位置

- `/gemini/code/huawei/asc-devkit/docs/api/context/hypotf.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/rhypotf.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/norm3df.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/rnorm3df.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/norm4df.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/rnorm4df.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/cbrtf.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/rcbrtf.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/j0f.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/j1f.md`
