# brpc GlobalUpdate bvar PerSecond 字符串泄露分析(bvar 抛弃家族之四)

> **现象**:ASan 报告
> ```
> Direct leak of 61 byte(s) in 1 object(s) allocated from:
>     #1 std::__cxx11::basic_string::reserve
>     #2 bvar::to_underscored_name variable.cpp:943
>     #3 bvar::Variable::expose_impl variable.cpp:155
>     #4 bvar::detail::WindowBase<PassiveStatus<long>, SeriesFrequency::1>::expose_impl window.h:147
>     #5 bvar::Variable::expose variable.h:141
>     #6 bvar::PerSecond<PassiveStatus<long>>::PerSecond window.h:206
>     #7 GlobalUpdate global.cpp:232
>     #8 bthread::TaskGroup::task_runner
> ```
>
> 本文确认其为 bvar local 抛弃家族的**第三个长跑 bthread** 实例(`GlobalUpdate`),与 `UpdateDerivedVars`(server.cpp)/`TimerThread::run`(timer_thread.cpp)同根同模式。

## 1. bvar 抛弃家族的三个长跑 bthread

至今观测到的 bvar local 抛弃泄露分布在 brpc 的三个独立长跑 bthread,模式完全一致(循环外声明 bvar local + vector,进程退出时 bthread 被抛弃 → local 未析构 → 暴露名 string/缓冲失主):

| bthread | 文件 | bvar local 例 | vector | 已观测泄露 |
|---------|------|--------------|--------|-----------|
| `Server::UpdateDerivedVars` | `server.cpp:315` | `nconn_st`/`nservice_st`/`start_time_st`/... | `conns`/`internal_conns` | 77B/71B/63B + vector 80B |
| `bthread::TimerThread::run` | `timer_thread.cpp:318` | `nscheduled_second`/`ntriggered_second`/`busy_seconds_second` | `tasks` | 73B |
| **`GlobalUpdate`(本报告)** | **`global.cpp:222`** | **`var_iobuf_new_bigview_second`/...** | **`conns`(:244)** | **61B** |

三者均为 brpc 库内部 bthread,ubs-comm/ub_test 均无直接代码关联(仅 ub_test 启动 brpc 间接触发)。

## 2. 泄露对象

- **对象**:`std::__cxx11::basic_string` 堆缓冲(超 SSO 32B → 堆分配),bvar 暴露名。
- **分配点**:`bvar::to_underscored_name`(`variable.cpp:943`)`reserve`,经 `PerSecond<PassiveStatus<int64_t>>::PerSecond`(`window.h:206`)→ `expose`(`variable.h:141`)→ `WindowBase::expose_impl`(`window.h:147`)→ `Variable::expose_impl`。
- **大小**:61 字节。
- **归属 string**:`iobuf_newbigview_second` 经 `to_underscored_name` 转换后的暴露名。

## 3. 代码细节

`GlobalUpdate`(`global.cpp:222`)是 brpc 全局 init 启动的 bthread,循环外声明一串 bvar local(`global.cpp:224-236`)+ `conns` vector(`:244`):

```cpp
static void* GlobalUpdate(void*) {
    // Expose variables.
    bvar::PassiveStatus<int64_t> var_iobuf_block_count(
        "iobuf_block_count", GetIOBufBlockCount, NULL);                  // :224-225
    bvar::PassiveStatus<int64_t> var_iobuf_block_count_hit_tls_threshold(
        "iobuf_block_count_hit_tls_threshold", ...);                    // :226-228
    bvar::PassiveStatus<int64_t> var_iobuf_new_bigview_count(
        GetIOBufNewBigViewCount, NULL);                                 // :229-230
    bvar::PerSecond<bvar::PassiveStatus<int64_t>> var_iobuf_new_bigview_second(
        "iobuf_newbigview_second", &var_iobuf_new_bigview_count);       // :231-232 ← frame #6 本报告
    bvar::PassiveStatus<int64_t> var_iobuf_block_memory(
        "iobuf_block_memory", GetIOBufBlockMemory, NULL);                // :233-234
    bvar::PassiveStatus<int> var_running_server_count(
        "rpc_server_count", GetRunningServerCount, NULL);               // :235-236

    butil::FileWatcher fw;
    ...
    std::vector<SocketId> conns;                                         // :244 — 同 UpdateDerivedVars 模式
    ...
    while (1) {
        ...
        if (bthread_usleep(sleep_us) < 0) {
            PLOG_IF(FATAL, errno != ESTOP) << "Fail to sleep";
            break;                                                       // ESTOP 退出 → local 析构
        }
        ...
    }
}
```

`GlobalUpdate` 与 `UpdateDerivedVars` 结构高度同构:循环外 bvar local + `conns` vector,`while(1)` + `bthread_usleep`,ESTOP `break` 退出。

## 4. 为何泄露(同 UpdateDerivedVars/TimerThread 家族)

- 正常路径:brpc 全局退出触发 `bthread_usleep` 返回 ESTOP(`global.cpp:253-255` `break`)→ `while` 结束 → 函数返回 → 函数级 local 析构 → bvar `Unexpose` 释放 string + `conns` vector 释放缓冲。**无泄露**。
- 泄露路径:进程退出未干净停止 `GlobalUpdate` bthread → 被抛弃 → 函数级 local(`var_iobuf_*` + `var_running_server_count` + `conns` vector)全部未析构 → 各自暴露名 string + vector 缓冲失主 → ASan 逐个标记。

本 61B 是 `var_iobuf_new_bigview_second` 的暴露名 string。同 bthread 的其余 bvar local(`var_iobuf_block_count`/`var_iobuf_block_memory`/`var_running_server_count` 等)的暴露名 string + `conns` vector 缓冲应另有独立 ASan 报告(未全部粘贴),均同根。

## 5. 为何 61B / 1 个

- `to_underscored_name("iobuf_newbigview_second")` 结果超 SSO 32B → 堆分配,capacity 61B。
- ASan 报 1 个 = `var_iobuf_new_bigview_second` 的暴露名 string(frame #6 `:232` 命中)。
- 与同 bthread 其他 bvar local 的 string/vector 是**同 bthread 同次抛弃的多个独立对象**,应一并观察。

## 6. 触发条件

- brpc 全局 init 启动 `GlobalUpdate` bthread(任何 brpc 程序)
- 进程退出未干净停止该 bthread → 抛弃 → local 未析构
- 与 UB 配置无关,纯 brpc 库行为
- 本次报在 `ub_test_client`(client 启动 brpc 全局→GlobalUpdate bthread 启动)

## 7. 修复方案

**与 `UBSOCKET-UBTEST-DUMMY-SERVER-BVAR-*-LEAK-ANALYSIS.ch.md`、`UBSOCKET-UBTEST-TIMERTHREAD-BVAR-LEAK-ANALYSIS.ch.md` 同家族共用**,brpc 侧确保 `GlobalUpdate` bthread 干净停止:

### 方案 1【brpc 侧】全局退出停止 GlobalUpdate bthread

brpc 全局退出(`bthread_stop_world`/atexit)中,像 `UpdateDerivedVars` 经 ESTOP 退出一样,确保 `GlobalUpdate` bthread 收到 ESTOP 并 `break` → 函数返回 → local 析构 → bvar Unexpose + vector 释放。需 brpc 补全局退出时对各长跑 bthread 的统一 join。

### 方案 2【brpc 侧】bvar local 改静态/进程级

`GlobalUpdate` 的 `var_iobuf_*`/`var_running_server_count` 改为 `static` 或全局对象,生命周期与进程一致,bvar registry 持有引用可达,不依赖 bthread 析构时机。但需注意 bvar 重复 expose 检查。

### 方案 3【防御】bvar registry atexit 钩子统一 Unexpose

bvar `Variable` 全局 registry 注册 atexit 钩子,退出时遍历 Unexpose 释放所有暴露名 string,**一次性覆盖所有 bvar local 未析构场景**(UpdateDerivedVars + TimerThread + GlobalUpdate 三个 bthread 的全部 bvar string/vector 泄露)。最彻底。

## 8. ubs-comm 侧动作

**无。** 与 ubsocket/umq 适配层无任何代码关联。属 brpc 库生命周期问题。

## 9. 关键代码位置索引

| 位置 | 作用 | 泄露相关性 |
|------|------|-----------|
| `global.cpp:222` | `GlobalUpdate` bthread 主函数 | 第三个长跑 bthread |
| `global.cpp:224-236` | bvar PassiveStatus/PerSecond local 全家谱 | 同家族 |
| `global.cpp:231-232` | `var_iobuf_new_bigview_second` PerSecond ctor | **frame #6,泄露触发** |
| `global.cpp:244` | `conns` vector | 同 UpdateDerivedVars 模式(应有报告) |
| `window.h:206` | `PerSecond` ctor → `expose` | 调用链 |
| `window.h:147` | `WindowBase::expose_impl` | 调用链 |
| `variable.cpp:155` | `expose_impl` → `to_underscored_name` | 调用链 |
| `variable.cpp:943` | `to_underscored_name` `reserve` | **string 堆分配点** |

## 10. 与其他泄露的关系

| 维度 | 本泄露(GlobalUpdate 61B) | UpdateDerivedVars bvar(77/71/63B) | TimerThread bvar(73B) | dummy vector 80B | TX Event | RespClosure/done | RX | 析构链家族 | RpcMeta |
|------|--------------------------|------------------------------------|----------------------|-----------------|----------|------------------|----|-----------|---------|
| 归属 | brpc 全局 init | brpc Server + ub_test dummy | brpc bthread/bvar | 同 UpdateDerivedVars | ubsocket umq | ub_test | ubsocket umq | ubsocket 核心 | brpc/protobuf |
| 类别 | bvar local 未析构 | 同 | 同(不同线程) | bthread 抛弃致 vector 未析构 | 退出未释放 | 闭包/drain | buffer 不回流 | 析构不 delete | 解析逃逸 |
| 与其他 bvar 关系 | **同家族第三线程** | 自身 | 同家族第二线程 | 同 bthread 抛弃类 | 独立 | 独立 | 独立 | 独立 | 独立 |
| ubs-comm 修复 | ❌ | ❌ | ❌ | ❌ | ✓ | ❌ | ✓ | ✓ | ❌ |

本泄露是 bvar 抛弃家族的**第三个长跑 bthread**(GlobalUpdate),与 UpdateDerivedVars(server.cpp)、TimerThread(timer_thread.cpp)同根同模式。三者均 brpc 库生命周期问题,共用修复方案(方案 3 atexit 钩子最彻底,一次覆盖三个 bthread 全部 bvar string/vector 泄露)。

## 参考

- `UBSOCKET-UBTEST-DUMMY-SERVER-BVAR-LEAK-ANALYSIS.ch.md` — UpdateDerivedVars `nconn_st` 77B(同家族第一线程)
- `UBSOCKET-UBTEST-DUMMY-SERVER-BVAR-SERVICE-LEAK-ANALYSIS.ch.md` — `nservice_st` 71B
- `UBSOCKET-UBTEST-DUMMY-SERVER-BVAR-STARTTIME-LEAK-ANALYSIS.ch.md` — `start_time_st` 63B
- `UBSOCKET-UBTEST-TIMERTHREAD-BVAR-LEAK-ANALYSIS.ch.md` — TimerThread bvar 73B(同家族第二线程)
- `UBSOCKET-UBTEST-DUMMY-SERVER-LEAK-ANALYSIS.ch.md` — dummy vector 80B
- 其他 ubs-comm/ub_test/brpc 泄露文档
- brpc 源码:`src/brpc/global.cpp:222-256`、`src/bvar/window.h:147,206`、`src/bvar/variable.cpp:155,943`
