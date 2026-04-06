# Libdevice 函数到 AscendC API 的对应分析 (Batch 20)

**分析日期**: 2026-03-31
**分析范围**: tgamma, round, llround, fdim, ilogb, logb, isfinite (isfinited)

---

## 1. tgamma - Gamma 函数

### libdevice 定义
```c
double tgamma(double x);  // Gamma函数
float tgammaf(float x);
```

### AscendC 对应实现
| 属性 | 说明 |
|------|------|
| **函数名** | `tgammaf` |
| **函数原型** | `__simt_callee__ inline float tgammaf(float x)` |
| **头文件** | `#include "simt_api/math_functions.h"` |
| **功能** | 计算输入数据x的伽马函数值 Γ(x) |

### 参数说明
| 参数 | 输入/输出 | 描述 |
|------|----------|------|
| x | 输入 | 源操作数 |

### 返回值
- 输入数据的伽马函数值
- x为+0.0时，返回inf
- x为-0.0时，返回-inf
- x为inf时，返回inf
- x为-inf时，返回nan
- x为nan时，返回nan
- 若返回值超出float最大范围，返回inf
- 若返回值超出float最小范围，返回-inf

### 硬件兼容性
| 产品 | 支持情况 |
|------|----------|
| Ascend 950PR/Ascend 950DT | √ 支持 |
| Atlas A3 训练/推理系列产品 | × 不支持 |
| Atlas A2 训练/推理系列产品 | × 不支持 |

### 使用示例
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelTgamma(__gm__ float* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = tgammaf(x[idx]);
}
```

### 重要限制
- **仅支持float类型**，没有直接的double版本
- 如需处理double，需要手动转换为float或自行实现

---

## 2. round - 四舍五入到最近整数

### libdevice 定义
```c
double round(double x);
float roundf(float x);
```

### AscendC 对应实现
| 属性 | 说明 |
|------|------|
| **函数名** | `roundf` |
| **函数原型** | `__simt_callee__ inline float roundf(float x)` |
| **头文件** | `#include "simt_api/math_functions.h"` |
| **功能** | 获取对输入数据四舍五入后的整数 |

### 参数说明
| 参数 | 输入/输出 | 描述 |
|------|----------|------|
| x | 输入 | 源操作数 |

### 返回值
- 对输入四舍五入后的整数
- x为nan时，返回nan
- x为inf时，返回inf
- x为-inf时，返回-inf

### 硬件兼容性
| 产品 | 支持情况 |
|------|----------|
| Ascend 950PR/Ascend 950DT | √ 支持 |
| Atlas A3 训练/推理系列产品 | × 不支持 |
| Atlas A2 训练/推理系列产品 | × 不支持 |

### 使用示例
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelRound(__gm__ float* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = roundf(x[idx]);
}
```

### 重要限制
- **仅支持float类型**，返回float而非整数类型
- 如需转换为整数类型，需要额外进行类型转换

---

## 3. llround - 四舍五入到 long long

### libdevice 定义
```c
long long llround(double x);
long long llroundf(float x);
```

### AscendC 对应实现
| 属性 | 说明 |
|------|------|
| **函数名** | `llroundf` |
| **函数原型** | `__simt_callee__ inline long long int llroundf(float x)` |
| **头文件** | `#include "simt_api/math_functions.h"` |
| **功能** | 获取对输入数据四舍五入后的long long整数 |

### 参数说明
| 参数 | 输入/输出 | 描述 |
|------|----------|------|
| x | 输入 | 源操作数 |

### 返回值
- 对输入四舍五入后的long long整数
- x为nan时，返回值为0

### 硬件兼容性
| 产品 | 支持情况 |
|------|----------|
| Ascend 950PR/Ascend 950DT | √ 支持 |
| Atlas A3 训练/推理系列产品 | × 不支持 |
| Atlas A2 训练/推理系列产品 | × 不支持 |

### 使用示例
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelRound(__gm__ long long int* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = llroundf(x[idx]);
}
```

### 相关函数
- `lroundf`: 返回 `long int` 类型

### 重要限制
- **仅支持float输入**，没有double输入版本
- 当输入为nan时，返回0（与标准库行为一致）

---

## 4. fdim - 正差值 (Positive Difference)

### libdevice 定义
```c
double fdim(double x, double y);  // max(x-y, 0)
float fdimf(float x, float y);
```

### AscendC 对应实现
| 属性 | 说明 |
|------|------|
| **函数名** | `fdimf` |
| **函数原型** | `__simt_callee__ inline float fdimf(float x, float y)` |
| **头文件** | `#include "simt_api/math_functions.h"` |
| **功能** | 获取两个输入数据的差值，差值小于0时返回0 |
| **计算公式** | result = x > y ? x - y : 0 |

### 参数说明
| 参数 | 输入/输出 | 描述 |
|------|----------|------|
| x | 输入 | 源操作数1（被减数） |
| y | 输入 | 源操作数2（减数） |

### 返回值
- 输入数据的差值，差值小于0时返回0
- x,y任意一个为nan时，返回值为nan
- x为inf，y为-inf，返回值为inf
- x为-inf，y为inf，返回值为0
- x为inf，y为inf，返回值为0

### 硬件兼容性
| 产品 | 支持情况 |
|------|----------|
| Ascend 950PR/Ascend 950DT | √ 支持 |
| Atlas A3 训练/推理系列产品 | × 不支持 |
| Atlas A2 训练/推理系列产品 | × 不支持 |

### 使用示例
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelDim(__gm__ float* dst, __gm__ float* x, __gm__ float* y)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = fdimf(x[idx], y[idx]);
}
```

### 重要限制
- **仅支持float类型**，没有double版本
- 实现的是数学上的正差函数（positive difference）

---

## 5. ilogb - 整数指数部分 (以2为底)

### libdevice 定义
```c
int ilogb(double x);   // 提取指数部分，返回int
int ilogbf(float x);
```

### AscendC 对应实现
| 属性 | 说明 |
|------|------|
| **函数名** | `ilogbf` |
| **函数原型** | `__simt_callee__ inline int ilogbf(float x)` |
| **头文件** | `#include "simt_api/math_functions.h"` |
| **功能** | 计算以2为底输入数据的对数，并对结果向下取整，返回整数 |
| **计算公式** | result = floor(log₂(x)) |

### 参数说明
| 参数 | 输入/输出 | 描述 |
|------|----------|------|
| x | 输入 | 源操作数 |

### 返回值
- 以2为底的x的对数，并向下取整后的整数值
- x为inf时，返回值为2147483647 (INT_MAX)
- x为-inf时，返回值为2147483647 (INT_MAX)
- x为nan时，返回值为-2147483648 (INT_MIN)
- x为0时，返回值为-2147483648 (INT_MIN)

### 硬件兼容性
| 产品 | 支持情况 |
|------|----------|
| Ascend 950PR/Ascend 950DT | √ 支持 |
| Atlas A3 训练/推理系列产品 | × 不支持 |
| Atlas A2 训练/推理系列产品 | × 不支持 |

### 使用示例
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelILogb(__gm__ int* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = ilogbf(x[idx]);
}
```

### 重要限制
- **仅支持float类型**，返回int
- 用于提取浮点数的指数部分（以2为底）

---

## 6. logb - 浮点数指数部分 (以2为底)

### libdevice 定义
```c
double logb(double x);  // 提取指数部分，返回浮点数
float logbf(float x);
```

### AscendC 对应实现
| 属性 | 说明 |
|------|------|
| **函数名** | `logbf` |
| **函数原型** | `__simt_callee__ inline float logbf(float x)` |
| **头文件** | `#include "simt_api/math_functions.h"` |
| **功能** | 计算以2为底输入数据的对数，并对结果向下取整，返回浮点数 |
| **计算公式** | result = floor(log₂(x)) |

### 参数说明
| 参数 | 输入/输出 | 描述 |
|------|----------|------|
| x | 输入 | 源操作数 |

### 返回值
- 以2为底的x的对数，并向下取整后的浮点数值
- x为inf时，返回值为inf
- x为-inf时，返回值为inf
- x为nan时，返回值为nan
- x为0时，返回值为inf

### 硬件兼容性
| 产品 | 支持情况 |
|------|----------|
| Ascend 950PR/Ascend 950DT | √ 支持 |
| Atlas A3 训练/推理系列产品 | × 不支持 |
| Atlas A2 训练/推理系列产品 | × 不支持 |

### 使用示例
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelLogb(__gm__ float* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = logbf(x[idx]);
}
```

### ilogb vs logb 对比
| 特性 | ilogbf | logbf |
|------|--------|-------|
| 返回值类型 | int | float |
| x=inf | INT_MAX | inf |
| x=-inf | INT_MAX | inf |
| x=nan | INT_MIN | nan |
| x=0 | INT_MIN | inf |

---

## 7. isfinited - 检查 double 是否为有限数

### libdevice 定义
```c
int isfinite(double x);   // 检查是否为有限数
int isfinited(double x);  // double版本
```

### AscendC 对应实现

AscendC 提供了两种检查有限数的方式：

#### 方式1: SIMT API (标量版本)
| 属性 | 说明 |
|------|------|
| **函数名** | `isfinite` |
| **函数原型** | `__simt_callee__ inline bool isfinite(float x)` |
| **头文件** | `#include "simt_api/math_functions.h"` |
| **功能** | 判断浮点数是否为有限数（非inf、非nan） |

#### 方式2: AscendC 矢量 API (推荐用于算子开发)
| 属性 | 说明 |
|------|------|
| **函数名** | `IsFinite` |
| **函数原型** | `template<typename T, typename U> __aicore__ inline void IsFinite(const LocalTensor<U>& dst, const LocalTensor<T>& src, uint32_t calCount)` |
| **头文件** | `#include "kernel_operator.h"` |
| **功能** | 按元素判断输入的浮点数是否非NAN、非±INF |

### SIMT API 参数说明 (isfinite)
| 参数 | 输入/输出 | 描述 |
|------|----------|------|
| x | 输入 | 源操作数（float类型） |

### SIMT API 返回值
- false：输入为nan、inf、-inf
- true：输入为有限数

### IsFinite 矢量 API 参数说明
| 参数 | 输入/输出 | 描述 |
|------|----------|------|
| dst | 输出 | 目的操作数（LocalTensor） |
| src | 输入 | 源操作数（LocalTensor） |
| calCount | 输入 | 参与计算的元素个数 |

### IsFinite 支持的数据类型组合
| srcDtype | dstDtype |
|----------|----------|
| half | half |
| half | bool |
| float | float |
| float | bool |
| bfloat16_t | bfloat16_t |
| bfloat16_t | bool |

### 硬件兼容性
| 产品 | 支持情况 |
|------|----------|
| Ascend 950PR/Ascend 950DT | √ 支持 |
| Atlas A3 训练/推理系列产品 | × 不支持 |
| Atlas A2 训练/推理系列产品 | × 不支持 |

### SIMT 使用示例
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelIsFinite(__gm__ bool* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = isfinite(x[idx]);
}
```

### 矢量 API 使用示例
```cpp
#include "kernel_operator.h"

AscendC::LocalTensor<SrcT> xLocal = inQueueX.DeQue<SrcT>();
if constexpr (std::is_same_v<DstT, bool>) {
    AscendC::LocalTensor<int8_t> yLocal = outQueueY.AllocTensor<int8_t>();
    AscendC::Duplicate(yLocal, (int8_t)0, dataSize);
    AscendC::IsFinite(yLocal, xLocal, calCount);
    outQueueY.EnQue(yLocal);
} else {
    AscendC::LocalTensor<DstT> yLocal = outQueueY.AllocTensor<DstT>();
    AscendC::Duplicate(yLocal, (DstT)0, dataSize);
    AscendC::IsFinite(yLocal, xLocal, calCount);
    outQueueY.EnQue(yLocal);
}
inQueueX.FreeTensor(xLocal);
```

### 结果示例
```
输入的数据类型为float，输出的数据类型为bool
输入数据(src): [1.0, +inf, 3.0, 4.0, nan, 6.0, -inf, 8.0]
输出数据(dst): [true, false, true, true, false, true, false, true]
```

### 重要限制
- **SIMT版本的isfinite仅支持float类型**，没有直接的double版本
- **矢量版本的IsFinite支持half、float、bfloat16_t**
- 如需检查double类型，需要转换为float或自行实现

---

## 总结表

| libdevice 函数 | AscendC 对应函数 | 输入类型 | 输出类型 | 头文件 | 硬件支持 |
|---------------|-----------------|----------|----------|--------|----------|
| tgamma | tgammaf | float | float | simt_api/math_functions.h | Ascend 950 |
| round | roundf | float | float | simt_api/math_functions.h | Ascend 950 |
| llround | llroundf | float | long long | simt_api/math_functions.h | Ascend 950 |
| fdim | fdimf | float, float | float | simt_api/math_functions.h | Ascend 950 |
| ilogb | ilogbf | float | int | simt_api/math_functions.h | Ascend 950 |
| logb | logbf | float | float | simt_api/math_functions.h | Ascend 950 |
| isfinite/isfinited | isfinite / IsFinite | float/LocalTensor | bool/LocalTensor | simt_api/math_functions.h / kernel_operator.h | Ascend 950 |

## 重要注意事项

1. **类型限制**: 所有SIMT API函数仅支持`float`类型输入，没有直接的`double`版本
2. **硬件限制**: 这些API仅在Ascend 950PR/Ascend 950DT上支持，A2和A3系列不支持
3. **矢量 vs 标量**:
   - SIMT API (`tgammaf`, `roundf`等) 用于标量计算
   - `IsFinite` 是矢量API，用于算子开发中的张量计算
4. **头文件区别**:
   - SIMT API: `#include "simt_api/math_functions.h"`
   - 矢量 API: `#include "kernel_operator.h"`

## 迁移建议

对于需要从CUDA libdevice迁移到AscendC的开发者：

1. **double类型处理**: 需要手动将double转换为float，或自行实现double版本
2. **函数名转换**: 大多数函数只需添加`f`后缀（如`tgamma` -> `tgammaf`）
3. **调用约定**: SIMT API使用`__simt_callee__`修饰符，与CUDA的`__device__`类似
4. **返回值差异**: 注意某些函数的边界条件返回值可能与标准库略有差异
