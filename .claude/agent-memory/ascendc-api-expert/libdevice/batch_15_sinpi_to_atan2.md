# Libdevice 函数到 AscendC 的映射分析 - Batch 15

## 概述

本文档分析了10个 libdevice 数学函数在 AscendC 中的对应实现方式，涵盖三角函数、指数函数、对数函数和双曲函数。

**重要兼容性提示**：本文档中所有函数均仅支持 **Ascend 950PR/Ascend 950DT** 产品，不支持 Atlas A2 和 A3 系列。

---

## 1. sinpi - Sine of pi*x

### Libdevice 函数
```cuda
float sinpif(float x);  // sin(pi * x)
```

### AscendC 对应实现
```cpp
__simt_callee__ inline float sinpif(float x)
```

### 详细说明
- **功能**：获取输入数据与π相乘的正弦值，即 sin(π * x)
- **头文件**：`#include "simt_api/math_functions.h"`
- **参数**：
  - `x`: 输入的 float 类型源操作数
- **返回值**：
  - 输入数据与π相乘的正弦值
  - 当 x*π 超出 float 最大/最小范围时，返回 nan
  - 当 x 为 inf/-inf/nan 时，返回 nan
- **约束**：
  - 线程配置最大不超过1024，否则有栈溢出风险
- **产品支持**：仅 Ascend 950PR/Ascend 950DT

### 调用示例
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelSinpi(__gm__ float* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = sinpif(x[idx]);
}
```

---

## 2. cospi - Cosine of pi*x

### Libdevice 函数
```cuda
float cospif(float x);  // cos(pi * x)
```

### AscendC 对应实现
```cpp
__simt_callee__ inline float cospif(float x)
```

### 详细说明
- **功能**：获取输入数据与π相乘的余弦值，即 cos(π * x)
- **头文件**：`#include "simt_api/math_functions.h"`
- **参数**：
  - `x`: 输入的 float 类型源操作数
- **返回值**：
  - 输入数据与π相乘的余弦值
  - 当 x*π 超出 float 最大/最小范围时，返回 nan
  - 当 x 为 inf/-inf/nan 时，返回 nan
- **约束**：
  - 线程配置最大不超过1024，否则有栈溢出风险
- **产品支持**：仅 Ascend 950PR/Ascend 950DT

### 调用示例
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelCospi(__gm__ float* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = cospif(x[idx]);
}
```

---

## 3. tan - Tangent function

### Libdevice 函数
```cuda
float tanf(float x);  // tan(x)
```

### AscendC 对应实现
```cpp
__simt_callee__ inline float tanf(float x)
```

### 详细说明
- **功能**：获取输入数据的三角函数正切值，即 tan(x)
- **头文件**：`#include "simt_api/math_functions.h"`
- **参数**：
  - `x`: 输入的 float 类型源操作数（弧度）
- **返回值**：
  - 输入数据的正切值
  - 当 x 为 inf/-inf/nan 时，返回 nan
- **约束**：
  - 线程配置最大不超过1024，否则有栈溢出风险
- **产品支持**：仅 Ascend 950PR/Ascend 950DT

### 调用示例
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelTan(__gm__ float* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = tanf(x[idx]);
}
```

---

## 4. log2 - Log base 2

### Libdevice 函数
```cuda
float log2f(float x);  // log2(x)
```

### AscendC 对应实现
```cpp
__simt_callee__ inline float log2f(float x)
```

### 详细说明
- **功能**：获取以2为底的输入数据的对数，即 log₂(x)
- **头文件**：`#include "simt_api/math_functions.h"`
- **参数**：
  - `x`: 输入的 float 类型源操作数
- **返回值**：
  - 以2为底的x的对数
  - 当 x 为 inf 时，返回 inf
  - 当 x 为 -inf 时，返回 nan
  - 当 x 为 nan 时，返回 nan
- **约束**：无
- **产品支持**：仅 Ascend 950PR/Ascend 950DT

### 调用示例
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelLog2(__gm__ float* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = log2f(x[idx]);
}
```

---

## 5. exp - Exponential function

### Libdevice 函数
```cuda
float expf(float x);  // e^x
```

### AscendC 对应实现
```cpp
__simt_callee__ inline float expf(float x)
```

### 详细说明
- **功能**：获取e的x次方，即 e^x
- **头文件**：`#include "simt_api/math_functions.h"`
- **参数**：
  - `x`: 输入的 float 类型源操作数
- **返回值**：
  - e的x次方
  - 当 x 为 inf 时，返回 inf
  - 当 x 为 -inf 时，返回 0
  - 当 x 为 nan 时，返回 nan
- **约束**：无
- **产品支持**：仅 Ascend 950PR/Ascend 950DT

### 调用示例
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelExp(__gm__ float* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = expf(x[idx]);
}
```

---

## 6. exp10 - Exp base 10

### Libdevice 函数
```cuda
float exp10f(float x);  // 10^x
```

### AscendC 对应实现
```cpp
__simt_callee__ inline float exp10f(float x)
```

### 详细说明
- **功能**：获取10的x次方，即 10^x
- **头文件**：`##include "simt_api/math_functions.h"`
- **参数**：
  - `x`: 输入的 float 类型源操作数
- **返回值**：
  - 10的x次方
  - 当 x 为 inf 时，返回 inf
  - 当 x 为 -inf 时，返回 0
  - 当 x 为 nan 时，返回 nan
- **约束**：无
- **产品支持**：仅 Ascend 950PR/Ascend 950DT

### 调用示例
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelExp10(__gm__ float* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = exp10f(x[idx]);
}
```

---

## 7. cosh - Hyperbolic cosine

### Libdevice 函数
```cuda
float coshf(float x);  // cosh(x) = (e^x + e^(-x)) / 2
```

### AscendC 对应实现
```cpp
__simt_callee__ inline float coshf(float x)
```

### 详细说明
- **功能**：获取输入数据的双曲余弦值，即 cosh(x)
- **头文件**：`#include "simt_api/math_functions.h"`
- **参数**：
  - `x`: 输入的 float 类型源操作数
- **返回值**：输入数据的双曲余弦值
- **约束**：
  - 输入数据必须在 [-89.4, 89.4] 范围内
- **产品支持**：仅 Ascend 950PR/Ascend 950DT

### 调用示例
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelCosh(__gm__ float* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = coshf(x[idx]);
}
```

---

## 8. sinh - Hyperbolic sine

### Libdevice 函数
```cuda
float sinhf(float x);  // sinh(x) = (e^x - e^(-x)) / 2
```

### AscendC 对应实现
```cpp
__simt_callee__ inline float sinhf(float x)
```

### 详细说明
- **功能**：获取输入数据的双曲正弦值，即 sinh(x)
- **头文件**：`#include "simt_api/math_functions.h"`
- **参数**：
  - `x`: 输入的 float 类型源操作数
- **返回值**：输入数据的双曲正弦值
- **约束**：
  - 输入数据必须在 [-89.4, 89.4] 范围内
- **产品支持**：仅 Ascend 950PR/Ascend 950DT

### 调用示例
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelSinh(__gm__ float* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = sinhf(x[idx]);
}
```

---

## 9. tanh - Hyperbolic tangent

### Libdevice 函数
```cuda
float tanhf(float x);  // tanh(x) = sinh(x) / cosh(x)
```

### AscendC 对应实现
```cpp
__simt_callee__ inline float tanhf(float x)
```

### 详细说明
- **功能**：获取输入数据的双曲正切值，即 tanh(x)
- **头文件**：`#include "simt_api/math_functions.h"`
- **参数**：
  - `x`: 输入的 float 类型源操作数
- **返回值**：
  - 输入数据的双曲正切值
  - 当 x 为 inf 时，返回 1.0
  - 当 x 为 -inf 时，返回 -1.0
  - 当 x 为 nan 时，返回 nan
- **约束**：无
- **产品支持**：仅 Ascend 950PR/Ascend 950DT

### 调用示例
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelTanh(__gm__ float* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = tanhf(x[idx]);
}
```

---

## 10. atan2 - Arctangent of y/x

### Libdevice 函数
```cuda
float atan2f(float y, float x);  // atan2(y, x)
```

### AscendC 对应实现
```cpp
__simt_callee__ inline float atan2f(float y, float x)
```

### 详细说明
- **功能**：获取输入数据 y/x 的反正切值，即 atan2(y, x)
- **头文件**：`#include "simt_api/math_functions.h"`
- **参数**：
  - `y`: 第一个输入的 float 类型源操作数（分子）
  - `x`: 第二个输入的 float 类型源操作数（分母）
- **返回值**：
  - y/x 的反正切值
  - 当 x, y 任意为 nan 时，返回 nan
  - 当 x=0, y=0 时，返回 nan
  - 当 y=inf, x=inf 时，返回 π/4
  - 当 y=-inf, x=inf 时，返回 -π/4
  - 当 y=1, x=inf 时，返回 0.0
  - 当 y=inf, x=-inf 时，返回 3π/4
  - 当 y=-inf, x=-inf 时，返回 -3π/4
  - 当 y=1, x=-inf 时，返回 π
  - 当 y=inf, x=1 时，返回 π/2
  - 当 y=-inf, x=1 时，返回 -π/2
- **约束**：无
- **产品支持**：仅 Ascend 950PR/Ascend 950DT

### 调用示例
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelAtan2(__gm__ float* dst, __gm__ float* y, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = atan2f(y[idx], x[idx]);
}
```

---

## 汇总表

| 序号 | Libdevice 函数 | AscendC 函数 | 功能描述 | 特殊约束 |
|------|----------------|--------------|----------|----------|
| 1 | sinpif | sinpif | sin(π * x) | 线程数 ≤ 1024 |
| 2 | cospif | cospif | cos(π * x) | 线程数 ≤ 1024 |
| 3 | tanf | tanf | tan(x) | 线程数 ≤ 1024 |
| 4 | log2f | log2f | log₂(x) | 无 |
| 5 | expf | expf | e^x | 无 |
| 6 | exp10f | exp10f | 10^x | 无 |
| 7 | coshf | coshf | cosh(x) | 输入范围 [-89.4, 89.4] |
| 8 | sinhf | sinhf | sinh(x) | 输入范围 [-89.4, 89.4] |
| 9 | tanhf | tanhf | tanh(x) | 无 |
| 10 | atan2f | atan2f | atan2(y, x) | 无 |

## 通用说明

### 头文件
所有函数都需要包含：
```cpp
#include "simt_api/math_functions.h"
```

### 函数修饰符
所有函数都使用 `__simt_callee__` 修饰符，表示这是 SIMT 模式的被调用函数。

### 产品兼容性
**重要**：本文档中所有函数仅支持 Ascend 950PR/Ascend 950DT 产品。
- Atlas A2 训练/推理系列产品：不支持
- Atlas A3 训练/推理系列产品：不支持

### 数据类型
所有函数目前仅支持 `float` 类型输入和输出。

---

## 参考文档

- `/gemini/code/huawei/asc-devkit/docs/api/context/sinpif.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/cospif.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/tanf.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/log2f.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/expf.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/exp10f.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/coshf.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/sinhf.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/tanhf.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/atan2f.md`
