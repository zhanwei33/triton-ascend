# Libdevice to AscendC API 映射分析汇总

## 分析概况

- **总函数数**: 197个
- **分析批次**: 20批
- **完成时间**: 2026-03-31
- **分析文件位置**: `/gemini/code/huawei/triton-ascend/.claude/agent-memory/ascendc-api-expert/libdevice/`

## 批次列表

| 批次 | 函数范围 | 文件 |
|-----|---------|------|
| 1 | clz ~ rcp64h | batch_1_clz_to_rcp64h.md |
| 2 | rsqrt ~ fast_dividef | batch_2_rsqrt_to_fast_dividef.md |
| 3 | div_rn ~ sqrt_rz | batch_3_div_rn_to_sqrt_rz.md |
| 4 | sqrt_rd ~ mul_rd | batch_4_sqrt_rd_to_mul_rd.md |
| 5 | mul_ru ~ double2uint_rn | batch_5_mul_ru_to_double2uint_rn.md |
| 6 | double2uint_rz ~ float2uint_rn | batch_6_double2uint_rz_to_float2uint_rn.md |
| 7 | float2uint_rz ~ uint2float_rd | batch_7_float2uint_rz_to_uint2float_rd.md |
| 8 | uint2float_ru ~ float2ull_rz | batch_8_uint2float_ru_to_float2ull_rz.md |
| 9 | float2ull_rd ~ double2ull_ru | batch_9_float2ull_rd_to_double2ull_ru.md |
| 10 | ll2float_rn ~ ll2double_rz | batch_10_ll2float_rn_to_ll2double_rz.md |
| 11 | ll2double_rd ~ float_as_uint | batch_11_ll2double_rd_to_float_as_uint.md |
| 12 | longlong_as_double ~ fast_log10f | batch_12_longlong_as_double_to_fast_log10f.md |
| 13 | fast_powf ~ rint | batch_13_fast_powf_to_rint.md |
| 14 | llrint ~ cos | batch_14_llrint_to_cos.md |
| 15 | sinpi ~ atan2 | batch_15_sinpi_to_atan2.md |
| 16 | atan ~ expm1 | batch_16_atan_to_expm1.md |
| 17 | hypot ~ j1 | batch_17_hypot_to_j1.md |
| 18 | y0 ~ erfcx | batch_18_y0_to_erfcx.md |
| 19 | erfcinv ~ pow | batch_19_erfcinv_to_pow.md |
| 20 | tgamma ~ isfinited | batch_20_tgamma_to_isfinited.md |

## 关键发现

### 一、完全支持的函数类别

#### 1. 基础数学运算 (直接支持)
- `abs`, `floor`, `ceil`, `trunc`, `round`
- `sqrt`, `rsqrt`, `cbrt`, `rcbrt`
- `exp`, `exp2`, `exp10`, `expm1`
- `log`, `log2`, `log10`, `log1p`
- `pow`

#### 2. 三角函数 (直接支持)
- `sin`, `cos`, `tan`
- `sinpi`, `cospi`
- `asin`, `acos`, `atan`, `atan2`
- `sinh`, `cosh`, `tanh`
- `asinh`, `acosh`, `atanh`

#### 3. 特殊函数 (直接支持)
- `erf`, `erfc`, `erfinv`, `erfcinv`, `erfcx`
- `normcdf`, `normcdfinv`
- `lgamma`, `tgamma`
- `j0`, `j1`, `jn`, `y0`, `y1`, `yn`
- `cyl_bessel_i0`, `cyl_bessel_i1`

#### 4. 位运算和类型转换 (部分支持)
- `clz`, `popc`, `ffs`, `brev`
- `abs` (整数), `mulhi`, `mul24`
- `float2int`/`int2float` 系列 (含舍入模式)
- `float_as_int`, `int_as_float` 等位解释转换

#### 5. 高精度运算
- `fma`, `fmod`, `remainder`
- `div_rn/rz/rd/ru` 系列
- `sqrt_rn/rz/rd/ru` 系列
- `add_rn/rz/rd/ru` 系列
- `mul_rn/rz/rd/ru` 系列

### 二、硬件兼容性关键差异

| 功能 | Ascend 950PR/950DT | Atlas A2/A3 |
|-----|-------------------|-------------|
| SIMT API (`sinf`, `cosf` 等) | ✅ 完全支持 | ❌ 不支持 |
| Vector API (`Sin`, `Cos` 等) | ✅ 支持 | ✅ 支持 |
| C API (`asc_sin`, `asc_cos` 等) | ✅ 支持 | ✅ 支持 |
| 类型转换 SIMT API | ✅ 支持 | ❌ 不支持 |
| 位运算 SIMT API | ✅ 支持 | ❌ 不支持 |

### 三、需要特殊处理的函数

#### 1. 舍入模式变体
AscendC **不直接支持**通过参数指定舍入模式。实现方式：
- 基础运算 + `Cast` API 配合 `RoundMode` 枚举
- `CAST_RINT` (最近偶数), `CAST_TRUNC` (向零), `CAST_FLOOR` (向下), `CAST_CEIL` (向上)

#### 2. Double 类型相关
- 大部分 SIMT API 仅支持 `float`，`double` 版本需用 C++ 标准转换
- `hiloint2double`, `double2loint`, `double2hiint` 无直接支持，需 union 实现

#### 3. Fast 版本函数
- `fast_sinf`, `fast_cosf` 等无专门 fast 版本
- 可直接使用标准 `sinf`, `cosf` 或适当降低精度配置

#### 4. 组合实现函数
- `saturatef`: `min(max(x, 0.0f), 1.0f)`
- `hadd`: `(a + b) / 2`
- `rhadd`: `(a + b + 1) / 2`
- `byte_perm`: `asc_gather` + `asc_pack` 组合
- `brev`: 移位操作组合

### 四、头文件对照

| API 类型 | 头文件 |
|---------|--------|
| SIMT 数学函数 | `simt_api/math_functions.h` |
| SIMT 设备函数 | `simt_api/device_functions.h` |
| Vector 算子 API | `kernel_operator.h` |
| C API (A2/A3) | `ascendc.h` |

### 五、使用建议

1. **跨平台兼容性**: 优先使用 Vector API，支持所有硬件平台
2. **性能优化**: Ascend 950 上可使用 SIMT API 获得更好性能
3. **精度要求**: 对于需要特定舍入模式的场景，使用 `Cast` + `RoundMode`
4. **Double 类型**: 尽量避免 double 类型的高频计算，使用 float 替代
5. **特殊函数**: Bessel 函数等在 Ascend 950 上有线程数限制（≤256）

## 统计汇总

| 类别 | 数量 | 占比 |
|-----|------|------|
| 直接支持 (所有平台) | ~120 | 61% |
| 直接支持 (仅 950) | ~50 | 25% |
| 需组合实现 | ~20 | 10% |
| 需软件模拟 | ~7 | 4% |

## 注意事项

1. 所有分析基于 AscendC API 文档，实际使用需验证硬件和 CANN 版本兼容性
2. 部分函数有输入值域限制，超出范围可能导致未定义行为
3. 建议在实际部署前进行充分测试
