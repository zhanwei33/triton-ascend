# brpc GlobalUpdate bvar hit_tls_threshold 字符串泄露分析(GlobalUpdate bvar 家族之二)

> **现象**:ASan 报告
> ```
> Direct leak of 44 byte(s) in 1 object(s) allocated from:
>     #1 std::__cxx11::basic_string::reserve
>     #2 bvar::Variable::expose_impl variable.cpp:148
>     #3 bvar::PassiveStatus<long>::expose_impl passive_status.h:173
>     #4 bvar::Variable::expose variable.h:141
>     #5 bvar::PassiveStatus<long>::PassiveStatus(prefix, long(*)(void*), void*) passive_status.h:85
>     #6 GlobalUpdate global.cpp:228
>     #7 bthread::TaskGroup::task_runner
> ```
>
> 本文确认其为 `UBSOCKET-UBTEST-GLOBALUPDATE-BVAR-LEAK-ANALYSIS.ch.md`(61B `var_iobuf_new_bigview_second`)的**同 bthread 同次抛弃伴生**,即该文档 §3 已列出的 `var_iobuf_block_count_hit_tls_threshold` local。

## 1. GlobalUpdate bvar local 全家谱(已观测 2/6)

`GlobalUpdate`(`global.cpp:222`)循环外声明 6 个 bvar local(`global.cpp:224-236`),`expose` 注册暴露名 string(超 SSO 堆分配)。bthread 被抛弃时全部未析构 → 各自 string 失主。已观测 2 个:

| local | 类型 | 行 | 暴露名 | ASan 大小 | 分配点 | 状态 |
|-------|------|-----|--------|-----------|--------|------|
| `var_iobuf_block_count` | `PassiveStatus<int64_t>` | :224-225 | `iobuf_block_count` | ? | ? | 同家族(应有报告) |
| **`var_iobuf_block_count_hit_tls_threshold`** | **`PassiveStatus<int64_t>`**(=`PassiveStatus<long>` on aarch64) | **:226-228** | **`iobuf_block_count_hit_tls_threshold`** | **44B** | **`expose_impl:148`** | **本报告** |
| `var_iobuf_new_bigview_count` | `PassiveStatus<int64_t>` | :229-230 | (无 name,默认) | ? | ? | 同家族 |
| `var_iobuf_new_bigview_second` | `PerSecond<PassiveStatus<int64_t>>` | :231-232 | `iobuf_newbigview_second` | 61B | `to_underscored_name:943` | 已文档 |
| `var_iobuf_block_memory` | `PassiveStatus<int64_t>` | :233-234 | `iobuf_block_memory` | ? | ? | 同家族 |
| `var_running_server_count` | `PassiveStatus<int>` | :235-236 | `rpc_server_count` | ? | ? | 同家族 |

## 2. 泄露对象

- **对象**:`std::__cxx11::basic_string` 堆缓冲(超 SSO 32B → 堆分配),bvar 暴露名。
- **分配点**:`bvar::Variable::expose_impl`(`variable.cpp:148`)`std::string::reserve`。
- **大小**:44 字节。
- **归属 string**:`iobuf_block_count_hit_tls_threshold` 经 `expose_impl` 构造的暴露名(本 local 用单名 ctor `PassiveStatus(name, cb, arg)`,无 prefix,故暴露名即转换后的 name)。
- **类型注**:frame #3 `PassiveStatus<long>`——aarch64 64 位下 `int64_t`==`long`,源码声明 `PassiveStatus<int64_t>` 经模板实例化为 `PassiveStatus<long>`,与报告一致。

## 3. 代码细节

`GlobalUpdate`(`global.cpp:222`)循环外(`global.cpp:226-228`):

```cpp
bvar::PassiveStatus<int64_t> var_iobuf_block_count_hit_tls_threshold(
    "iobuf_block_count_hit_tls_threshold",
    GetIOBufBlockCountHitTLSThreshold, NULL);   // :226-228 ← frame #6 本报告
```

`PassiveStatus` 构造 → `expose`(`variable.h:141` 单名版)→ `expose_impl`(`variable.cpp:148` 拼暴露名 string)→ 注册到 bvar registry。bthread 正常退出(ESTOP `break`)时 local 析构 → bvar `Unexpose` 释放 string。

## 4. 为何泄露(同 var_iobuf_new_bigview_second)

与 `UBSOCKET-UBTEST-GLOBALUPDATE-BVAR-LEAK-ANALYSIS.ch.md` §4 完全相同:

- 正常路径:brpc 全局退出触发 `bthread_usleep` 返回 ESTOP(`global.cpp:253-255` `break`)→ `while` 结束 → 函数返回 → 函数级 local 析构 → bvar `Unexpose` 释放 string。**无泄露**。
- 泄露路径:进程退出未干净停止 `GlobalUpdate` bthread → 被抛弃 → 函数级 local(`var_iobuf_block_count`/`var_iobuf_block_count_hit_tls_threshold`/`var_iobuf_new_bigview_count`/`var_iobuf_new_bigview_second`/`var_iobuf_block_memory`/`var_running_server_count` + `conns` vector)全部未析构 → 各自暴露名 string + vector 缓冲失主 → ASan 逐个标记。

## 5. 为何 44B / 1 个

- `expose_impl:148` 构造暴露名 `iobuf_block_count_hit_tls_threshold`(36 字符)→ 超 SSO 32B → 堆分配,capacity 44B。
- ASan 报 1 个 = `var_iobuf_block_count_hit_tls_threshold` 在 `expose_impl:148` 的暴露名 string。
- 与 `var_iobuf_new_bigview_second`(61B)是**同 bthread 同次抛弃的两个独立对象**,应一并观察。

## 6. 触发条件

- brpc 全局 init 启动 `GlobalUpdate` bthread(任何 brpc 程序)
- 进程退出未干净停止该 bthread → 抛弃 → local 未析构
- 与 UB 配置无关,纯 brpc 库行为

## 7. 修复方案

**与 `UBSOCKET-UBTEST-GLOBALUPDATE-BVAR-LEAK-ANALYSIS.ch.md` §7 完全共用**,修复一次消除全部 `GlobalUpdate` bvar local 泄露(44B hit_tls + 61B newbigview + 其余 4 个 + vector):

### 方案 1【brpc 侧】全局退出停止 GlobalUpdate bthread

brpc 全局退出确保 `GlobalUpdate` bthread 收 ESTOP 并 `break` → 函数返回 → local 析构 → bvar Unexpose + vector 释放。

### 方案 2【brpc 侧】bvar local 改静态/进程级

`GlobalUpdate` 的 bvar local 改 `static` 或全局,生命周期与进程一致,不依赖 bthread 析构时机。

### 方案 3【防御,跨三 bthread 通用】bvar registry atexit 钩子统一 Unexpose

bvar `Variable` 全局 registry 注册 atexit 钩子,退出时遍历 Unexpose 释放所有暴露名 string,**一次覆盖 `UpdateDerivedVars`/`TimerThread`/`GlobalUpdate` 三个 bthread 全部 bvar string/vector 泄露**。最彻底。

## 8. ubs-comm 侧动作

**无。** 与 ubsocket/umq 适配层无任何代码关联。属 brpc 库生命周期问题。

## 9. 关键代码位置索引

| 位置 | 作用 | 泄露相关性 |
|------|------|-----------|
| `global.cpp:226-228` | `var_iobuf_block_count_hit_tls_threshold` ctor | **frame #6,泄露触发** |
| `global.cpp:224-236` | 全部 bvar local | 同家族(block_count/newbigview/block_memory/running_server) |
| `passive_status.h:85` | `PassiveStatus` 单名 ctor → `expose` | 调用链 |
| `variable.h:141` | `expose`(单名版)→ `expose_impl` | 调用链 |
| `variable.cpp:148` | `expose_impl` 拼暴露名 string | **本报告 string 堆分配点** |
| `global.cpp:222-256` | `GlobalUpdate` 函数级 local + while 循环 | bthread 抛弃致不析构 |

## 10. 与其他泄露的关系

| 维度 | 本泄露(hit_tls 44B) | newbigview 61B | UpdateDerivedVars bvar(63/71/77/46B) | TimerThread bvar 73B | dummy vector 80B | TX Event | RespClosure/done | RX | 析构链家族 | RpcMeta |
|------|---------------------|----------------|-------------------------------------|---------------------|-----------------|----------|------------------|----|-----------|---------|
| 归属 | brpc 全局 init | 同 | brpc Server + ub_test dummy | brpc bthread/bvar | 同 UpdateDerivedVars | ubsocket umq | ub_test | ubsocket umq | ubsocket 核心 | brpc/protobuf |
| 类别 | bvar local 未析构 | 同 | 同 | 同(不同线程) | bthread 抛弃致 vector 未析构 | 退出未释放 | 闭包/drain | buffer 不回流 | 析构不 delete | 解析逃逸 |
| 与 newbigview 关系 | **同 bthread 同次(第二 local)** | 自身 | 同家族不同 bthread | 同家族不同 bthread | 同 bthread 抛弃类 | 独立 | 独立 | 独立 | 独立 | 独立 |
| ubs-comm 修复 | ❌ | ❌ | ❌ | ❌ | ❌ | ✓ | ❌ | ✓ | ✓ | ❌ |

本泄露与 `var_iobuf_new_bigview_second`(61B)是**同 `GlobalUpdate` bthread 同次抛弃的两个 PassiveStatus local**,均 brpc 库生命周期问题,共用修复方案。方案 3(bvar registry atexit 钩子)可一次覆盖三个 bthread 全部 bvar/vector 泄露。

## 参考

- `UBSOCKET-UBTEST-GLOBALUPDATE-BVAR-LEAK-ANALYSIS.ch.md` — `var_iobuf_new_bigview_second` 61B(同 bthread 同次,§3 已列出本 local)
- `UBSOCKET-UBTEST-DUMMY-SERVER-BVAR-*-LEAK-ANALYSIS.ch.md` — UpdateDerivedVars bvar 家族(同家族不同 bthread)
- `UBSOCKET-UBTEST-TIMERTHREAD-BVAR-LEAK-ANALYSIS.ch.md` — TimerThread bvar 73B(同家族不同 bthread)
- `UBSOCKET-UBTEST-DUMMY-SERVER-LEAK-ANALYSIS.ch.md` — dummy vector 80B
- 其他 ubs-comm/ub_test/brpc 泄露文档
- brpc 源码:`src/brpc/global.cpp:222-256`、`src/bvar/variable.cpp:148,155,943`、`src/bvar/passive_status.h:85,173`
