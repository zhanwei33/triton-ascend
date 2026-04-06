# Libdevice 函数到 AscendC API 的映射分析 - Batch 16

**分析范围**: atan, asin, acos, log, log10, log1p, acosh, asinh, atanh, expm1

**分析日期**: 2026-03-31

---

## 1. atan - Arctangent (反正切函数)

### AscendC 对应实现

| 属性 | 说明 |
|------|------|
| **函数名** | `Atan` |
| **头文件** | `kernel_operator.h` |
| **API 类型** | 张量运算 (LocalTensor) |

### 函数原型

```cpp
// 通过 sharedTmpBuffer 入参传入临时空间
template <typename T, bool isReuseSource = false, const AtanConfig& config = defaultAtanConfig>
__aicore__ inline void Atan(const LocalTensor<T>& dstTensor, const LocalTensor<T>& srcTensor,
                            const LocalTensor<uint8_t>& sharedTmpBuffer, const uint32_t calCount);

template <typename T, bool isReuseSource = false, const AtanConfig& config = defaultAtanConfig>
__aicore__ inline void Atan(const LocalTensor<T>& dstTensor, const LocalTensor<T>& srcTensor,
                            const LocalTensor<uint8_t>& sharedTmpBuffer);

// 接口框架申请临时空间
template <typename T, bool isReuseSource = false, const AtanConfig& config = defaultAtanConfig>
__aicore__ inline void Atan(const LocalTensor<T>& dstTensor, const LocalTensor<T>& srcTensor,
                            const uint32_t calCount);

template <typename T, bool isReuseSource = false, const AtanConfig& config = defaultAtanConfig>
__aicore__ inline void Atan(const LocalTensor<T>& dstTensor, const LocalTensor<T>& srcTensor);
```

### 产品支持情况

| 产品 | 支持情况 |
|------|----------|
| Ascend 950PR/Ascend 950DT | √ |
| Atlas A3 训练/推理系列产品 | √ |
| Atlas A2 训练/推理系列产品 | √ |
| Kirin X90 | √ |
| Kirin 9030 | √ |

### 数据类型支持
- `half`, `float`

### 特殊配置

```cpp
enum class AtanAlgo {
    TAYLOR_EXPANSION = 0,      // 默认值，6阶泰勒展开，支持half/float
    POLYNOMIAL_APPROXIMATION,  // 17次多项式逼近，仅支持float
};

struct AtanConfig {
    AtanAlgo algo = AtanAlgo::TAYLOR_EXPANSION;
};
```

### 使用示例

```cpp
// 使用默认算法
AscendC::Atan(dstLocal, srcLocal, sharedTmpBuffer, 512);

// 指定多项式逼近算法
static constexpr AscendC::AtanConfig atanConfig = {AscendC::AtanAlgo::POLYNOMIAL_APPROXIMATION};
AscendC::Atan<float, false, atanConfig>(dstLocal, srcLocal, sharedTmpBuffer, 512);
```

---

## 2. asin - Arcsine (反正弦函数)

### AscendC 对应实现

| 属性 | 说明 |
|------|------|
| **函数名** | `Asin` |
| **头文件** | `kernel_operator.h` |
| **API 类型** | 张量运算 (LocalTensor) |

### 函数原型

```cpp
// 通过 sharedTmpBuffer 入参传入临时空间
template <typename T, bool isReuseSource = false>
__aicore__ inline void Asin(const LocalTensor<T>& dstTensor, const LocalTensor<T>& srcTensor,
                            const LocalTensor<uint8_t>& sharedTmpBuffer, const uint32_t calCount);

template <typename T, bool isReuseSource = false>
__aicore__ inline void Asin(const LocalTensor<T>& dstTensor, const LocalTensor<T>& srcTensor,
                            const LocalTensor<uint8_t>& sharedTmpBuffer);

// 接口框架申请临时空间
template <typename T, bool isReuseSource = false>
__aicore__ inline void Asin(const LocalTensor<T>& dstTensor, const LocalTensor<T>& srcTensor,
                            const uint32_t calCount);

template <typename T, bool isReuseSource = false>
__aicore__ inline void Asin(const LocalTensor<T>& dstTensor, const LocalTensor<T>& srcTensor);
```

### 产品支持情况

| 产品 | 支持情况 |
|------|----------|
| Ascend 950PR/Ascend 950DT | √ |
| Atlas A3 训练/推理系列产品 | √ |
| Atlas A2 训练/推理系列产品 | √ |
| Kirin X90 | √ |
| Kirin 9030 | √ |

### 数据类型支持
- `half`, `float`

### 约束说明
- **输入值域**: [-1, 1]，若输入不在范围内，输出结果无效
- 不支持源操作数与目的操作数地址重叠
- 不支持 sharedTmpBuffer 与源/目的操作数地址重叠

### 使用示例

```cpp
AscendC::Asin(dstLocal, srcLocal, sharedTmpBuffer, 512);
```

---

## 3. acos - Arccosine (反余弦函数)

### AscendC 对应实现

| 属性 | 说明 |
|------|------|
| **函数名** | `Acos` |
| **头文件** | `kernel_operator.h` |
| **API 类型** | 张量运算 (LocalTensor) |

### 函数原型

```cpp
// 通过 sharedTmpBuffer 入参传入临时空间
template <typename T, bool isReuseSource = false>
__aicore__ inline void Acos(const LocalTensor<T>& dstTensor, const LocalTensor<T>& srcTensor,
                            const LocalTensor<uint8_t>& sharedTmpBuffer, const uint32_t calCount);

template <typename T, bool isReuseSource = false>
__aicore__ inline void Acos(const LocalTensor<T>& dstTensor, const LocalTensor<T>& srcTensor,
                            const LocalTensor<uint8_t>& sharedTmpBuffer);

// 接口框架申请临时空间
template <typename T, bool isReuseSource = false>
__aicore__ inline void Acos(const LocalTensor<T>& dstTensor, const LocalTensor<T>& srcTensor,
                            const uint32_t calCount);

template <typename T, bool isReuseSource = false>
__aicore__ inline void Acos(const LocalTensor<T>& dstTensor, const LocalTensor<T>& srcTensor);
```

### 产品支持情况

| 产品 | 支持情况 |
|------|----------|
| Ascend 950PR/Ascend 950DT | √ |
| Atlas A3 训练/推理系列产品 | √ |
| Atlas A2 训练/推理系列产品 | √ |
| Kirin X90 | √ |
| Kirin 9030 | √ |

### 数据类型支持
- `half`, `float`

### 约束说明
- **输入值域**: [-1, 1]，若输入不在范围内，输出结果无效
- 不支持源操作数与目的操作数地址重叠
- 不支持 sharedTmpBuffer 与源/目的操作数地址重叠

### 使用示例

```cpp
AscendC::Acos(dstLocal, srcLocal, sharedTmpBuffer, 512);
```

---

## 4. log - Natural Logarithm (自然对数)

### AscendC 对应实现

| 属性 | 说明 |
|------|------|
| **函数名** | `Log` |
| **头文件** | `kernel_operator.h` |
| **API 类型** | 张量运算 (LocalTensor) |

### 函数原型

```cpp
// 以e为底 - 源操作数Tensor全部/部分参与计算
template<typename T, bool isReuseSource = false>
__aicore__ inline void Log(const LocalTensor<T>& dstTensor, const LocalTensor<T>& srcTensor,
                           uint32_t calCount);

// 以e为底 - 源操作数Tensor全部参与计算
template<typename T, bool isReuseSource = false>
__aicore__ inline void Log(const LocalTensor<T>& dstTensor, const LocalTensor<T>& srcTensor);
```

### 产品支持情况

| 产品 | 支持情况 |
|------|----------|
| Ascend 950PR/Ascend 950DT | √ |
| Atlas A3 训练/推理系列产品 | √ |
| Atlas A2 训练/推理系列产品 | √ |
| Kirin X90 | √ |
| Kirin 9030 | √ |

### 数据类型支持
- `half`, `float`

### 约束说明
- 不支持源操作数与目的操作数地址重叠

### 使用示例

```cpp
AscendC::Log(dstLocal, srcLocal);
// 或指定计算数量
AscendC::Log(dstLocal, srcLocal, 512);
```

---

## 5. log10 - Log Base 10 (以10为底的对数)

### AscendC 对应实现

| 属性 | 说明 |
|------|------|
| **函数名** | `Log10` |
| **头文件** | `kernel_operator.h` |
| **API 类型** | 张量运算 (LocalTensor) |

### 函数原型

```cpp
// 以10为底 - 源操作数Tensor全部/部分参与计算
template<typename T, bool isReuseSource = false>
__aicore__ inline void Log10(const LocalTensor<T>& dstTensor, const LocalTensor<T>& srcTensor,
                             uint32_t calCount);

// 以10为底 - 源操作数Tensor全部参与计算
template<typename T, bool isReuseSource = false>
__aicore__ inline void Log10(const LocalTensor<T>& dstTensor, const LocalTensor<T>& srcTensor);
```

### 产品支持情况

| 产品 | 支持情况 |
|------|----------|
| Ascend 950PR/Ascend 950DT | √ |
| Atlas A3 训练/推理系列产品 | √ |
| Atlas A2 训练/推理系列产品 | √ |
| Kirin X90 | √ |
| Kirin 9030 | √ |

### 数据类型支持
- `half`, `float`

### 约束说明
- 不支持源操作数与目的操作数地址重叠

### 使用示例

```cpp
AscendC::Log10(dstLocal, srcLocal);
// 或指定计算数量
AscendC::Log10(dstLocal, srcLocal, 512);
```

---

## 6. log1p - Log(1+x) (自然对数1+x)

### AscendC 对应实现

| 属性 | 说明 |
|------|------|
| **函数名** | `log1pf` |
| **头文件** | `simt_api/math_functions.h` |
| **API 类型** | SIMT 标量运算 |
| **适用场景** | CUDA 兼容模式 / SIMT 编程模型 |

### 函数原型

```cpp
__simt_callee__ inline float log1pf(float x)
```

### 产品支持情况

| 产品 | 支持情况 |
|------|----------|
| Ascend 950PR/Ascend 950DT | √ |
| Atlas A3 训练/推理系列产品 | x (不支持) |
| Atlas A2 训练/推理系列产品 | x (不支持) |

### 数据类型支持
- `float`

### 返回值说明
- 以e为底的x+1的对数
- 当x为inf时，返回值为inf
- 当x为-inf时，返回值为nan
- 当x为nan时，返回值为nan

### 使用示例

```cpp
#include "simt_api/math_functions.h"

__simt_vf__ __launch_bounds__(1024) inline void KernelLog1p(__gm__ float* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = log1pf(x[idx]);
}
```

### 替代方案
对于 A2/A3 系列产品，可以使用组合运算实现:
```cpp
// 使用 Log 和 Add 组合实现 log1p
AscendC::Adds(tmpLocal, srcLocal, 1.0f, calCount);  // tmp = x + 1
AscendC::Log(dstLocal, tmpLocal, calCount);          // dst = log(tmp)
```

---

## 7. acosh - Inverse Hyperbolic Cosine (反双曲余弦)

### AscendC 对应实现

| 属性 | 说明 |
|------|------|
| **函数名** | `Acosh` |
| **头文件** | `kernel_operator.h` |
| **API 类型** | 张量运算 (LocalTensor) |

### 函数原型

```cpp
// 通过 sharedTmpBuffer 入参传入临时空间
template <typename T, bool isReuseSource = false>
__aicore__ inline void Acosh(const LocalTensor<T>& dstTensor, const LocalTensor<T>& srcTensor,
                             const LocalTensor<uint8_t>& sharedTmpBuffer, const uint32_t calCount);

template <typename T, bool isReuseSource = false>
__aicore__ inline void Acosh(const LocalTensor<T>& dstTensor, const LocalTensor<T>& srcTensor,
                             const LocalTensor<uint8_t>& sharedTmpBuffer);

// 接口框架申请临时空间
template <typename T, bool isReuseSource = false>
__aicore__ inline void Acosh(const LocalTensor<T>& dstTensor, const LocalTensor<T>& srcTensor,
                             const uint32_t calCount);

template <typename T, bool isReuseSource = false>
__aicore__ inline void Acosh(const LocalTensor<T>& dstTensor, const LocalTensor<T>& srcTensor);
```

### 产品支持情况

| 产品 | 支持情况 |
|------|----------|
| Ascend 950PR/Ascend 950DT | √ |
| Atlas A3 训练/推理系列产品 | √ |
| Atlas A2 训练/推理系列产品 | √ |

### 数据类型支持
- `half`, `float`

### 约束说明
- **输入值域**: [1, 65504]，若输入不在范围内，输出结果无效
- 不支持源操作数与目的操作数地址重叠
- 不支持 sharedTmpBuffer 与源/目的操作数地址重叠

### 使用示例

```cpp
AscendC::Acosh(dstLocal, srcLocal, sharedTmpBuffer, 512);
```

---

## 8. asinh - Inverse Hyperbolic Sine (反双曲正弦)

### AscendC 对应实现

| 属性 | 说明 |
|------|------|
| **函数名** | `Asinh` |
| **头文件** | `kernel_operator.h` |
| **API 类型** | 张量运算 (LocalTensor) |

### 函数原型

```cpp
// 通过 sharedTmpBuffer 入参传入临时空间
template <typename T, bool isReuseSource = false>
__aicore__ inline void Asinh(const LocalTensor<T>& dstTensor, const LocalTensor<T>& srcTensor,
                             const LocalTensor<uint8_t>& sharedTmpBuffer, const uint32_t calCount);

template <typename T, bool isReuseSource = false>
__aicore__ inline void Asinh(const LocalTensor<T>& dstTensor, const LocalTensor<T>& srcTensor,
                             const LocalTensor<uint8_t>& sharedTmpBuffer);

// 接口框架申请临时空间
template <typename T, bool isReuseSource = false>
__aicore__ inline void Asinh(const LocalTensor<T>& dstTensor, const LocalTensor<T>& srcTensor,
                             const uint32_t calCount);

template <typename T, bool isReuseSource = false>
__aicore__ inline void Asinh(const LocalTensor<T>& dstTensor, const LocalTensor<T>& srcTensor);
```

### 产品支持情况

| 产品 | 支持情况 |
|------|----------|
| Ascend 950PR/Ascend 950DT | √ |
| Atlas A3 训练/推理系列产品 | √ |
| Atlas A2 训练/推理系列产品 | √ |

### 数据类型支持
- `half`, `float`

### 约束说明
- **输入值域**: [-65504, 65504]，若输入不在范围内，输出结果无效
- 不支持源操作数与目的操作数地址重叠
- 不支持 sharedTmpBuffer 与源/目的操作数地址重叠

### 使用示例

```cpp
AscendC::Asinh(dstLocal, srcLocal, sharedTmpBuffer, 512);
```

---

## 9. atanh - Inverse Hyperbolic Tangent (反双曲正切)

### AscendC 对应实现

| 属性 | 说明 |
|------|------|
| **函数名** | `Atanh` |
| **头文件** | `kernel_operator.h` |
| **API 类型** | 张量运算 (LocalTensor) |

### 函数原型

```cpp
// 通过 sharedTmpBuffer 入参传入临时空间
template <typename T, bool isReuseSource = false>
__aicore__ inline void Atanh(const LocalTensor<T>& dstTensor, const LocalTensor<T>& srcTensor,
                             const LocalTensor<uint8_t>& sharedTmpBuffer, const uint32_t calCount);

template <typename T, bool isReuseSource = false>
__aicore__ inline void Atanh(const LocalTensor<T>& dstTensor, const LocalTensor<T>& srcTensor,
                             const LocalTensor<uint8_t>& sharedTmpBuffer);

// 接口框架申请临时空间
template <typename T, bool isReuseSource = false>
__aicore__ inline void Atanh(const LocalTensor<T>& dstTensor, const LocalTensor<T>& srcTensor,
                             const uint32_t calCount);

template <typename T, bool isReuseSource = false>
__aicore__ inline void Atanh(const LocalTensor<T>& dstTensor, const LocalTensor<T>& srcTensor);
```

### 产品支持情况

| 产品 | 支持情况 |
|------|----------|
| Ascend 950PR/Ascend 950DT | √ |
| Atlas A3 训练/推理系列产品 | √ |
| Atlas A2 训练/推理系列产品 | √ |

### 数据类型支持
- `half`, `float`

### 约束说明
- **输入值域**: (-0.99, -0.001) 或 (0.001, 0.99) 区间内，若输入不在范围内，输出结果无效
- 不支持源操作数与目的操作数地址重叠
- 不支持 sharedTmpBuffer 与源/目的操作数地址重叠

### 使用示例

```cpp
AscendC::Atanh(dstLocal, srcLocal, sharedTmpBuffer, 512);
```

---

## 10. expm1 - exp(x)-1 (指数减1)

### AscendC 对应实现

| 属性 | 说明 |
|------|------|
| **函数名** | `expm1f` |
| **头文件** | `simt_api/math_functions.h` |
| **API 类型** | SIMT 标量运算 |
| **适用场景** | CUDA 兼容模式 / SIMT 编程模型 |

### 函数原型

```cpp
__simt_callee__ inline float expm1f(float x)
```

### 产品支持情况

| 产品 | 支持情况 |
|------|----------|
| Ascend 950PR/Ascend 950DT | √ |
| Atlas A3 训练/推理系列产品 | x (不支持) |
| Atlas A2 训练/推理系列产品 | x (不支持) |

### 数据类型支持
- `float`

### 返回值说明
- e的x次方减1
- 当x为inf时，返回值为inf
- 当x为-inf时，返回值为-1.0
- 当x为nan时，返回值为nan

### 使用示例

```cpp
#include "simt_api/math_functions.h"

__simt_vf__ __launch_bounds__(1024) inline void KernelExpm1(__gm__ float* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = expm1f(x[idx]);
}
```

### 替代方案
对于 A2/A3 系列产品，可以使用组合运算实现:
```cpp
// 使用 Exp 和 Sub 组合实现 expm1
AscendC::Exp(tmpLocal, srcLocal, calCount);          // tmp = exp(x)
AscendC::Adds(dstLocal, tmpLocal, -1.0f, calCount);  // dst = tmp - 1
```

---

## 汇总表

| libdevice 函数 | AscendC API | 头文件 | 产品支持 | 数据类型 | 备注 |
|---------------|-------------|--------|----------|----------|------|
| atan | `Atan` | `kernel_operator.h` | 950PR/950DT, A2, A3, X90, 9030 | half, float | 支持两种算法配置 |
| asin | `Asin` | `kernel_operator.h` | 950PR/950DT, A2, A3, X90, 9030 | half, float | 输入值域[-1,1] |
| acos | `Acos` | `kernel_operator.h` | 950PR/950DT, A2, A3, X90, 9030 | half, float | 输入值域[-1,1] |
| log | `Log` | `kernel_operator.h` | 950PR/950DT, A2, A3, X90, 9030 | half, float | 以e为底 |
| log10 | `Log10` | `kernel_operator.h` | 950PR/950DT, A2, A3, X90, 9030 | half, float | 以10为底 |
| log1p | `log1pf` | `simt_api/math_functions.h` | 仅 950PR/950DT | float | SIMT API，A2/A3需组合实现 |
| acosh | `Acosh` | `kernel_operator.h` | 950PR/950DT, A2, A3 | half, float | 输入值域[1,65504] |
| asinh | `Asinh` | `kernel_operator.h` | 950PR/950DT, A2, A3 | half, float | 输入值域[-65504,65504] |
| atanh | `Atanh` | `kernel_operator.h` | 950PR/950DT, A2, A3 | half, float | 输入值域限制严格 |
| expm1 | `expm1f` | `simt_api/math_functions.h` | 仅 950PR/950DT | float | SIMT API，A2/A3需组合实现 |

---

## 重要注意事项

### 1. 临时空间管理

对于 `Atan`, `Asin`, `Acos`, `Acosh`, `Asinh`, `Atanh` 等复杂数学函数，需要临时空间存储中间变量:

```cpp
// 获取临时空间大小
uint32_t bufferSize = GetAtanMaxMinTmpSize<T>(calCount);  // 以 Atan 为例

// 申请临时空间
AscendC::TPipe pipe;
AscendC::TQue<AscendC::TPosition::VECCALC, 1> tmpQue;
pipe.InitBuffer(tmpQue, 1, bufferSize);
AscendC::LocalTensor<uint8_t> sharedTmpBuffer = tmpQue.AllocTensor<uint8_t>();

// 调用函数
AscendC::Atan(dstLocal, srcLocal, sharedTmpBuffer, calCount);
```

### 2. 值域约束

反三角函数和反双曲函数有严格的输入值域限制:
- `Asin`/`Acos`: 输入必须在 [-1, 1] 范围内
- `Acosh`: 输入必须在 [1, 65504] 范围内
- `Atanh`: 输入必须在 (-0.99, -0.001) 或 (0.001, 0.99) 范围内

### 3. 平台兼容性

`log1pf` 和 `expm1f` 是 SIMT API，仅支持 Ascend 950PR/950DT:
- 对于 A2/A3 系列产品，需要使用组合运算实现
- 这些函数主要用于 CUDA 兼容编程场景

### 4. 地址重叠约束

所有张量运算 API 都不支持:
- 源操作数与目的操作数地址重叠
- sharedTmpBuffer 与源/目的操作数地址重叠
