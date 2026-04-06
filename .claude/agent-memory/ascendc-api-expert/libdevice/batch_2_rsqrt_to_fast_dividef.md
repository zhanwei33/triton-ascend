# Libdevice 函数 AscendC 对应实现分析 - Batch 2

## 概述

本文档分析 10 个 libdevice 函数在 AscendC 中的对应实现方式，涵盖 rsqrt、ceil、trunc、exp2、saturatef、各种 fma 变体以及 fast_dividef。

---

## 1. rsqrt - Reciprocal Square Root (平方根倒数)

### libdevice 原型
```c
float rsqrtf(float x);  // 计算 1/sqrt(x)
```

### AscendC 对应实现

#### C++ API (SIMT 风格)
```c++
__simt_callee__ inline float rsqrtf(float x)
```
- **头文件**: `#include "simt_api/math_functions.h"`
- **支持产品**: Ascend 950PR/Ascend 950DT
- **不支持**: Atlas A3、Atlas A2 系列

**功能**: 获取输入数据x的平方根的倒数，公式: $dst = 1/\sqrt{x}$

**特殊场景**:
- x为0时，返回inf
- x为inf时，返回0
- x为-inf时，返回nan
- x为nan时，返回nan

**约束**: 输入数据范围x必须大于等于0，否则返回nan

#### C API (A2/A3 系列)
```c++
// 前n个数据计算
__aicore__ inline void asc_rsqrt(__ubuf__ half* dst, __ubuf__ half* src, uint32_t count)
__aicore__ inline void asc_rsqrt(__ubuf__ float* dst, __ubuf__ float* src, uint32_t count)

// 高维切分计算
__aicore__ inline void asc_rsqrt(__ubuf__ half* dst, __ubuf__ half* src, uint8_t repeat,
    uint16_t dst_block_stride, uint16_t src_block_stride,
    uint16_t dst_repeat_stride, uint16_t src_repeat_stride)

// 同步计算
__aicore__ inline void asc_rsqrt_sync(__ubuf__ half* dst, __ubuf__ half* src, uint32_t count)
```
- **支持产品**: Atlas A3 训练/推理、Atlas A2 训练/推理
- **流水类型**: PIPE_V
- **约束**: dst、src起始地址需要32字节对齐

#### 高阶 API (Tensor 操作)
```c++
template <typename T>
__aicore__ inline void Rsqrt(const LocalTensor<T>& dst, const LocalTensor<T>& src, const int32_t& count)
```
- **支持产品**: Atlas A3、Atlas A2、Kirin X90、Kirin 9030
- **数据类型**: half、float

---

## 2. ceil - Ceiling Function (向上取整)

### libdevice 原型
```c
float ceilf(float x);  // 获取大于或等于x的最小整数
```

### AscendC 对应实现

#### C++ API (SIMT 风格)
```c++
__simt_callee__ inline float ceilf(float x)
```
- **头文件**: `#include "simt_api/math_functions.h"`
- **支持产品**: Ascend 950PR/Ascend 950DT
- **不支持**: Atlas A3、Atlas A2 系列

**功能**: 获取大于或等于输入数据的最小整数值

**特殊场景**:
- x为nan时，返回nan
- x为inf时，返回inf
- x为-inf时，返回-inf

#### C API (寄存器计算 - 950 系列)
```c++
// CEIL舍入模式
__simd_callee__ inline void asc_ceil(vector_half& dst, vector_half src, vector_bool mask)
__simd_callee__ inline void asc_ceil(vector_bfloat16_t& dst, vector_bfloat16_t src, vector_bool mask)
__simd_callee__ inline void asc_ceil(vector_float& dst, vector_float src, vector_bool mask)
```
- **支持产品**: Ascend 950PR/Ascend 950DT
- **流水类型**: PIPE_V

#### 高阶 API (Cast + RoundMode)
可以通过 `Cast` 接口配合 `RoundMode::CAST_CEIL` 实现:
```c++
// 使用 Cast 接口，设置 RoundMode 为 CAST_CEIL
RoundMode::CAST_CEIL  // 向正无穷舍入（向上取整）
```

---

## 3. trunc - Truncate Function (向零取整/截断)

### libdevice 原型
```c
float truncf(float x);  // 截断小数部分，向零取整
```

### AscendC 对应实现

#### C++ API (SIMT 风格)
```c++
__simt_callee__ inline float truncf(float x)
```
- **头文件**: `#include "simt_api/math_functions.h"`
- **支持产品**: Ascend 950PR/Ascend 950DT
- **不支持**: Atlas A3、Atlas A2 系列

**功能**: 获取对输入数据的浮点数截断后的整数（向零取整）

**特殊场景**:
- x为nan时，返回nan
- x为inf时，返回inf
- x为-inf时，返回-inf

#### C API (寄存器计算 - 950 系列)
```c++
// TRUNC舍入模式
__simd_callee__ inline void asc_trunc(vector_half& dst, vector_half src, vector_bool mask)
__simd_callee__ inline void asc_trunc(vector_bfloat16_t& dst, vector_bfloat16_t src, vector_bool mask)
__simd_callee__ inline void asc_trunc(vector_float& dst, vector_float src, vector_bool mask)
```
- **支持产品**: Ascend 950PR/Ascend 950DT

#### 高阶 API (Cast + RoundMode)
```c++
RoundMode::CAST_TRUNC  // 向零舍入（截断）
```

---

## 4. exp2 - Base-2 Exponential (以2为底的指数)

### libdevice 原型
```c
float exp2f(float x);  // 计算 2^x
```

### AscendC 对应实现

#### C++ API (SIMT 风格)
```c++
__simt_callee__ inline float exp2f(float x)
```
- **头文件**: `#include "simt_api/math_functions.h"`
- **支持产品**: Ascend 950PR/Ascend 950DT
- **不支持**: Atlas A3、Atlas A2 系列

**功能**: 指定输入x，获取2的x次方，公式: $dst = 2^x$

**特殊场景**:
- x为inf时，返回inf
- x为-inf时，返回0
- x为nan时，返回nan
- 结果超出float最大范围时，返回inf

#### Half 精度变体
```c++
// half 类型
__simt_callee__ inline half hexp2(half x)      // 单 half
__simt_callee__ inline half2 h2exp2(half2 x)   // half2 向量
```

---

## 5. saturatef - Saturate Float (浮点饱和)

### libdevice 原型
```c
float saturatef(float x);  // 将值限制在 [0.0, 1.0] 范围内
```

### AscendC 对应实现

**重要发现**: 在 AscendC 官方 API 文档中，**没有直接找到 `saturatef` 的对应实现**。

#### 替代实现方案
可以通过组合现有 API 实现:

```c++
// 方案1: 使用 Max + Min 组合
// saturate(x) = min(max(x, 0.0f), 1.0f)

// 方案2: 使用 Select 条件选择
// 通过比较操作和 select 实现饱和

// 方案3: 使用 Relu + Min 组合
// 先通过 Relu 确保 >= 0，再通过 Min 确保 <= 1
```

#### 相关 API
- `AscendC::Max` / `AscendC::Min` - 按元素求最大/最小值
- `AscendC::Select` - 根据掩码选择元素
- `asc_max` / `asc_min` (C API) - 矢量最大/最小值

**建议**: 对于 saturatef 功能，建议使用 `min(max(x, 0.0f), 1.0f)` 的组合方式实现。

---

## 6-9. FMA 变体 - Fused Multiply-Add with Rounding Modes

### libdevice 原型
```c
float fmaf(float x, float y, float z);           // 标准 FMA，round to nearest
float fma_rn(float x, float y, float z);         // FMA round to nearest (RN)
float fma_rz(float x, float y, float z);         // FMA round toward zero (RZ)
float fma_rd(float x, float y, float z);         // FMA round downward (RD - 向负无穷)
float fma_ru(float x, float y, float z);         // FMA round upward (RU - 向正无穷)
```

### AscendC 对应实现

#### C++ API (SIMT 风格)
```c++
__simt_callee__ inline float fmaf(float x, float y, float z)
```
- **头文件**: `#include "simt_api/math_functions.h"`
- **支持产品**: Ascend 950PR/Ascend 950DT
- **不支持**: Atlas A3、Atlas A2 系列

**功能**: 计算 x * y + z 的结果，公式: $dst = x \times y + z$

**特殊场景**:
- x为±inf，y为±0，返回nan
- x为±0，y为±inf，返回nan
- x*y为inf，z为-inf，返回nan
- x*y为-inf，z为inf，返回nan
- x*y+z超出范围，返回inf或-inf
- x、y、z任意一个为nan，返回nan

#### C API (A2/A3 系列)
```c++
// 前n个数据计算
__aicore__ inline void asc_fma(__ubuf__ half* dst, __ubuf__ half* src0, __ubuf__ half* src1, uint32_t count)
__aicore__ inline void asc_fma(__ubuf__ float* dst, __ubuf__ float* src0, __ubuf__ float* src1, uint32_t count)

// 同步计算
__aicore__ inline void asc_fma_sync(__ubuf__ half* dst, __ubuf__ half* src0, __ubuf__ half* src1, uint32_t count)
__aicore__ inline void asc_fma_sync(__ubuf__ float* dst, __ubuf__ float* src0, __ubuf__ float* src1, uint32_t count)
```
- **支持产品**: Atlas A3、Atlas A2 系列
- **公式**: $dst_i = (src0_i \times src1_i) + dst_i$ (注意: dst 同时作为输入和输出)

#### 高阶 API (Tensor 操作)
```c++
template <const FmaConfig& config = DEFAULT_FMA_CONFIG, typename T>
__aicore__ inline void Fma(const LocalTensor<T>& dst, const LocalTensor<T>& src0,
    const LocalTensor<T>& src1, const LocalTensor<T>& src2,
    const LocalTensor<uint8_t>& sharedTmpBuffer, const uint32_t count)
```
- **支持产品**: Ascend 950PR/Ascend 950DT
- **数据类型**: half、float
- **注意**: 需要额外的临时空间存储中间变量

#### 关于舍入模式变体 (fma_rn/fma_rz/fma_rd/fma_ru)

**重要发现**: 在 AscendC API 文档中，**没有直接找到带显式舍入模式参数的 FMA 变体**。

##### RoundMode 枚举定义
```c++
enum class RoundMode {
    CAST_NONE = 0,   // 不涉及精度损失时表示不舍入
    CAST_RINT,       // rint，四舍六入五成双舍入 (对应 RN)
    CAST_FLOOR,      // floor，向负无穷舍入 (对应 RD)
    CAST_CEIL,       // ceil，向正无穷舍入 (对应 RU)
    CAST_ROUND,      // round，四舍五入舍入
    CAST_TRUNC,      // trunc，向零舍入 (对应 RZ)
    CAST_ODD,        // Von Neumann rounding，最近邻奇数舍入
    CAST_HYBRID,     // hybrid，特指hif8数据的随机舍入
};
```

##### 建议实现方案
对于需要特定舍入模式的 FMA，可以:
1. 使用 `fmaf` 计算中间结果
2. 通过 `Cast` 接口配合相应的 `RoundMode` 进行舍入调整
3. 或者在更高层次通过软件实现舍入控制

---

## 10. fast_dividef - Fast Floating Point Divide (快速浮点除法)

### libdevice 原型
```c
float fast_dividef(float x, float y);  // 快速但精度较低的除法
```

### AscendC 对应实现

#### C API (A2/A3 系列)
```c++
// 前n个数据计算
__aicore__ inline void asc_div(__ubuf__ half* dst, __ubuf__ half* src0, __ubuf__ half* src1, uint32_t count)
__aicore__ inline void asc_div(__ubuf__ float* dst, __ubuf__ float* src0, __ubuf__ float* src1, uint32_t count)

// 高维切分计算
__aicore__ inline void asc_div(__ubuf__ half* dst, __ubuf__ half* src0, __ubuf__ half* src1, uint8_t repeat,
    uint8_t dst_block_stride, uint8_t src0_block_stride, uint8_t src1_block_stride,
    uint8_t dst_repeat_stride, uint8_t src0_repeat_stride, uint8_t src1_repeat_stride)

// 同步计算
__aicore__ inline void asc_div_sync(__ubuf__ half* dst, __ubuf__ half* src0, __ubuf__ half* src1, uint32_t count)
```
- **支持产品**: Atlas A3、Atlas A2 系列
- **流水类型**: PIPE_V
- **约束**: dst、src0、src1起始地址需要32字节对齐，注意除0错误

#### 高阶 API (Div - 带精度配置)
```c++
template <typename T, const DivConfig& config = DEFAULT_DIV_CONFIG>
__aicore__ inline void Div(const LocalTensor<T>& dst, const LocalTensor<T>& src0,
    const LocalTensor<T>& src1, const int32_t& count)
```

**DivConfig 精度配置**:
```c++
enum class DivAlgo {
    INTRINSIC = 0,           // 单指令计算，最大误差1 ulp（最快）
    DIFF_COMPENSATION,       // 差值补偿算法，最大误差0 ulp
    PRECISION_1ULP_FTZ_TRUE, // 单指令计算，最大误差1 ulp
    PRECISION_0ULP_FTZ_TRUE, // 差值补偿算法，最大误差0 ulp
    PRECISION_0ULP_FTZ_FALSE,// 支持Subnormal，差值补偿，最大误差0 ulp
    PRECISION_1ULP_FTZ_FALSE // 支持Subnormal，单指令，最大误差1 ulp
};
```

**fast_dividef 对应配置**:
```c++
// 快速除法 - 使用 INTRINSIC 模式
static constexpr DivConfig fast_div_config = { DivAlgo::INTRINSIC };
Div<T, fast_div_config>(dst, src0, src1, count);
```

#### 替代方案 (Reciprocal + Mul)
对于快速除法，也可以使用倒数+乘法的组合:
```c++
// 使用 Reciprocal 接口获取倒数，然后乘法
// dst = src0 / src1 = src0 * (1/src1)

// Reciprocal 配置
enum class ReciprocalAlgo {
    INTRINSIC = 0,           // 单指令计算（最快，对应 fast_dividef）
    PRECISION_1ULP_FTZ_TRUE, // 单指令，Subnormal近似为0
    PRECISION_1ULP_FTZ_FALSE,// 支持Subnormal计算
};
```

---

## 总结表

| libdevice 函数 | AscendC C++ API | AscendC C API | 高阶 API | 支持产品 |
|---------------|-----------------|---------------|----------|----------|
| rsqrtf | rsqrtf | asc_rsqrt | Rsqrt | 950: C++ API; A2/A3: C API |
| ceilf | ceilf | asc_ceil | Cast+CEIL | 950: C++ API; 950: C API |
| truncf | truncf | asc_trunc | Cast+TRUNC | 950: C++ API; 950: C API |
| exp2f | exp2f | - | - | 950: C++ API only |
| saturatef | **无直接对应** | **无直接对应** | Max+Min组合 | 需组合实现 |
| fmaf | fmaf | asc_fma | Fma | 950: C++ API; A2/A3: C API |
| fma_rn/rz/rd/ru | **无舍入变体** | **无舍入变体** | fmaf + Cast | 需组合实现 |
| fast_dividef | **无直接对应** | asc_div | Div+INTRINSIC | A2/A3: C API |

---

## 重要注意事项

1. **产品兼容性**: Ascend 950PR/950DT 主要支持 SIMT 风格的 C++ API，而 Atlas A2/A3 系列主要支持 C API。在迁移代码时需要根据目标硬件选择合适的 API。

2. **saturatef 缺失**: 该函数在 AscendC 中没有直接对应，建议使用 `min(max(x, 0.0f), 1.0f)` 组合实现。

3. **FMA 舍入模式**: AscendC 的 FMA 接口没有显式的舍入模式参数，但可以通过 RoundMode 枚举在 Cast 操作中实现类似功能。

4. **fast_dividef**: 在 AscendC 中可以通过 `Div` 接口的 `DivAlgo::INTRINSIC` 模式或 `Reciprocal` + `Mul` 组合实现快速除法。

5. **地址对齐**: C API 和高阶 API 通常要求操作数地址32字节对齐，使用时需注意。
