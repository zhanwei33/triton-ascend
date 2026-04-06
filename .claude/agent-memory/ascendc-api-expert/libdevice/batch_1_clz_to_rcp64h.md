# Libdevice 函数 AscendC 对应实现分析 (Batch 1)

## 概述

本文档分析 10 个 libdevice 函数在 AscendC 中的对应实现方式，涵盖位操作、算术运算和数学函数。

---

## 1. clz - Count Leading Zeros (前导零计数)

### CUDA libdevice 原型
```c
int __clz(int x);           // 32-bit
int __clzll(long long x);   // 64-bit
```

### AscendC 对应实现

#### 标量计算 (C API)
```c++
__aicore__ inline int64_t asc_clz(uint64_t value_in)
```

**头文件**: `scalar_compute.h`

**功能说明**: 计算一个 uint64_t 类型整数在二进制表示下的前导零个数，即从二进制最高位开始，到第一个出现二进制 1 为止，中间连续的 0 的数量。

**参数**:
| 参数名 | 类型 | 描述 |
|--------|------|------|
| value_in | uint64_t | 待统计的数字 |

**返回值**: int64_t，前导 0 的个数

**流水类型**: PIPE_S

**产品支持**:
- Ascend 950PR/Ascend 950DT: √
- Atlas A3 训练/推理系列产品: √
- Atlas A2 训练/推理系列产品: √

**调用示例**:
```c++
uint64_t value_in = 0x0fffffffffffffff;
int64_t ans = asc_clz(value_in); // 返回 ans = 4
```

### 实现建议
对于 32-bit clz，可以将输入转换为 uint64_t 后调用 asc_clz，结果减去 32（如果输入为 0 需特殊处理）。

---

## 2. popc - Population Count (1 的位数统计)

### CUDA libdevice 原型
```c
int __popc(int x);          // 32-bit
int __popcll(long long x);  // 64-bit
```

### AscendC 对应实现

#### 标量计算 (C API)
```c++
__aicore__ inline int64_t asc_popc(uint64_t value)
```

**头文件**: `scalar_compute.h`

**功能说明**: 获取一个 uint64_t 类型数字的二进制中 1 的个数。

**参数**:
| 参数名 | 类型 | 描述 |
|--------|------|------|
| value | uint64_t | 被统计的二进制数字 |

**返回值**: int64_t，二进制中 1 的个数

**流水类型**: PIPE_S

**产品支持**:
- Ascend 950PR/Ascend 950DT: √
- Atlas A3 训练/推理系列产品: √
- Atlas A2 训练/推理系列产品: √

**调用示例**:
```c++
uint64_t scalar = 33;
int64_t count_one = asc_popc(scalar); // 输出 2
```

### 补充相关 API
- `asc_zero_bits_cnt(uint64_t value)`: 统计二进制中 0 的个数

---

## 3. byte_perm - Byte Permutation (字节置换)

### CUDA libdevice 原型
```c
int __byte_perm(int x, int y, int s);
```

### AscendC 对应实现

**状态**: 无直接对应 API

### 实现建议
可以使用以下组合方式实现：

1. **使用 asc_gather 进行字节级收集**:
```c++
__aicore__ inline void asc_gather(
    __ubuf__ uint8_t* dst,
    __ubuf__ uint8_t* src,
    __ubuf__ uint32_t* src_offset,
    uint32_t count
)
```

2. **使用 asc_pack/asc_unpack 进行字节打包/解包** (仅 Ascend 950PR/950DT):
```c++
// 打包操作
__simd_callee__ inline void asc_pack(vector_uint8_t& dst, vector_uint16_t src)
__simd_callee__ inline void asc_pack_v2(vector_uint8_t& dst, vector_uint16_t src)

// 解包操作
__simd_callee__ inline void asc_unpack_upper(vector_uint16_t& dst, vector_uint8_t src)
__simd_callee__ inline void asc_unpack_lower(vector_uint16_t& dst, vector_uint8_t src)
```

### 实现方案
byte_perm 可以通过预先计算偏移量数组，然后使用 asc_gather 按字节收集来实现。

---

## 4. mulhi - Multiply High Bits (高位乘法)

### CUDA libdevice 原型
```c
int __mulhi(int x, int y);          // 32-bit signed
unsigned int __umulhi(unsigned int x, unsigned int y);  // 32-bit unsigned
```

### AscendC 对应实现

#### 寄存器计算 (仅 Ascend 950PR/950DT)
```c++
__simd_callee__ inline void asc_mull(
    vector_int32_t& dst0,
    vector_int32_t& dst1,
    vector_int32_t src0,
    vector_int32_t src1,
    vector_bool mask
)

__simd_callee__ inline void asc_mull(
    vector_uint32_t& dst0,
    vector_uint32_t& dst1,
    vector_uint32_t src0,
    vector_uint32_t src1,
    vector_bool mask
)
```

**头文件**: `cube_compute.h`

**功能说明**: 无符号/有符号整数乘法，将 src0 和 src1 对应元素相乘，结果写入 dst0，溢出部分写入 dst1。

**参数**:
| 参数名 | 类型 | 描述 |
|--------|------|------|
| dst0 | vector_int32_t/vector_uint32_t | 低 32 位结果 |
| dst1 | vector_int32_t/vector_uint32_t | 高 32 位结果 (mulhi 所需) |
| src0 | vector_int32_t/vector_uint32_t | 源操作数 0 |
| src1 | vector_int32_t/vector_uint32_t | 源操作数 1 |
| mask | vector_bool | 掩码寄存器 |

**流水类型**: PIPE_V

**产品支持**:
- Ascend 950PR/Ascend 950DT: √
- Atlas A3/A2: 不支持

### 实现建议
对于 A2/A3 平台，可以使用 64-bit 乘法后右移 32 位来模拟：
```c++
// 使用 64-bit 乘法模拟 mulhi
int64_t result = (int64_t)x * (int64_t)y;
int32_t high = (int32_t)(result >> 32);
```

---

## 5. mul24 - 24-bit Multiply (24 位乘法)

### CUDA libdevice 原型
```c
int __mul24(int x, int y);
unsigned int __umul24(unsigned int x, unsigned int y);
```

### AscendC 对应实现

**状态**: 无直接对应 API

### 实现建议
可以使用标准乘法后掩码实现：
```c++
// 使用 asc_mul 后掩码低 24 位
__aicore__ inline void asc_mul(
    __ubuf__ int32_t* dst,
    __ubuf__ int32_t* src0,
    __ubuf__ int32_t* src1,
    uint32_t count
)

// 结果与 0xFFFFFF 掩码
```

或使用移位操作：
```c++
// 左移 8 位，右移 8 位来清除高 8 位
asc_shiftleft(temp, src0, 8, count);
asc_shiftright(temp, temp, 8, count);
asc_mul(dst, temp, src1, count);
```

---

## 6. brev - Bit Reverse (位反转)

### CUDA libdevice 原型
```c
int __brev(int x);          // 32-bit
long long __brevll(long long x);  // 64-bit
```

### AscendC 对应实现

**状态**: 无直接对应 API

### 实现建议
位反转可以通过查表法或移位组合实现。AscendC 提供了丰富的移位操作：

```c++
// 左移
__aicore__ inline void asc_shiftleft(
    __ubuf__ int32_t* dst,
    __ubuf__ int32_t* src,
    uint32_t distance,
    uint32_t count
)

// 右移
__aicore__ inline void asc_shiftright(
    __ubuf__ int32_t* dst,
    __ubuf__ int32_t* src,
    int32_t value,
    uint32_t count
)
```

### 实现方案
使用分阶段位反转算法：
1. 交换相邻位
2. 交换相邻 2 位组
3. 交换相邻 4 位组
4. 继续直到交换半字

或使用 asc_gather 配合预计算的位偏移表。

---

## 7. sad - Sum of Absolute Differences (绝对差之和)

### CUDA libdevice 原型
```c
int __sad(int x, int y, int z);
```

### AscendC 对应实现

#### 寄存器计算 (仅 Ascend 950PR/950DT)
```c++
template <typename T = DefaultType, MaskMergeMode mode = MaskMergeMode::ZEROING, typename U>
__simd_callee__ inline void AbsSub(
    U& dstReg,
    U& srcReg0,
    U& srcReg1,
    MaskReg& mask
)
```

**功能说明**: srcReg0 与 srcReg1 相减再求绝对值，根据 mask 将计算结果写入 dstReg。

**支持数据类型**: half/float/int64_t

**产品支持**:
- Ascend 950PR/Ascend 950DT: √
- Atlas A3/A2: 不支持

### 实现建议
对于 A2/A3 平台，可以组合实现：
```c++
// 1. 使用 asc_sub 计算差值
// 2. 使用 asc_abs 取绝对值
// 3. 使用 asc_repeat_reduce_sum 或累加求和
```

---

## 8. abs - Absolute Value (绝对值)

### CUDA libdevice 原型
```c
int __abs(int x);
float fabsf(float x);
```

### AscendC 对应实现

#### C API 矢量计算
```c++
// 前 n 个数据计算
__aicore__ inline void asc_abs(__ubuf__ half* dst, __ubuf__ half* src, uint32_t count)
__aicore__ inline void asc_abs(__ubuf__ float* dst, __ubuf__ float* src, uint32_t count)

// 高维切分计算
__aicore__ inline void asc_abs(
    __ubuf__ half* dst,
    __ubuf__ half* src,
    uint8_t repeat,
    uint16_t dst_block_stride,
    uint16_t src_block_stride,
    uint16_t dst_repeat_stride,
    uint16_t src_repeat_stride
)

// 同步计算
__aicore__ inline void asc_abs_sync(__ubuf__ half* dst, __ubuf__ half* src, uint32_t count)
```

**头文件**: `vector_compute.h`

**流水类型**: PIPE_V

**产品支持**:
- Atlas A3 训练/推理系列产品: √
- Atlas A2 训练/推理系列产品: √

#### 高级 API (Tensor 操作)
```c++
template <typename T>
__aicore__ inline void Abs(
    const LocalTensor<T>& dst,
    const LocalTensor<T>& src,
    const int32_t& count
)
```

**支持数据类型**:
- Ascend 950PR/950DT: int8_t, int16_t, half, int32_t, float, int64_t
- Atlas A3/A2: half, float

**调用示例**:
```c++
// C API 方式
__ubuf__ half src[128];
__ubuf__ half dst[128];
asc_abs(dst, src, 128);

// 高级 API 方式
AscendC::Abs(dstLocal, srcLocal, 512);
```

---

## 9. floor - Floor Function (向下取整)

### CUDA libdevice 原型
```c
float floorf(float x);
double floor(double x);
```

### AscendC 对应实现

#### 高级 API (Tensor 操作)
```c++
// 通过 sharedTmpBuffer 入参传入临时空间
template <typename T, bool isReuseSource = false>
__aicore__ inline void Floor(
    const LocalTensor<T>& dstTensor,
    const LocalTensor<T>& srcTensor,
    const LocalTensor<uint8_t>& sharedTmpBuffer,
    const uint32_t calCount
)

// 接口框架申请临时空间
template <typename T, bool isReuseSource = false>
__aicore__ inline void Floor(
    const LocalTensor<T>& dstTensor,
    const LocalTensor<T>& srcTensor,
    const uint32_t calCount
)
```

**功能说明**: 获取小于或等于 x 的最小的整数值，即向负无穷取整操作。

**示例**:
- Floor(3.9) = 3.0
- Floor(-3.9) = -4.0

**支持数据类型**: half, float

**产品支持**:
- Ascend 950PR/Ascend 950DT: √
- Atlas A3 训练/推理系列产品: √
- Atlas A2 训练/推理系列产品: √
- Kirin X90: √
- Kirin 9030: √

**临时空间获取**:
```c++
// 通过 GetFloorMaxMinTmpSize 获取临时空间大小
```

**调用示例**:
```c++
AscendC::TPipe pipe;
AscendC::TQue<AscendC::TPosition::VECCALC, 1> tmpQue;
pipe.InitBuffer(tmpQue, 1, bufferSize);
AscendC::LocalTensor<uint8_t> sharedTmpBuffer = tmpQue.AllocTensor<uint8_t>();
AscendC::Floor(dstLocal, srcLocal, sharedTmpBuffer, 512);
```

---

## 10. rcp64h - Reciprocal 64-bit High Precision (64 位高精度倒数)

### CUDA libdevice 原型
```c
double rcp64h(double x);  // 高精度倒数
```

### AscendC 对应实现

#### C API 矢量计算
```c++
// 前 n 个数据计算
__aicore__ inline void asc_rcp(__ubuf__ half* dst, __ubuf__ half* src, uint32_t count)
__aicore__ inline void asc_rcp(__ubuf__ float* dst, __ubuf__ float* src, uint32_t count)

// 同步计算
__aicore__ inline void asc_rcp_sync(__ubuf__ half* dst, __ubuf__ half* src, uint32_t count)
__aicore__ inline void asc_rcp_sync(__ubuf__ float* dst, __ubuf__ float* src, uint32_t count)
```

**头文件**: `vector_compute.h`

**功能说明**: 执行矢量的取倒数运算: dst_i = 1 / src_i

**流水类型**: PIPE_V

**产品支持**:
- Atlas A3 训练/推理系列产品: √
- Atlas A2 训练/推理系列产品: √

#### 高级 API (Tensor 操作)
```c++
template <typename T, const ReciprocalConfig& config = DEFAULT_RECIPROCAL_CONFIG>
__aicore__ inline void Reciprocal(
    const LocalTensor<T>& dst,
    const LocalTensor<T>& src,
    const int32_t& count
)
```

**精度配置**:
```c++
enum class ReciprocalAlgo {
    INTRINSIC = 0,              // 单指令计算，Subnormal 近似为 0
    PRECISION_1ULP_FTZ_TRUE,    // 单指令计算，Subnormal 近似为 0
    PRECISION_1ULP_FTZ_FALSE,   // 支持 Subnormal 数据计算
};

struct ReciprocalConfig {
    ReciprocalAlgo algo = ReciprocalAlgo::INTRINSIC;
};
```

**产品支持**:
- Ascend 950PR/Ascend 950DT: half, float, int64_t, uint64_t
- Atlas A3/A2: half, float
- Kirin X90/9030: half, float

**注意**:
- half 的算子结果对比误差不满足双千分之一的要求
- float 的算子结果对比误差不满足双万分之一的要求
- 如果需要高精度，建议使用 Div 替代实现

**调用示例**:
```c++
// C API 方式
__ubuf__ half src[128];
__ubuf__ half dst[128];
asc_rcp(dst, src, 128);

// 高级 API 方式
AscendC::Reciprocal(dstLocal, srcLocal, 512);

// 高精度配置
static constexpr ReciprocalConfig config = { ReciprocalAlgo::PRECISION_1ULP_FTZ_FALSE };
AscendC::Reciprocal<T, config>(dstLocal, srcLocal, 512);
```

---

## 总结表

| libdevice 函数 | AscendC 对应 API | 支持平台 | 备注 |
|----------------|------------------|----------|------|
| clz | `asc_clz` | 950/A3/A2 | 标量操作，仅支持 uint64_t |
| popc | `asc_popc` | 950/A3/A2 | 标量操作，仅支持 uint64_t |
| byte_perm | 无直接对应 | - | 可用 asc_gather/asc_pack 组合实现 |
| mulhi | `asc_mull` | 仅 950 | 寄存器操作，A2/A3 需用 64-bit 乘法模拟 |
| mul24 | 无直接对应 | - | 可用 asc_mul + 掩码实现 |
| brev | 无直接对应 | - | 需用移位操作组合实现 |
| sad | `AbsSub` | 仅 950 | A2/A3 需用 sub+abs+sum 组合实现 |
| abs | `asc_abs` / `Abs` | 950/A3/A2 | 矢量/Tensor 操作，支持多种数据类型 |
| floor | `Floor` | 950/A3/A2/X90/9030 | Tensor 操作，需要临时空间 |
| rcp64h | `asc_rcp` / `Reciprocal` | 950/A3/A2 | 支持高精度配置 |

---

## 附录：相关头文件路径

```
asc-devkit/docs/api/context/c_api/scalar_compute/asc_clz.md
asc-devkit/docs/api/context/c_api/scalar_compute/asc_popc.md
asc-devkit/docs/api/context/c_api/vector_compute/asc_abs.md
asc-devkit/docs/api/context/c_api/vector_compute/asc_rcp.md
asc-devkit/docs/api/context/c_api/vector_compute/asc_shiftleft.md
asc-devkit/docs/api/context/c_api/vector_compute/asc_shiftright.md
asc-devkit/docs/api/context/c_api/vector_compute/asc_mul.md
asc-devkit/docs/api/context/c_api/vector_compute/asc_gather.md
asc-devkit/docs/api/context/c_api/reg/reg_vector/asc_mull.md
asc-devkit/docs/api/context/c_api/reg/reg_vector/asc_pack.md
asc-devkit/docs/api/context/c_api/reg/reg_vector/asc_unpack.md
asc-devkit/docs/api/context/Abs.md
asc-devkit/docs/api/context/Floor.md
asc-devkit/docs/api/context/Reciprocal.md
asc-devkit/docs/api/context/AbsSub.md
```
