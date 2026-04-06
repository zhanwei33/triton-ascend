# Libdevice 到 AscendC API 对应分析 - Batch 9

## 分析函数列表

1. float2ull_rd - Float to unsigned long long round downward
2. float2ull_ru - Float to unsigned long long round upward
3. double2ll_rn - Double to long long round to nearest
4. double2ll_rz - Double to long long round toward zero
5. double2ll_rd - Double to long long round downward
6. double2ll_ru - Double to long long round upward
7. double2ull_rn - Double to unsigned long long round to nearest
8. double2ull_rz - Double to unsigned long long round toward zero
9. double2ull_rd - Double to unsigned long long round downward
10. double2ull_ru - Double to unsigned long long round upward

---

## 1. float2ull_rd

### Libdevice 描述
Float to unsigned long long round downward（向下取整）

### AscendC 对应实现

**SIMT API:** `__float2ull_rd`

**函数原型:**
```cpp
__simt_callee__ inline unsigned long long int __float2ull_rd(const float x)
```

**头文件:**
```cpp
#include "simt_api/device_functions.h"
```

**功能说明:**
将浮点数转换为向下取整的64位无符号整数。

**参数:**
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| x | 输入 | 源操作数 |

**返回值:**
- 正常情况：向下取整的64位无符号整数
- x为nan时：返回0
- x为inf时：返回18446744073709551615 (ULLONG_MAX)
- x为-inf时：返回0

**产品支持:**
| 产品 | 是否支持 |
|------|----------|
| Ascend 950PR/Ascend 950DT | √ |
| Atlas A3 训练/推理系列产品 | x |
| Atlas A2 训练/推理系列产品 | x |

**使用示例:**
```cpp
__simt_vf__ __launch_bounds__(1024) inline void kernel__float2ull_rd(__gm__ uint64_t* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = __float2ull_rd(x[idx]);
}
```

---

## 2. float2ull_ru

### Libdevice 描述
Float to unsigned long long round upward（向上取整）

### AscendC 对应实现

**SIMT API:** `__float2ull_ru`

**函数原型:**
```cpp
__simt_callee__ inline unsigned long long int __float2ull_ru(const float x)
```

**头文件:**
```cpp
#include "simt_api/device_functions.h"
```

**功能说明:**
将浮点数转换为向上取整的64位无符号整数。

**参数:**
| 参数名 | 输入/输出 | 描述 |
|--------|-----------|------|
| x | 输入 | 源操作数 |

**返回值:**
- 正常情况：向上取整的64位无符号整数
- x为nan时：返回0
- x为inf时：返回18446744073709551615 (ULLONG_MAX)
- x为-inf时：返回0

**产品支持:**
| 产品 | 是否支持 |
|------|----------|
| Ascend 950PR/Ascend 950DT | √ |
| Atlas A3 训练/推理系列产品 | x |
| Atlas A2 训练/推理系列产品 | x |

**使用示例:**
```cpp
__simt_vf__ __launch_bounds__(1024) inline void kernel__float2ull_ru(__gm__ uint64_t* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = __float2ull_ru(x[idx]);
}
```

---

## 3. double2ll_rn

### Libdevice 描述
Double to long long round to nearest（四舍五入到最接近的偶数）

### AscendC 对应实现

**重要说明:** AscendC 中没有直接的 SIMT API 实现 `__double2ll_rn`。需要使用 `Cast` 指令来实现。

**替代方案: Cast 指令**

**函数原型:**
```cpp
// tensor前n个数据计算
template <typename T, typename U>
__aicore__ inline void Cast(const LocalTensor<T>& dst, const LocalTensor<U>& src, const RoundMode& roundMode, const uint32_t count)

// tensor高维切分计算 - mask连续模式
template <typename T, typename U, bool isSetMask = true>
__aicore__ inline void Cast(const LocalTensor<T>& dst, const LocalTensor<U>& src, const RoundMode& roundMode, const uint64_t mask, const uint8_t repeatTime, const UnaryRepeatParams& repeatParams)
```

**头文件:**
```cpp
#include "kernel_operator.h"
```

**RoundMode 枚举:**
```cpp
enum class RoundMode {
    CAST_NONE = 0,  // 在转换有精度损失时表示CAST_RINT模式，不涉及精度损失时表示不舍入
    CAST_RINT,      // rint，四舍六入五成双舍入（对应rn模式）
    CAST_FLOOR,     // floor，向负无穷舍入（对应rd模式）
    CAST_CEIL,      // ceil，向正无穷舍入（对应ru模式）
    CAST_ROUND,     // round，四舍五入舍入
    CAST_TRUNC,     // trunc，向零舍入（对应rz模式）
    CAST_ODD,       // Von Neumann rounding，最近邻奇数舍入
    CAST_HYBRID,    // hybrid，随机舍入
};
```

**对应关系:**
| Libdevice | AscendC Cast RoundMode |
|-----------|------------------------|
| double2ll_rn | CAST_RINT |
| double2ll_rz | CAST_TRUNC |
| double2ll_rd | CAST_FLOOR |
| double2ll_ru | CAST_CEIL |

**使用示例:**
```cpp
#include "kernel_operator.h"
using namespace AscendC;

// double -> int64_t (long long) with round to nearest
__aicore__ inline void DoubleToLongLongRn(LocalTensor<int64_t>& dst, LocalTensor<double>& src, uint32_t count)
{
    Cast(dst, src, RoundMode::CAST_RINT, count);
}
```

**产品支持:**
| 产品 | 是否支持 |
|------|----------|
| Ascend 950PR/Ascend 950DT | √ |
| Atlas A3 训练/推理系列产品 | √ |
| Atlas A2 训练/推理系列产品 | √ |
| Kirin X90 | √ |
| Kirin 9030 | √ |

**约束说明:**
- double -> int64_t 转换仅支持 tensor 前n个数据计算接口

---

## 4. double2ll_rz

### Libdevice 描述
Double to long long round toward zero（向零取整）

### AscendC 对应实现

**重要说明:** 使用 `Cast` 指令，RoundMode 设置为 `CAST_TRUNC`

**使用示例:**
```cpp
#include "kernel_operator.h"
using namespace AscendC;

// double -> int64_t (long long) with round toward zero
__aicore__ inline void DoubleToLongLongRz(LocalTensor<int64_t>& dst, LocalTensor<double>& src, uint32_t count)
{
    Cast(dst, src, RoundMode::CAST_TRUNC, count);
}
```

**产品支持:** 同 double2ll_rn

---

## 5. double2ll_rd

### Libdevice 描述
Double to long long round downward（向下取整）

### AscendC 对应实现

**重要说明:** 使用 `Cast` 指令，RoundMode 设置为 `CAST_FLOOR`

**使用示例:**
```cpp
#include "kernel_operator.h"
using namespace AscendC;

// double -> int64_t (long long) with round downward (floor)
__aicore__ inline void DoubleToLongLongRd(LocalTensor<int64_t>& dst, LocalTensor<double>& src, uint32_t count)
{
    Cast(dst, src, RoundMode::CAST_FLOOR, count);
}
```

**产品支持:** 同 double2ll_rn

---

## 6. double2ll_ru

### Libdevice 描述
Double to long long round upward（向上取整）

### AscendC 对应实现

**重要说明:** 使用 `Cast` 指令，RoundMode 设置为 `CAST_CEIL`

**使用示例:**
```cpp
#include "kernel_operator.h"
using namespace AscendC;

// double -> int64_t (long long) with round upward (ceil)
__aicore__ inline void DoubleToLongLongRu(LocalTensor<int64_t>& dst, LocalTensor<double>& src, uint32_t count)
{
    Cast(dst, src, RoundMode::CAST_CEIL, count);
}
```

**产品支持:** 同 double2ll_rn

---

## 7. double2ull_rn

### Libdevice 描述
Double to unsigned long long round to nearest（四舍五入到最接近的偶数）

### AscendC 对应实现

**重要说明:** AscendC 中没有直接的 SIMT API 实现。需要使用 `Cast` 指令。

**约束说明:**
根据 Cast 指令文档，目前支持 double -> int64_t 转换，但未明确列出 double -> uint64_t (unsigned long long) 的直接支持。

**可能的实现方案:**
1. 先使用 Cast 将 double 转换为 int64_t，然后转换为 uint64_t
2. 检查输入值范围，确保非负后再进行转换

**使用示例（需要验证）:**
```cpp
#include "kernel_operator.h"
using namespace AscendC;

// double -> uint64_t (unsigned long long) with round to nearest
// 注意：需要先确保输入值为非负数
__aicore__ inline void DoubleToULongLongRn(LocalTensor<uint64_t>& dst, LocalTensor<double>& src, uint32_t count)
{
    // 方案1：如果硬件支持直接的 double->uint64_t 转换
    // Cast(dst, src, RoundMode::CAST_RINT, count);

    // 方案2：先转int64再转uint64（需要确保值在有效范围内）
    LocalTensor<int64_t> tempInt64;
    Cast(tempInt64, src, RoundMode::CAST_RINT, count);
    // 然后类型转换为uint64_t
}
```

---

## 8. double2ull_rz

### Libdevice 描述
Double to unsigned long long round toward zero（向零取整）

### AscendC 对应实现

**重要说明:** 使用 `Cast` 指令，RoundMode 设置为 `CAST_TRUNC`

**使用示例:**
```cpp
#include "kernel_operator.h"
using namespace AscendC;

// double -> uint64_t (unsigned long long) with round toward zero
__aicore__ inline void DoubleToULongLongRz(LocalTensor<uint64_t>& dst, LocalTensor<double>& src, uint32_t count)
{
    Cast(dst, src, RoundMode::CAST_TRUNC, count);
}
```

---

## 9. double2ull_rd

### Libdevice 描述
Double to unsigned long long round downward（向下取整）

### AscendC 对应实现

**重要说明:** 使用 `Cast` 指令，RoundMode 设置为 `CAST_FLOOR`

**使用示例:**
```cpp
#include "kernel_operator.h"
using namespace AscendC;

// double -> uint64_t (unsigned long long) with round downward (floor)
__aicore__ inline void DoubleToULongLongRd(LocalTensor<uint64_t>& dst, LocalTensor<double>& src, uint32_t count)
{
    Cast(dst, src, RoundMode::CAST_FLOOR, count);
}
```

---

## 10. double2ull_ru

### Libdevice 描述
Double to unsigned long long round upward（向上取整）

### AscendC 对应实现

**重要说明:** 使用 `Cast` 指令，RoundMode 设置为 `CAST_CEIL`

**使用示例:**
```cpp
#include "kernel_operator.h"
using namespace AscendC;

// double -> uint64_t (unsigned long long) with round upward (ceil)
__aicore__ inline void DoubleToULongLongRu(LocalTensor<uint64_t>& dst, LocalTensor<double>& src, uint32_t count)
{
    Cast(dst, src, RoundMode::CAST_CEIL, count);
}
```

---

## 总结

### 函数对应表

| Libdevice 函数 | AscendC 实现 | 实现方式 | 支持产品 |
|----------------|--------------|----------|----------|
| float2ull_rd | `__float2ull_rd` | SIMT API | Ascend 950PR/950DT |
| float2ull_ru | `__float2ull_ru` | SIMT API | Ascend 950PR/950DT |
| double2ll_rn | `Cast` + `CAST_RINT` | Cast 指令 | A2/A3/950PR/950DT/Kirin |
| double2ll_rz | `Cast` + `CAST_TRUNC` | Cast 指令 | A2/A3/950PR/950DT/Kirin |
| double2ll_rd | `Cast` + `CAST_FLOOR` | Cast 指令 | A2/A3/950PR/950DT/Kirin |
| double2ll_ru | `Cast` + `CAST_CEIL` | Cast 指令 | A2/A3/950PR/950DT/Kirin |
| double2ull_rn | `Cast` + `CAST_RINT` | Cast 指令 | A2/A3/950PR/950DT/Kirin |
| double2ull_rz | `Cast` + `CAST_TRUNC` | Cast 指令 | A2/A3/950PR/950DT/Kirin |
| double2ull_rd | `Cast` + `CAST_FLOOR` | Cast 指令 | A2/A3/950PR/950DT/Kirin |
| double2ull_ru | `Cast` + `CAST_CEIL` | Cast 指令 | A2/A3/950PR/950DT/Kirin |

### 重要注意事项

1. **float2ull_rd/float2ull_ru**: 仅支持 Ascend 950PR/950DT 产品，A2/A3 不支持

2. **double 转换**: 所有 double 到 long long/unsigned long long 的转换都需要使用 `Cast` 指令，没有直接的 SIMT API 实现

3. **double 转换限制**: double -> int64_t 转换仅支持 tensor 前n个数据计算接口，不支持高维切分计算接口

4. **RoundMode 映射:**
   - rn (round to nearest even) -> CAST_RINT
   - rz (round toward zero) -> CAST_TRUNC
   - rd (round downward/floor) -> CAST_FLOOR
   - ru (round upward/ceil) -> CAST_CEIL

5. **溢出处理**: Cast 指令在转换时溢出默认按照饱和处理

6. **特殊值处理差异**:
   - SIMT API (__float2ull_*) 对 nan/inf/-inf 有明确的返回值定义
   - Cast 指令文档中未明确说明对 nan/inf 的处理方式，需要实际测试验证
