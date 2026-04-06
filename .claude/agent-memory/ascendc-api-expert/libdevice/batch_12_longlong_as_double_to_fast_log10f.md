# Libdevice 函数 AscendC 对应实现分析 - Batch 12

分析时间: 2026-03-31
分析范围: longlong_as_double, double_as_longlong, fast_sinf, fast_cosf, fast_log2f, fast_logf, fast_expf, fast_tanf, fast_exp10f, fast_log10f

---

## 1. longlong_as_double - Long long as double (bit cast)

### libdevice 描述
将64位有符号整数(long long)的位模式重新解释为双精度浮点数(double)，不进行任何数值转换，仅进行位级别的重新解释。

### AscendC 对应实现

**状态: 无直接对应API - 需要手动实现**

AscendC SIMT API 提供了32位版本的bit cast函数，但没有直接提供64位版本的 `longlong_as_double` 或 `double_as_longlong`。

**可用的32位bit cast函数:**

| 函数名 | 功能 | 原型 |
|--------|------|------|
| `__int_as_float` | int -> float (bit cast) | `__simt_callee__ inline float __int_as_float(const int x)` |
| `__uint_as_float` | uint -> float (bit cast) | `__simt_callee__ inline float __uint_as_float(const unsigned int x)` |
| `__float_as_int` | float -> int (bit cast) | `__simt_callee__ inline int __float_as_int(const float x)` |
| `__float_as_uint` | float -> uint (bit cast) | `__simt_callee__ inline unsigned int __float_as_uint(const float x)` |

**产品支持情况 (所有bit cast函数):**
- Ascend 950PR/Ascend 950DT: 支持
- Atlas A3 训练/推理系列产品: 不支持
- Atlas A2 训练/推理系列产品: 不支持

**头文件:**
```cpp
#include "simt_api/device_functions.h"
```

**64位bit cast手动实现方案:**
```cpp
// 方案1: 使用union (推荐)
union LongLongDouble {
    long long ll;
    double d;
};

__simt_vf__ inline double longlong_as_double(long long x) {
    LongLongDouble u;
    u.ll = x;
    return u.d;
}

__simt_vf__ inline long long double_as_longlong(double x) {
    LongLongDouble u;
    u.d = x;
    return u.ll;
}

// 方案2: 使用reinterpret_cast (C++风格)
__simt_vf__ inline double longlong_as_double(long long x) {
    return *reinterpret_cast<double*>(&x);
}

__simt_vf__ inline long long double_as_longlong(double x) {
    return *reinterpret_cast<long long*>(&x);
}
```

**重要说明:**
- 64位bit cast函数在AscendC SIMT API中未直接提供
- 需要用户自行实现，使用union或reinterpret_cast
- 自行实现的函数在Ascend 950PR/Ascend 950DT上可用
- Atlas A2/A3系列不支持SIMT API

---

## 2. double_as_longlong - Double as long long (bit cast)

### libdevice 描述
将双精度浮点数(double)的位模式重新解释为64位有符号整数(long long)，不进行任何数值转换。

### AscendC 对应实现

**状态: 无直接对应API - 需要手动实现**

与 `longlong_as_double` 相同，AscendC没有提供直接的64位bit cast函数。

**手动实现方案:** 参考上述 `longlong_as_double` 的实现方案。

---

## 3. fast_sinf - Fast sine

### libdevice 描述
快速正弦函数，以精度换取速度。

### AscendC 对应实现

**对应函数:** `sinf`

**函数原型:**
```cpp
__simt_callee__ inline float sinf(float x)
```

**功能说明:**
获取输入数据的三角函数正弦值。

**产品支持情况:**
- Ascend 950PR/Ascend 950DT: 支持
- Atlas A3 训练/推理系列产品: 不支持
- Atlas A2 训练/推理系列产品: 不支持

**头文件:**
```cpp
#include "simt_api/math_functions.h"
```

**调用示例:**
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelSin(__gm__ float* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = sinf(x[idx]);
}
```

**特殊返回值:**
- x为inf时，返回nan
- x为-inf时，返回nan
- x为nan时，返回nan

**约束说明:**
- 线程配置最大不超过1024，否则有栈溢出风险

**注意:** AscendC的`sinf`是标准精度实现。如果需要更快的近似实现，需要用户自行开发近似算法（如使用泰勒级数展开或查找表）。

---

## 4. fast_cosf - Fast cosine

### libdevice 描述
快速余弦函数，以精度换取速度。

### AscendC 对应实现

**对应函数:** `cosf`

**函数原型:**
```cpp
__simt_callee__ inline float cosf(float x)
```

**功能说明:**
获取输入数据的三角函数余弦值。

**产品支持情况:**
- Ascend 950PR/Ascend 950DT: 支持
- Atlas A3 训练/推理系列产品: 不支持
- Atlas A2 训练/推理系列产品: 不支持

**头文件:**
```cpp
#include "simt_api/math_functions.h"
```

**调用示例:**
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelCos(__gm__ float* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = cosf(x[idx]);
}
```

**特殊返回值:**
- x为inf时，返回nan
- x为-inf时，返回nan
- x为nan时，返回nan

**约束说明:**
- 线程配置最大不超过1024，否则有栈溢出风险

**注意:** AscendC的`cosf`是标准精度实现。如需快速近似版本，需自行实现。

---

## 5. fast_log2f - Fast log base 2

### libdevice 描述
快速以2为底的对数函数。

### AscendC 对应实现

**对应函数:** `log2f`

**函数原型:**
```cpp
__simt_callee__ inline float log2f(float x)
```

**功能说明:**
获取以2为底，输入数据的对数。

**产品支持情况:**
- Ascend 950PR/Ascend 950DT: 支持
- Atlas A3 训练/推理系列产品: 不支持
- Atlas A2 训练/推理系列产品: 不支持

**头文件:**
```cpp
#include "simt_api/math_functions.h"
```

**调用示例:**
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelLog2(__gm__ float* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = log2f(x[idx]);
}
```

**特殊返回值:**
- x为inf时，返回inf
- x为-inf时，返回nan
- x为nan时，返回nan

**约束说明:**
- 无特殊约束

---

## 6. fast_logf - Fast natural log

### libdevice 描述
快速自然对数函数。

### AscendC 对应实现

**对应函数:** `logf`

**函数原型:**
```cpp
__simt_callee__ inline float logf(float x)
```

**功能说明:**
获取以e为底，输入数据的对数。

**产品支持情况:**
- Ascend 950PR/Ascend 950DT: 支持
- Atlas A3 训练/推理系列产品: 不支持
- Atlas A2 训练/推理系列产品: 不支持

**头文件:**
```cpp
#include "simt_api/math_functions.h"
```

**调用示例:**
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelLog(__gm__ float* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = logf(x[idx]);
}
```

**特殊返回值:**
- x小于0或x为nan时，返回nan
- x为0时，返回-inf
- x为inf时，返回inf

**约束说明:**
- 无特殊约束

---

## 7. fast_expf - Fast exponential

### libdevice 描述
快速指数函数(e^x)。

### AscendC 对应实现

**对应函数:** `expf`

**函数原型:**
```cpp
__simt_callee__ inline float expf(float x)
```

**功能说明:**
指定输入x，获取e的x次方。

**产品支持情况:**
- Ascend 950PR/Ascend 950DT: 支持
- Atlas A3 训练/推理系列产品: 不支持
- Atlas A2 训练/推理系列产品: 不支持

**头文件:**
```cpp
#include "simt_api/math_functions.h"
```

**调用示例:**
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelExp(__gm__ float* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = expf(x[idx]);
}
```

**特殊返回值:**
- x为inf时，返回inf
- x为-inf时，返回0
- x为nan时，返回nan
- 结果超出float最大范围时，返回inf

**约束说明:**
- 无特殊约束

---

## 8. fast_tanf - Fast tangent

### libdevice 描述
快速正切函数。

### AscendC 对应实现

**对应函数:** `tanf`

**函数原型:**
```cpp
__simt_callee__ inline float tanf(float x)
```

**功能说明:**
获取输入数据的三角函数正切值。

**产品支持情况:**
- Ascend 950PR/Ascend 950DT: 支持
- Atlas A3 训练/推理系列产品: 不支持
- Atlas A2 训练/推理系列产品: 不支持

**头文件:**
```cpp
#include "simt_api/math_functions.h"
```

**调用示例:**
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelTan(__gm__ float* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = tanf(x[idx]);
}
```

**特殊返回值:**
- x为inf时，返回nan
- x为-inf时，返回nan
- x为nan时，返回nan

**约束说明:**
- 线程配置最大不超过1024，否则有栈溢出风险

---

## 9. fast_exp10f - Fast exp base 10

### libdevice 描述
快速以10为底的指数函数(10^x)。

### AscendC 对应实现

**对应函数:** `exp10f`

**函数原型:**
```cpp
__simt_callee__ inline float exp10f(float x)
```

**功能说明:**
指定输入x，获取10的x次方。

**产品支持情况:**
- Ascend 950PR/Ascend 950DT: 支持
- Atlas A3 训练/推理系列产品: 不支持
- Atlas A2 训练/推理系列产品: 不支持

**头文件:**
```cpp
#include "simt_api/math_functions.h"
```

**调用示例:**
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelExp10(__gm__ float* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = exp10f(x[idx]);
}
```

**特殊返回值:**
- x为inf时，返回inf
- x为-inf时，返回0
- x为nan时，返回nan

**约束说明:**
- 无特殊约束

---

## 10. fast_log10f - Fast log base 10

### libdevice 描述
快速以10为底的对数函数。

### AscendC 对应实现

**对应函数:** `log10f`

**函数原型:**
```cpp
__simt_callee__ inline float log10f(float x)
```

**功能说明:**
获取以10为底，输入数据的对数。

**产品支持情况:**
- Ascend 950PR/Ascend 950DT: 支持
- Atlas A3 训练/推理系列产品: 不支持
- Atlas A2 训练/推理系列产品: 不支持

**头文件:**
```cpp
#include "simt_api/math_functions.h"
```

**调用示例:**
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelLog10(__gm__ float* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = log10f(x[idx]);
}
```

**特殊返回值:**
- x为inf时，返回inf
- x为-inf时，返回nan
- x为nan时，返回nan

**约束说明:**
- 无特殊约束

---

## 总结表

| libdevice函数 | AscendC对应函数 | 支持状态 | 头文件 | 产品支持 |
|--------------|----------------|---------|--------|---------|
| longlong_as_double | 需手动实现 | 间接支持 | - | Ascend 950PR/Ascend 950DT |
| double_as_longlong | 需手动实现 | 间接支持 | - | Ascend 950PR/Ascend 950DT |
| fast_sinf | sinf | 支持 | simt_api/math_functions.h | Ascend 950PR/Ascend 950DT |
| fast_cosf | cosf | 支持 | simt_api/math_functions.h | Ascend 950PR/Ascend 950DT |
| fast_log2f | log2f | 支持 | simt_api/math_functions.h | Ascend 950PR/Ascend 950DT |
| fast_logf | logf | 支持 | simt_api/math_functions.h | Ascend 950PR/Ascend 950DT |
| fast_expf | expf | 支持 | simt_api/math_functions.h | Ascend 950PR/Ascend 950DT |
| fast_tanf | tanf | 支持 | simt_api/math_functions.h | Ascend 950PR/Ascend 950DT |
| fast_exp10f | exp10f | 支持 | simt_api/math_functions.h | Ascend 950PR/Ascend 950DT |
| fast_log10f | log10f | 支持 | simt_api/math_functions.h | Ascend 950PR/Ascend 950DT |

## 重要说明

1. **SIMT API限制**: 所有上述API都属于AscendC SIMT API，仅支持Ascend 950PR/Ascend 950DT产品。Atlas A2/A3系列不支持SIMT API。

2. **Fast版本说明**: AscendC提供的数学函数是标准精度实现。如果需要"fast"版本（以精度换取速度），需要用户自行实现近似算法。

3. **64位Bit Cast**: `longlong_as_double` 和 `double_as_longlong` 需要用户使用union或reinterpret_cast自行实现。

4. **线程约束**: sinf, cosf, tanf等三角函数在使用时线程配置最大不超过1024，否则有栈溢出风险。

5. **头文件组织**:
   - 数学函数: `simt_api/math_functions.h`
   - Bit cast函数: `simt_api/device_functions.h`
