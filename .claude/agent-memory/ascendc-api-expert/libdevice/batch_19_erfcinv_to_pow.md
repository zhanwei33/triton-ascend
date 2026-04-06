# Libdevice 函数 AscendC 对应实现分析 (Batch 19)

分析 10 个 libdevice 函数在 AscendC 中的对应实现方式。

**分析日期**: 2026-03-31
**API 文档路径**: `/gemini/code/huawei/asc-devkit/docs/api/context/`

---

## 1. erfcinv - Inverse complementary error function

### Libdevice 函数
`erfcinv(x)` - 逆互补误差函数

### AscendC 对应实现
**函数名**: `erfcinvf`

**函数原型**:
```cpp
__simt_callee__ inline float erfcinvf(float x)
```

**功能说明**:
获取输入数据的逆互补误差函数值。

**参数**:
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| x | 输入 | 源操作数 |

**返回值**:
- 当 x 为 0 时，返回值为 inf
- 当 x 为 nan 时，返回值为 nan
- 当 x 为 2 时，返回值为 -inf
- 当 x ∉ [0,2] 时，返回 nan

**头文件**:
```cpp
#include "simt_api/math_functions.h"
```

**使用示例**:
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelErfc(__gm__ float* x, __gm__ float* y)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    y[idx] = erfcinvf(x[idx]);
}
```

**产品支持**:
- Ascend 950PR/Ascend 950DT: 支持
- Atlas A3 训练/推理系列产品: 不支持
- Atlas A2 训练/推理系列产品: 不支持

---

## 2. normcdfinv - Inverse standard normal CDF

### Libdevice 函数
`normcdfinv(x)` - 标准正态累积分布的逆函数

### AscendC 对应实现
**函数名**: `normcdfinvf`

**函数原型**:
```cpp
__simt_callee__ inline float normcdfinvf(float x)
```

**功能说明**:
获取输入数据 x 的标准正态累积分布的逆函数。

**参数**:
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| x | 输入 | 源操作数 |

**返回值**:
- 当 x 为 0 时，返回值为 -inf
- 当 x 为 nan 时，返回值为 nan
- 当 x 为 1 时，返回值为 inf
- 当 x ∉ [0,1] 时，返回 nan

**头文件**:
```cpp
#include "simt_api/math_functions.h"
```

**使用示例**:
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelNormcdfinv(__gm__ float* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = normcdfinvf(x[idx]);
}
```

**产品支持**:
- Ascend 950PR/Ascend 950DT: 支持
- Atlas A3 训练/推理系列产品: 不支持
- Atlas A2 训练/推理系列产品: 不支持

---

## 3. normcdf - Standard normal CDF

### Libdevice 函数
`normcdf(x)` - 标准正态累积分布函数

### AscendC 对应实现
**函数名**: `normcdff`

**函数原型**:
```cpp
__simt_callee__ inline float normcdff(float x)
```

**功能说明**:
获取输入数据 x 的标准正态分布的累积分布函数值。

**参数**:
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| x | 输入 | 源操作数 |

**返回值**:
- 当 x 为 -inf，返回值为 0
- 当 x 为 inf，返回值为 1
- 当 x 为 nan，返回值为 nan

**头文件**:
```cpp
#include "simt_api/math_functions.h"
```

**使用示例**:
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelNormcdf(__gm__ float* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = normcdff(x[idx]);
}
```

**产品支持**:
- Ascend 950PR/Ascend 950DT: 支持
- Atlas A3 训练/推理系列产品: 不支持
- Atlas A2 训练/推理系列产品: 不支持

---

## 4. lgamma - Log gamma function

### Libdevice 函数
`lgamma(x)` - 对数伽马函数

### AscendC 对应实现
**函数名**: `lgammaf`

**函数原型**:
```cpp
__simt_callee__ inline float lgammaf(float x)
```

**功能说明**:
获取输入数据 x 伽马值的绝对值并求自然对数。公式: ln(|Γ(x)|)

**参数**:
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| x | 输入 | 源操作数 |

**返回值**:
- 当 x 为 0 时，返回值为 inf
- 当 x 为 inf 时，返回值为 inf
- 当 x 为 -inf 时，返回值为 inf
- 当 x 为 nan 时，返回值为 nan
- 若返回值超出 float 最大范围，返回值为 inf
- 若返回值超出 float 最小范围，返回值为 -inf

**头文件**:
```cpp
#include "simt_api/math_functions.h"
```

**使用示例**:
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelLgamma(__gm__ float* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = lgammaf(x[idx]);
}
```

**产品支持**:
- Ascend 950PR/Ascend 950DT: 支持
- Atlas A3 训练/推理系列产品: 不支持
- Atlas A2 训练/推理系列产品: 不支持

---

## 5. ldexp - Load exponent (x * 2^exp)

### Libdevice 函数
`ldexp(x, exp)` - 加载指数

### AscendC 对应实现
**函数名**: `ldexpf`

**函数原型**:
```cpp
__simt_callee__ inline float ldexpf(float x, int exp)
```

**功能说明**:
获取输入 x 乘以 2 的 exp 次幂的结果。公式: x * 2^exp

**参数**:
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| x | 输入 | 源操作数，输入数据 |
| exp | 输入 | 源操作数，指数 |

**返回值**:
- x * 2^exp
- 当 x 为 nan 时，返回值为 nan
- 若 x * 2^exp 超出 float 最大范围，返回值为 inf
- 若 x * 2^exp 超出 float 最小范围，返回值为 -inf

**头文件**:
```cpp
#include "simt_api/math_functions.h"
```

**使用示例**:
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelLdexp(__gm__ float* dst, __gm__ float* x, __gm__ int* exp)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = ldexpf(x[idx], exp[idx]);
}
```

**产品支持**:
- Ascend 950PR/Ascend 950DT: 支持
- Atlas A3 训练/推理系列产品: 不支持
- Atlas A2 训练/推理系列产品: 不支持

---

## 6. scalbn - Scale by power of 2

### Libdevice 函数
`scalbn(x, n)` - 按 2 的幂次缩放

### AscendC 对应实现
**函数名**: `scalbnf`

**函数原型**:
```cpp
__simt_callee__ inline float scalbnf(float x, int32_t n)
```

**功能说明**:
获取输入数据 x 与 2 的 n 次方的乘积。公式: x * 2^n

**参数**:
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| x | 输入 | 源操作数 |
| n | 输入 | 源操作数，指数 |

**返回值**:
- x 与 2 的 n 次方的乘积
- 当 x 为 nan 时，返回值为 nan
- 若 x * 2^n 超出 float 最大范围，返回值为 inf
- 若 x * 2^n 超出 float 最小范围，返回值为 -inf

**头文件**:
```cpp
#include "simt_api/math_functions.h"
```

**使用示例**:
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelScalbn(__gm__ float* dst, __gm__ float* x, __gm__ int* n)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = scalbnf(x[idx], n[idx]);
}
```

**产品支持**:
- Ascend 950PR/Ascend 950DT: 支持
- Atlas A3 训练/推理系列产品: 不支持
- Atlas A2 训练/推理系列产品: 不支持

**注意**: `scalbnf` 与 `ldexpf` 功能相似，都是计算 x * 2^n，但参数类型不同（scalbnf 使用 int32_t，ldexpf 使用 int）。

---

## 7. fmod - Floating point remainder

### Libdevice 函数
`fmod(x, y)` - 浮点余数

### AscendC 对应实现
**函数名**: `fmodf`

**函数原型**:
```cpp
__simt_callee__ inline float fmodf(float x, float y)
```

**功能说明**:
获取输入数据 x 除以 y 的余数。求余数时，商取 x 除以 y 浮点数结果的整数部分。

**参数**:
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| x | 输入 | 源操作数，被除数 |
| y | 输入 | 源操作数，除数 |

**返回值**:
- 输入数据 x 除以 y 的余数
- x，y 任意一个为 inf、-inf、nan 时，返回值为 nan

**头文件**:
```cpp
#include "simt_api/math_functions.h"
```

**使用示例**:
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelMod(__gm__ float* dst, __gm__ float* x, __gm__ float* y)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = fmodf(x[idx], y[idx]);
}
```

**产品支持**:
- Ascend 950PR/Ascend 950DT: 支持
- Atlas A3 训练/推理系列产品: 不支持
- Atlas A2 训练/推理系列产品: 不支持

---

## 8. remainder - IEEE remainder

### Libdevice 函数
`remainder(x, y)` - IEEE 余数

### AscendC 对应实现
**函数名**: `remainderf`

**函数原型**:
```cpp
__simt_callee__ inline float remainderf(float x, float y)
```

**功能说明**:
获取输入数据 x 除以 y 的余数。求余数时，商取最接近 x 除以 y 浮点数结果的整数，当 x 除以 y 的浮点数结果与左右最接近的整数距离相等时，商取偶数（IEEE 754 舍入到最近偶数规则）。

**参数**:
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| x | 输入 | 源操作数，被除数 |
| y | 输入 | 源操作数，除数 |

**返回值**:
- 输入数据 x 除以 y 的余数
- x，y 任意一个为 inf、-inf、nan 时，返回值为 nan

**头文件**:
```cpp
#include "simt_api/math_functions.h"
```

**使用示例**:
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelRemainder(__gm__ float* dst, __gm__ float* x, __gm__ float* y)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = remainderf(x[idx], y[idx]);
}
```

**产品支持**:
- Ascend 950PR/Ascend 950DT: 支持
- Atlas A3 训练/推理系列产品: 不支持
- Atlas A2 训练/推理系列产品: 不支持

**与 fmodf 的区别**:
- `fmodf`: 商向零取整（truncate）
- `remainderf`: 商四舍五入到最近整数，平局时取偶数

---

## 9. fma - Fused multiply-add

### Libdevice 函数
`fma(x, y, z)` - 融合乘加

### AscendC 对应实现
**函数名**: `fmaf`

**函数原型**:
```cpp
__simt_callee__ inline float fmaf(float x, float y, float z)
```

**功能说明**:
对输入数据 x、y、z，计算 x 与 y 相乘加上 z 的结果。公式: x * y + z

**参数**:
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| x | 输入 | 源操作数，乘数1 |
| y | 输入 | 源操作数，乘数2 |
| z | 输入 | 源操作数，加数 |

**返回值**:
- x * y + z 的值
- x 为 ±inf，y 为 ±0，返回 nan
- x 为 ±0，y 为 ±inf，返回 nan
- x*y 为 inf，z 为 -inf，返回 nan
- x*y 为 -inf，z 为 inf，返回 nan
- x*y+z 超出对应类型范围的最大值，返回 inf
- x*y+z 小于对应类型范围的最小值，返回 -inf
- x、y、z 任意一个为 nan，返回 nan

**头文件**:
```cpp
#include "simt_api/math_functions.h"
```

**使用示例**:
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelFma(__gm__ float* dst, __gm__ float* x, __gm__ float* y, __gm__ float* z)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = fmaf(x[idx], y[idx], z[idx]);
}
```

**产品支持**:
- Ascend 950PR/Ascend 950DT: 支持
- Atlas A3 训练/推理系列产品: 不支持
- Atlas A2 训练/推理系列产品: 不支持

**性能优势**:
FMA (Fused Multiply-Add) 指令在一次运算中完成乘法和加法，中间结果不进行舍入，可以提高精度并减少指令数。

---

## 10. pow - Power function

### Libdevice 函数
`pow(x, y)` - 幂函数

### AscendC 对应实现
**函数名**: `powf`

**函数原型**:
```cpp
__simt_callee__ inline float powf(float x, float y)
```

**功能说明**:
获取输入数据 x 的 y 次幂。公式: x^y

**参数**:
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| x | 输入 | 源操作数，幂计算的底数 |
| y | 输入 | 源操作数，幂计算的指数 |

**返回值**:
- x 的 y 次幂的结果
- 若 x^y 超出 float 最大范围，返回值为 inf
- 在如下边界场景，返回值为 nan:
  - 底数小于 0
  - 底数为 1 或 -1，指数为 inf
  - 底数为 1，指数为 nan
  - 底数为 0，指数为 0
  - 底数为 nan，指数为 0
  - 底数为 inf，指数为 0

**头文件**:
```cpp
#include "simt_api/math_functions.h"
```

**使用示例**:
```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelPow(__gm__ float* dst, __gm__ float* x, __gm__ float* y)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = powf(x[idx], y[idx]);
}
```

**产品支持**:
- Ascend 950PR/Ascend 950DT: 支持
- Atlas A3 训练/推理系列产品: 不支持
- Atlas A2 训练/推理系列产品: 不支持

---

## 总结

### 函数映射表

| Libdevice 函数 | AscendC 函数 | 参数 | 功能 |
|----------------|--------------|------|------|
| erfcinv | erfcinvf | (float x) | 逆互补误差函数 |
| normcdfinv | normcdfinvf | (float x) | 标准正态累积分布逆函数 |
| normcdf | normcdff | (float x) | 标准正态累积分布函数 |
| lgamma | lgammaf | (float x) | 对数伽马函数 |
| ldexp | ldexpf | (float x, int exp) | x * 2^exp |
| scalbn | scalbnf | (float x, int32_t n) | x * 2^n |
| fmod | fmodf | (float x, float y) | 浮点余数（向零取整） |
| remainder | remainderf | (float x, float y) | IEEE 余数（最近偶数取整） |
| fma | fmaf | (float x, float y, float z) | 融合乘加 x*y+z |
| pow | powf | (float x, float y) | 幂函数 x^y |

### 通用头文件

所有上述函数都需要包含:
```cpp
#include "simt_api/math_functions.h"
```

### 硬件兼容性警告

**重要**: 本批次所有 10 个函数均存在严重的硬件兼容性限制：

- **仅支持 Ascend 950PR/Ascend 950DT**
- **不支持 Atlas A2 训练/推理系列产品**
- **不支持 Atlas A3 训练/推理系列产品**

这意味着这些函数是 **SIMT 模式专用 API**，仅适用于较早期的 Ascend 950 系列硬件。在 A2/A3 系列硬件上，这些函数不可用，需要使用其他实现方式（如自定义计算逻辑或使用其他 API 组合）。

### 使用模式

所有函数都遵循 SIMT 编程模型的 `__simt_callee__` 调用约定，适用于 `__simt_vf__` 内核函数中。典型使用模式为:

```cpp
__simt_vf__ __launch_bounds__(1024) inline void KernelName(__gm__ float* dst, ...)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = function_name(...);
}
```

### 注意事项

1. **浮点精度**: 所有函数都是 float 版本（后缀 f），没有 double 版本
2. **边界处理**: 各函数都有详细的边界值处理规则，使用时需注意 nan/inf 的处理
3. **性能考虑**: `fmaf` 提供融合乘加操作，在需要高精度乘加时优先使用
4. **替代方案**: 对于 A2/A3 硬件，这些函数需要通过数学公式组合或其他 API 实现相同功能
