# Libdevice 到 AscendC API 对应分析 - Batch 6

## 概述

本文档分析 10 个 libdevice 类型转换函数在 AscendC 中的对应实现方式。

**重要发现**：
- **仅支持 Ascend 950PR/Ascend 950DT**：CUDA SIMT 风格的转换函数（如 `__float2int_rn` 等）仅在此平台支持
- **A2/A3 平台**：需要使用 `Cast` 或 `ScalarCast` API 配合 `RoundMode` 实现类似功能
- **Double 类型转换**：AscendC 不直接提供 `double2uint` 系列函数，需要通过 `Cast` API 实现

---

## 1. double2uint_rz - Double to uint round toward zero

### Libdevice 函数
```cuda
unsigned int double2uint_rz(double x);  // 向零舍入
```

### AscendC 对应实现

**方案一：使用 Cast API（推荐，A2/A3/950 均支持）**
```cpp
#include "ascendc.h"

// 使用 Cast API 进行向量转换
// RoundMode::CAST_TRUNC = 向零舍入
AscendC::Cast<uint32_t, double, AscendC::RoundMode::CAST_TRUNC>(dstReg, srcReg, mask);
```

**方案二：使用 ScalarCast（标量转换）**
```cpp
#include "ascendc.h"

// 标量转换
uint32_t result = AscendC::ScalarCast<double, uint32_t, AscendC::RoundMode::CAST_TRUNC>(value);
```

### 产品支持情况

| 产品 | 支持情况 |
|------|----------|
| Ascend 950PR/Ascend 950DT | 通过 Cast API 支持 |
| Atlas A3 训练/推理 | 通过 Cast API 支持 |
| Atlas A2 训练/推理 | 通过 Cast API 支持 |

### 函数原型
```cpp
// Cast API（向量转换）
template <typename T, typename U, const CastTrait& trait, typename S, typename V>
__simd_callee__ inline void Cast(S& dstReg, V& srcReg, MaskReg& mask);

// ScalarCast（标量转换）
template <typename T, typename U, RoundMode roundMode>
__aicore__ inline U ScalarCast(T valueIn);
```

### 特殊值处理

| 输入场景 | 返回值 |
|----------|--------|
| NaN | 0 |
| +Inf | UINT32_MAX (4294967295) |
| -Inf | 0 |

---

## 2. double2uint_rd - Double to uint round downward

### Libdevice 函数
```cuda
unsigned int double2uint_rd(double x);  // 向下取整（floor）
```

### AscendC 对应实现

**使用 Cast API（推荐）**
```cpp
#include "ascendc.h"

// RoundMode::CAST_FLOOR = 向下取整
AscendC::Cast<uint32_t, double, AscendC::RoundMode::CAST_FLOOR>(dstReg, srcReg, mask);
```

**使用 ScalarCast（标量）**
```cpp
uint32_t result = AscendC::ScalarCast<double, uint32_t, AscendC::RoundMode::CAST_FLOOR>(value);
```

### RoundMode 说明
- `CAST_FLOOR`：向负无穷方向舍入（floor 模式）
- 对于正数，效果与向零舍入相同
- 对于负数，向更小的方向舍入

---

## 3. double2uint_ru - Double to uint round upward

### Libdevice 函数
```cuda
unsigned int double2uint_ru(double x);  // 向上取整（ceil）
```

### AscendC 对应实现

**使用 Cast API（推荐）**
```cpp
#include "ascendc.h"

// RoundMode::CAST_CEIL = 向上取整
AscendC::Cast<uint32_t, double, AscendC::RoundMode::CAST_CEIL>(dstReg, srcReg, mask);
```

**使用 ScalarCast（标量）**
```cpp
uint32_t result = AscendC::ScalarCast<double, uint32_t, AscendC::RoundMode::CAST_CEIL>(value);
```

### RoundMode 说明
- `CAST_CEIL`：向正无穷方向舍入（ceil 模式）
- 对于负数，效果与向零舍入相同
- 对于正数，向更大的方向舍入

---

## 4. int2double_rn - Int to double round to nearest

### Libdevice 函数
```cuda
double int2double_rn(int x);  // 整数转 double，最近舍入
```

### AscendC 对应实现

**使用 Cast API（推荐）**
```cpp
#include "ascendc.h"

// 整数转浮点不需要舍入模式，使用 UNKNOWN
AscendC::Cast<double, int32_t>(dstReg, srcReg, mask);
```

**使用 ScalarCast（标量）**
```cpp
double result = AscendC::ScalarCast<int32_t, double>(value);
```

### 说明
- 整数转浮点（int32_t -> double）是精确转换
- 不需要指定 RoundMode
- AscendC 支持 int32_t/uint32_t/int64_t/uint64_t 到 double 的转换

---

## 5. uint2double_rn - Uint to double round to nearest

### Libdevice 函数
```cuda
double uint2double_rn(unsigned int x);  // 无符号整数转 double
```

### AscendC 对应实现

**使用 Cast API（推荐）**
```cpp
#include "ascendc.h"

// uint32_t 转 double
AscendC::Cast<double, uint32_t>(dstReg, srcReg, mask);
```

**使用 ScalarCast（标量）**
```cpp
double result = AscendC::ScalarCast<uint32_t, double>(value);
```

---

## 6. float2int_rn - Float to int round to nearest

### Libdevice 函数
```cuda
int float2int_rn(float x);  // 四舍五入到最接近的偶数
```

### AscendC 对应实现

**方案一：使用 SIMT API（仅 Ascend 950 支持）**
```cpp
#include "simt_api/device_functions.h"

__simt_vf__ __launch_bounds__(1024) inline void kernel(__gm__ int32_t* dst, __gm__ float* x)
{
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    dst[idx] = __float2int_rn(x[idx]);  // 四舍五入到最接近的偶数
}
```

**方案二：使用 Cast API（A2/A3/950 均支持）**
```cpp
#include "ascendc.h"

// RoundMode::CAST_RINT = 四舍六入五成双
AscendC::Cast<int32_t, float, AscendC::RoundMode::CAST_RINT>(dstReg, srcReg, mask);
```

**使用 ScalarCast（标量）**
```cpp
int32_t result = AscendC::ScalarCast<float, int32_t, AscendC::RoundMode::CAST_RINT>(value);
```

### 产品支持情况

| 产品 | __float2int_rn | Cast API |
|------|----------------|----------|
| Ascend 950PR/Ascend 950DT | 支持 | 支持 |
| Atlas A3 训练/推理 | 不支持 | 支持 |
| Atlas A2 训练/推理 | 不支持 | 支持 |

### 函数原型
```cpp
__simt_callee__ inline int __float2int_rn(const float x)
```

### 特殊值处理

| 输入场景 | 返回值 |
|----------|--------|
| NaN | 0 |
| +Inf | INT32_MAX (2147483647) |
| -Inf | INT32_MIN (-2147483648) |

---

## 7. float2int_rz - Float to int round toward zero

### Libdevice 函数
```cuda
int float2int_rz(float x);  // 向零舍入（截断）
```

### AscendC 对应实现

**方案一：使用 SIMT API（仅 Ascend 950 支持）**
```cpp
#include "simt_api/device_functions.h"

dst[idx] = __float2int_rz(x[idx]);
```

**方案二：使用 Cast API（推荐，全平台支持）**
```cpp
#include "ascendc.h"

// RoundMode::CAST_TRUNC = 向零舍入
AscendC::Cast<int32_t, float, AscendC::RoundMode::CAST_TRUNC>(dstReg, srcReg, mask);
```

**使用 ScalarCast（标量）**
```cpp
int32_t result = AscendC::ScalarCast<float, int32_t, AscendC::RoundMode::CAST_TRUNC>(value);
```

### 函数原型
```cpp
__simt_callee__ inline int __float2int_rz(const float x)
```

---

## 8. float2int_rd - Float to int round downward

### Libdevice 函数
```cuda
int float2int_rd(float x);  // 向下取整（floor）
```

### AscendC 对应实现

**方案一：使用 SIMT API（仅 Ascend 950 支持）**
```cpp
#include "simt_api/device_functions.h"

dst[idx] = __float2int_rd(x[idx]);
```

**方案二：使用 Cast API（推荐，全平台支持）**
```cpp
#include "ascendc.h"

// RoundMode::CAST_FLOOR = 向下取整
AscendC::Cast<int32_t, float, AscendC::RoundMode::CAST_FLOOR>(dstReg, srcReg, mask);
```

**使用 ScalarCast（标量）**
```cpp
int32_t result = AscendC::ScalarCast<float, int32_t, AscendC::RoundMode::CAST_FLOOR>(value);
```

### 函数原型
```cpp
__simt_callee__ inline int __float2int_rd(const float x)
```

---

## 9. float2int_ru - Float to int round upward

### Libdevice 函数
```cuda
int float2int_ru(float x);  // 向上取整（ceil）
```

### AscendC 对应实现

**方案一：使用 SIMT API（仅 Ascend 950 支持）**
```cpp
#include "simt_api/device_functions.h"

dst[idx] = __float2int_ru(x[idx]);
```

**方案二：使用 Cast API（推荐，全平台支持）**
```cpp
#include "ascendc.h"

// RoundMode::CAST_CEIL = 向上取整
AscendC::Cast<int32_t, float, AscendC::RoundMode::CAST_CEIL>(dstReg, srcReg, mask);
```

**使用 ScalarCast（标量）**
```cpp
int32_t result = AscendC::ScalarCast<float, int32_t, AscendC::RoundMode::CAST_CEIL>(value);
```

### 函数原型
```cpp
__simt_callee__ inline int __float2int_ru(const float x)
```

---

## 10. float2uint_rn - Float to uint round to nearest

### Libdevice 函数
```cuda
unsigned int float2uint_rn(float x);  // 四舍五入到最接近的偶数
```

### AscendC 对应实现

**方案一：使用 SIMT API（仅 Ascend 950 支持）**
```cpp
#include "simt_api/device_functions.h"

dst[idx] = __float2uint_rn(x[idx]);
```

**方案二：使用 Cast API（推荐，全平台支持）**
```cpp
#include "ascendc.h"

// RoundMode::CAST_RINT = 四舍六入五成双
AscendC::Cast<uint32_t, float, AscendC::RoundMode::CAST_RINT>(dstReg, srcReg, mask);
```

**使用 ScalarCast（标量）**
```cpp
uint32_t result = AscendC::ScalarCast<float, uint32_t, AscendC::RoundMode::CAST_RINT>(value);
```

### 函数原型
```cpp
__simt_callee__ inline unsigned int __float2uint_rn(const float x)
```

### 特殊值处理

| 输入场景 | 返回值 |
|----------|--------|
| NaN | 0 |
| +Inf | UINT32_MAX (4294967295) |
| -Inf | 0 |

---

## RoundMode 枚举详解

```cpp
enum class RoundMode {
    CAST_NONE = 0,   // 在转换有精度损失时表示CAST_RINT模式，不涉及精度损失时表示不舍入
    CAST_RINT,       // rint，四舍六入五成双舍入（round to nearest even）
    CAST_FLOOR,      // floor，向负无穷舍入（round downward）
    CAST_CEIL,       // ceil，向正无穷舍入（round upward）
    CAST_ROUND,      // round，四舍五入舍入
    CAST_TRUNC,      // trunc，向零舍入（round toward zero）
    CAST_ODD,        // Von Neumann rounding，最近邻奇数舍入
    CAST_HYBRID,     // hybrid，目前特指输出结果是hif8数据时，会用到的一种随机舍入
};
```

### 映射关系

| Libdevice 后缀 | RoundMode 枚举值 | 说明 |
|----------------|------------------|------|
| _rn (round nearest) | CAST_RINT | 四舍六入五成双 |
| _rz (round zero) | CAST_TRUNC | 向零舍入 |
| _rd (round down) | CAST_FLOOR | 向下取整 |
| _ru (round up) | CAST_CEIL | 向上取整 |

---

## 头文件要求

### SIMT API（仅 Ascend 950）
```cpp
#include "simt_api/device_functions.h"
```

### Cast API（全平台）
```cpp
#include "ascendc.h"
```

---

## 总结与建议

### 平台兼容性建议

| 目标平台 | 推荐 API | 说明 |
|----------|----------|------|
| Ascend 950PR/950DT | SIMT API 或 Cast API | SIMT API 更贴近 CUDA 风格 |
| Atlas A2/A3 | Cast API / ScalarCast | 唯一选择，功能完整 |
| 跨平台代码 | Cast API / ScalarCast | 保证兼容性 |

### Double 类型转换注意事项

1. **AscendC 支持 double 类型**，但仅作为基础数据类型支持
2. **没有专门的 double2uint 系列函数**，需要通过 `Cast` API 实现
3. **转换时需要注意舍入模式的选择**，根据业务需求选择合适的 RoundMode

### 最佳实践

```cpp
// 示例：跨平台的 float 到 int 转换（向零舍入）
template<typename T>
__aicore__ inline int32_t FloatToInt(T value) {
#ifdef USE_SIMT_API
    // 仅 Ascend 950
    return __float2int_rz(value);
#else
    // 全平台支持
    return AscendC::ScalarCast<float, int32_t, AscendC::RoundMode::CAST_TRUNC>(value);
#endif
}
```

---

## 参考文档

- `/gemini/code/huawei/asc-devkit/docs/api/context/__float2int_rn.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/__float2int_rz.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/__float2int_rd.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/__float2int_ru.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/__float2uint_rn.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/__float2uint_rz.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/__float2uint_rd.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/__float2uint_ru.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/Cast-46.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/ScalarCast.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/RoundMode.md`
- `/gemini/code/huawei/asc-devkit/docs/api/context/内置数据类型.md`
