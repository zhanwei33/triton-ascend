# Libdevice 函数到 AscendC API 的映射分析 (Batch 18)

分析日期: 2026-03-31
分析范围: y0, y1, yn, jn, cyl_bessel_i0, cyl_bessel_i1, erf, erfinv, erfc, erfcx

---

## 1. y0 - Bessel function of second kind (order 0)

### libdevice 函数
```c
float y0f(float x);
double y0(double x);
```

### AscendC 对应实现

| 属性 | 说明 |
|------|------|
| **函数名** | `y0f` |
| **函数原型** | `__simt_callee__ inline float y0f(float x)` |
| **头文件** | `simt_api/math_functions.h` |
| **功能** | 获取输入数据x的0阶第二类贝塞尔函数y0的值 |
| **数学公式** | Y₀(x) |

### 产品支持情况
| 产品 | 支持情况 |
|------|----------|
| Ascend 950PR/Ascend 950DT | ✓ 支持 |
| Atlas A3 训练/推理系列产品 | ✗ 不支持 |
| Atlas A2 训练/推理系列产品 | ✗ 不支持 |

### 返回值说明
- 当 x < 0 时，返回值为 nan
- 当 x = 0 时，返回值为 -inf
- 当 x = inf 时，返回值为 0
- 当 x = nan 时，返回值为 nan

### 约束说明
- 使用本接口时，配置的线程数不应超过256，否则有栈溢出风险

### 调用示例
```cpp
#include "simt_api/math_functions.h"

__simt_vf__ __launch_bounds__(256) inline void KernelY0(__gm__ float* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = y0f(x[idx]);
}
```

---

## 2. y1 - Bessel function of second kind (order 1)

### libdevice 函数
```c
float y1f(float x);
double y1(double x);
```

### AscendC 对应实现

| 属性 | 说明 |
|------|------|
| **函数名** | `y1f` |
| **函数原型** | `__simt_callee__ inline float y1f(float x)` |
| **头文件** | `simt_api/math_functions.h` |
| **功能** | 获取输入数据x的1阶第二类贝塞尔函数y1的值 |
| **数学公式** | Y₁(x) |

### 产品支持情况
| 产品 | 支持情况 |
|------|----------|
| Ascend 950PR/Ascend 950DT | ✓ 支持 |
| Atlas A3 训练/推理系列产品 | ✗ 不支持 |
| Atlas A2 训练/推理系列产品 | ✗ 不支持 |

### 返回值说明
- 当 x < 0 时，返回值为 nan
- 当 x = 0 时，返回值为 -inf
- 当 x = inf 时，返回值为 0
- 当 x = nan 时，返回值为 nan

### 约束说明
- 使用本接口时，配置的线程数不应超过256，否则有栈溢出风险

### 调用示例
```cpp
#include "simt_api/math_functions.h"

__simt_vf__ __launch_bounds__(256) inline void KernelY1(__gm__ float* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = y1f(x[idx]);
}
```

---

## 3. yn - Bessel function of second kind (order n)

### libdevice 函数
```c
float ynf(int n, float x);
double yn(int n, double x);
```

### AscendC 对应实现

| 属性 | 说明 |
|------|------|
| **函数名** | `ynf` |
| **函数原型** | `__simt_callee__ inline float ynf(int n, float x)` |
| **头文件** | `simt_api/math_functions.h` |
| **功能** | 获取输入数据x的n阶第二类贝塞尔函数yn的值 |
| **数学公式** | Yₙ(x) |

### 产品支持情况
| 产品 | 支持情况 |
|------|----------|
| Ascend 950PR/Ascend 950DT | ✓ 支持 |
| Atlas A3 训练/推理系列产品 | ✗ 不支持 |
| Atlas A2 训练/推理系列产品 | ✗ 不支持 |

### 参数说明
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| n | 输入 | 阶数（最大取值为128） |
| x | 输入 | 源操作数 |

### 返回值说明
- 当 n < 0 或 x < 0 时，返回值为 nan
- 当 x = 0 时，返回值为 -inf
- 当 x = inf 时，返回值为 0
- 当 x = nan 时，返回值为 nan

### 约束说明
- 使用本接口时，配置的线程数不应超过256，否则有栈溢出风险
- n 的最大取值为 128

### 调用示例
```cpp
#include "simt_api/math_functions.h"

__simt_vf__ __launch_bounds__(256) inline void KernelYn(__gm__ float* dst, __gm__ int* n, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = ynf(n[idx], x[idx]);
}
```

---

## 4. jn - Bessel function of first kind (order n)

### libdevice 函数
```c
float jnf(int n, float x);
double jn(int n, double x);
```

### AscendC 对应实现

| 属性 | 说明 |
|------|------|
| **函数名** | `jnf` |
| **函数原型** | `__simt_callee__ inline float jnf(int n, float x)` |
| **头文件** | `simt_api/math_functions.h` |
| **功能** | 获取输入数据x的n阶第一类贝塞尔函数jn的值 |
| **数学公式** | Jₙ(x) |

### 产品支持情况
| 产品 | 支持情况 |
|------|----------|
| Ascend 950PR/Ascend 950DT | ✓ 支持 |
| Atlas A3 训练/推理系列产品 | ✗ 不支持 |
| Atlas A2 训练/推理系列产品 | ✗ 不支持 |

### 参数说明
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| n | 输入 | 阶数（最大取值为128） |
| x | 输入 | 源操作数 |

### 返回值说明
- 当 n < 0 时，返回值为 nan
- 当 x = inf 或 -inf 时，返回值为 0
- 当 x = nan 时，返回值为 nan

### 约束说明
- 使用本接口时，配置的线程数不应超过256，否则有栈溢出风险
- n 的最大取值为 128

### 调用示例
```cpp
#include "simt_api/math_functions.h"

__simt_vf__ __launch_bounds__(256) inline void KernelJn(__gm__ float* dst, __gm__ int* n, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = jnf(n[idx], x[idx]);
}
```

---

## 5. cyl_bessel_i0 - Modified Bessel function I0

### libdevice 函数
```c
float cyl_bessel_i0f(float x);
double cyl_bessel_i0(double x);
```

### AscendC 对应实现

| 属性 | 说明 |
|------|------|
| **函数名** | `cyl_bessel_i0f` |
| **函数原型** | `__simt_callee__ inline float cyl_bessel_i0f(float x)` |
| **头文件** | `simt_api/math_functions.h` |
| **功能** | 获取输入数据x的0阶常规修正圆柱贝塞尔函数的值 |
| **数学公式** | I₀(x) |

### 产品支持情况
| 产品 | 支持情况 |
|------|----------|
| Ascend 950PR/Ascend 950DT | ✓ 支持 |
| Atlas A3 训练/推理系列产品 | ✗ 不支持 |
| Atlas A2 训练/推理系列产品 | ✗ 不支持 |

### 返回值说明
- 当 x 为 0 时，返回值为 1
- 当 x 为 inf 时，返回值为 inf
- 当 x 为 -inf 时，返回值为 inf
- 当 x 为 nan 时，返回值为 nan
- 若返回值超出 float 最大范围，返回值为 inf

### 约束说明
- 无

### 调用示例
```cpp
#include "simt_api/math_functions.h"

__simt_vf__ __launch_bounds__(1024) inline void KernelCylBesselI0(__gm__ float* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = cyl_bessel_i0f(x[idx]);
}
```

---

## 6. cyl_bessel_i1 - Modified Bessel function I1

### libdevice 函数
```c
float cyl_bessel_i1f(float x);
double cyl_bessel_i1(double x);
```

### AscendC 对应实现

| 属性 | 说明 |
|------|------|
| **函数名** | `cyl_bessel_i1f` |
| **函数原型** | `__simt_callee__ inline float cyl_bessel_i1f(float x)` |
| **头文件** | `simt_api/math_functions.h` |
| **功能** | 获取输入数据x的1阶常规修正圆柱贝塞尔函数的值 |
| **数学公式** | I₁(x) |

### 产品支持情况
| 产品 | 支持情况 |
|------|----------|
| Ascend 950PR/Ascend 950DT | ✓ 支持 |
| Atlas A3 训练/推理系列产品 | ✗ 不支持 |
| Atlas A2 训练/推理系列产品 | ✗ 不支持 |

### 返回值说明
- 当 x 为 0 时，返回值为 0
- 当 x 为 inf 时，返回值为 inf
- 当 x 为 -inf 时，返回值为 -inf
- 当 x 为 nan 时，返回值为 nan
- 若返回值超出 float 最大范围，返回值为 inf
- 若返回值超出 float 最小范围，返回值为 -inf

### 约束说明
- 无

### 调用示例
```cpp
#include "simt_api/math_functions.h"

__simt_vf__ __launch_bounds__(1024) inline void KernelCylBesselI1(__gm__ float* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = cyl_bessel_i1f(x[idx]);
}
```

---

## 7. erf - Error function

### libdevice 函数
```c
float erff(float x);
double erf(double x);
```

### AscendC 对应实现（方式一：SIMT API）

| 属性 | 说明 |
|------|------|
| **函数名** | `erff` |
| **函数原型** | `__simt_callee__ inline float erff(float x)` |
| **头文件** | `simt_api/math_functions.h` |
| **功能** | 获取输入数据的误差函数值 |
| **数学公式** | erf(x) = (2/√π) ∫₀ˣ e^(-t²) dt |

### 产品支持情况（SIMT API）
| 产品 | 支持情况 |
|------|----------|
| Ascend 950PR/Ascend 950DT | ✓ 支持 |
| Atlas A3 训练/推理系列产品 | ✗ 不支持 |
| Atlas A2 训练/推理系列产品 | ✗ 不支持 |

### 返回值说明（SIMT API）
- 当 x 为 0 时，返回值为 0
- 当 x 为 nan 时，返回值为 nan
- 当 x 为 inf 时，返回值为 1
- 当 x 为 -inf 时，返回值为 -1

### AscendC 对应实现（方式二：Vector API）

| 属性 | 说明 |
|------|------|
| **函数名** | `Erf` |
| **头文件** | `kernel_operator.h` |
| **功能** | 按元素做误差函数计算（高斯误差函数） |
| **支持数据类型** | half, float |

### 产品支持情况（Vector API）
| 产品 | 支持情况 |
|------|----------|
| Ascend 950PR/Ascend 950DT | ✓ 支持 |
| Atlas A3 训练/推理系列产品 | ✓ 支持 |
| Atlas A2 训练/推理系列产品 | ✓ 支持 |
| Kirin X90 | ✓ 支持 |
| Kirin 9030 | ✓ 支持 |

### Vector API 函数原型
```cpp
// 通过 sharedTmpBuffer 入参传入临时空间
// 源操作数 Tensor 全部/部分参与计算
template <typename T, bool isReuseSource = false, const ErfConfig& config = defaultErfConfig>
__aicore__ inline void Erf(const LocalTensor<T>& dstTensor, const LocalTensor<T>& srcTensor,
                           const LocalTensor<uint8_t>& sharedTmpBuffer, const uint32_t calCount);

// 源操作数 Tensor 全部参与计算
template <typename T, bool isReuseSource = false, const ErfConfig& config = defaultErfConfig>
__aicore__ inline void Erf(const LocalTensor<T>& dstTensor, const LocalTensor<T>& srcTensor,
                           const LocalTensor<uint8_t>& sharedTmpBuffer);

// 接口框架申请临时空间
// 源操作数 Tensor 全部/部分参与计算
template <typename T, bool isReuseSource = false, const ErfConfig& config = defaultErfConfig>
__aicore__ inline void Erf(const LocalTensor<T>& dstTensor, const LocalTensor<T>& srcTensor,
                           const uint32_t calCount);

// 源操作数 Tensor 全部参与计算
template <typename T, bool isReuseSource = false, const ErfConfig& config = defaultErfConfig>
__aicore__ inline void Erf(const LocalTensor<T>& dstTensor, const LocalTensor<T>& srcTensor);
```

### ErfConfig 配置
```cpp
enum class ErfAlgo {
    PADE_APPROXIMATION = 0,              // 默认值，高性能算法（帕德近似）
    SUBSECTION_POLYNOMIAL_APPROXIMATION, // 高精度算法（分段多项式逼近）
};

struct ErfConfig {
    ErfAlgo algo = ErfAlgo::PADE_APPROXIMATION;
};
```

### 约束说明（Vector API）
- 不支持源操作数与目的操作数地址重叠

### 调用示例（SIMT API）
```cpp
#include "simt_api/math_functions.h"

__simt_vf__ __launch_bounds__(1024) inline void KernelErf(__gm__ float* x, __gm__ float* y)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    y[idx] = erff(x[idx]);
}
```

### 调用示例（Vector API）
```cpp
#include "kernel_operator.h"

__aicore__ inline void Compute()
{
    AscendC::LocalTensor<float> dstLocal = outQueue.AllocTensor<float>();
    AscendC::LocalTensor<float> srcLocal = inQueueX.DeQue<float>();

    // 使用默认配置（高性能帕德近似算法）
    AscendC::Erf<float, false>(dstLocal, srcLocal);

    // 或使用高精度算法
    // static constexpr AscendC::ErfAlgo algo = AscendC::ErfAlgo::SUBSECTION_POLYNOMIAL_APPROXIMATION;
    // static constexpr AscendC::ErfConfig config = { algo };
    // AscendC::Erf<float, false, config>(dstLocal, srcLocal);

    outQueue.EnQue<float>(dstLocal);
    inQueueX.FreeTensor(srcLocal);
}
```

---

## 8. erfinv - Inverse error function

### libdevice 函数
```c
float erfinvf(float x);
double erfinv(double x);
```

### AscendC 对应实现

| 属性 | 说明 |
|------|------|
| **函数名** | `erfinvf` |
| **函数原型** | `__simt_callee__ inline float erfinvf(float x)` |
| **头文件** | `simt_api/math_functions.h` |
| **功能** | 获取输入数据的逆误差函数值 |
| **数学公式** | erfinv(x) = erf⁻¹(x) |

### 产品支持情况
| 产品 | 支持情况 |
|------|----------|
| Ascend 950PR/Ascend 950DT | ✓ 支持 |
| Atlas A3 训练/推理系列产品 | ✗ 不支持 |
| Atlas A2 训练/推理系列产品 | ✗ 不支持 |

### 返回值说明
- 当 x 为 0 时，返回值为 0
- 当 x 为 nan 时，返回值为 nan
- 当 x 为 1 时，返回值为 inf
- 当 x 为 -1 时，返回值为 -inf
- 当 x ∉ [-1, 1] 时，返回 nan

### 约束说明
- 无

### 调用示例
```cpp
#include "simt_api/math_functions.h"

__simt_vf__ __launch_bounds__(1024) inline void KernelErfinv(__gm__ float* x, __gm__ float* y)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    y[idx] = erfinvf(x[idx]);
}
```

---

## 9. erfc - Complementary error function

### libdevice 函数
```c
float erfcf(float x);
double erfc(double x);
```

### AscendC 对应实现

| 属性 | 说明 |
|------|------|
| **函数名** | `erfcf` |
| **函数原型** | `__simt_callee__ inline float erfcf(float x)` |
| **头文件** | `simt_api/math_functions.h` |
| **功能** | 获取输入数据的互补误差函数值 |
| **数学公式** | erfc(x) = 1 - erf(x) = (2/√π) ∫ₓ^∞ e^(-t²) dt |

### 产品支持情况
| 产品 | 支持情况 |
|------|----------|
| Ascend 950PR/Ascend 950DT | ✓ 支持 |
| Atlas A3 训练/推理系列产品 | ✗ 不支持 |
| Atlas A2 训练/推理系列产品 | ✗ 不支持 |

### 返回值说明
- 当 x 为 nan 时，返回值为 nan
- 当 x 为 inf 时，返回值为 0
- 当 x 为 -inf 时，返回值为 2

### 约束说明
- 无

### 调用示例
```cpp
#include "simt_api/math_functions.h"

__simt_vf__ __launch_bounds__(1024) inline void KernelErfc(__gm__ float* x, __gm__ float* y)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    y[idx] = erfcf(x[idx]);
}
```

---

## 10. erfcx - Scaled complementary error function

### libdevice 函数
```c
float erfcxf(float x);
double erfcx(double x);
```

### AscendC 对应实现

| 属性 | 说明 |
|------|------|
| **函数名** | `erfcxf` |
| **函数原型** | `__simt_callee__ inline float erfcxf(float x)` |
| **头文件** | `simt_api/math_functions.h` |
| **功能** | 获取输入数据的缩放互补误差函数值 |
| **数学公式** | erfcx(x) = e^(x²) · erfc(x) |

### 产品支持情况
| 产品 | 支持情况 |
|------|----------|
| Ascend 950PR/Ascend 950DT | ✓ 支持 |
| Atlas A3 训练/推理系列产品 | ✗ 不支持 |
| Atlas A2 训练/推理系列产品 | ✗ 不支持 |

### 返回值说明
- 当 x 为 0 时，返回值为 1
- 当 x 为 nan 时，返回值为 nan
- 当 x 为 -inf 时，返回值为 inf
- 当 x 为 inf 时，返回值为 0

### 约束说明
- 无

### 调用示例
```cpp
#include "simt_api/math_functions.h"

__simt_vf__ __launch_bounds__(1024) inline void KernelErfcx(__gm__ float* x, __gm__ float* y)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    y[idx] = erfcxf(x[idx]);
}
```

---

## 汇总表

| libdevice 函数 | AscendC 函数 | 头文件 | 支持产品 | 特殊约束 |
|----------------|--------------|--------|----------|----------|
| y0f/y0 | y0f | simt_api/math_functions.h | Ascend 950PR/950DT | 线程数 ≤ 256 |
| y1f/y1 | y1f | simt_api/math_functions.h | Ascend 950PR/950DT | 线程数 ≤ 256 |
| ynf/yn | ynf | simt_api/math_functions.h | Ascend 950PR/950DT | 线程数 ≤ 256, n ≤ 128 |
| jnf/jn | jnf | simt_api/math_functions.h | Ascend 950PR/950DT | 线程数 ≤ 256, n ≤ 128 |
| cyl_bessel_i0f/i0 | cyl_bessel_i0f | simt_api/math_functions.h | Ascend 950PR/950DT | 无 |
| cyl_bessel_i1f/i1 | cyl_bessel_i1f | simt_api/math_functions.h | Ascend 950PR/950DT | 无 |
| erff/erf | erff / Erf | simt_api/math_functions.h / kernel_operator.h | 见上方详细说明 | Vector API 有地址重叠限制 |
| erfinvf/erfinv | erfinvf | simt_api/math_functions.h | Ascend 950PR/950DT | 无 |
| erfcf/erfc | erfcf | simt_api/math_functions.h | Ascend 950PR/950DT | 无 |
| erfcxf/erfcx | erfcxf | simt_api/math_functions.h | Ascend 950PR/950DT | 无 |

---

## 重要说明

1. **硬件兼容性限制**: 本批次分析的10个函数中，除 `Erf` 的 Vector API 外，其他所有函数**仅支持 Ascend 950PR/Ascend 950DT** 产品，**不支持 Atlas A2 和 Atlas A3 系列产品**。

2. **两种编程模型**:
   - **SIMT API** (`simt_api/math_functions.h`): 类似 CUDA 的线程级编程模型，适用于细粒度并行计算
   - **Vector API** (`kernel_operator.h`): AscendC 原生向量编程模型，适用于算子开发

3. **double 类型支持**: 当前 AscendC API 仅提供 float 类型的单精度版本（带 `f` 后缀），如需 double 类型支持需要自行实现或转换。

4. **线程数限制**: 贝塞尔函数（y0f, y1f, ynf, jnf）有明确的线程数限制（≤256），超过此限制可能导致栈溢出。
