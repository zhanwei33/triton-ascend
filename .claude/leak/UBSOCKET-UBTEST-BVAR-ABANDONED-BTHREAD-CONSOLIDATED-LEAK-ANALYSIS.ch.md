# brpc bvar/local 抛弃 bthread 泄露汇总分析(7 条剩余实例)

> **现象**:7 条 ASan 报告,33B×1 + 31B×6,均来自 brpc 三个长跑 bthread(`UpdateDerivedVars`/`GlobalUpdate`/`TimerThread`)的函数级 local,进程退出时 bthread 被抛弃 → local 未析构 → string 泄露。
>
> 本文汇总这 7 条(均属已建立的 bvar/local 抛弃家族),不再逐个建独立文档。

## 1. 7 条泄露映射

| # | 大小 | bthread | local | 行 | 分配点 | 归属 string/对象 |
|---|------|---------|-------|-----|--------|-----------------|
| 1 | 33B | `UpdateDerivedVars` | `prefix`(local string) | `server.cpp:319` | `string_printf`(`string_printf.cpp:74`)经 `Server::ServerPrefix`(`:309`) | `<bvar_prefix>_<server>` prefix 串 |
| 2 | 31B | `GlobalUpdate` | `fw`(FileWatcher local) | `global.cpp:239` | `FileWatcher::init_from_not_exist`(`file_watcher.cpp:47`)+`_M_replace` | FileWatcher 内部路径 string |
| 3 | 31B | `GlobalUpdate` | `var_iobuf_block_count` | `global.cpp:225` | `expose_impl:148`(`PassiveStatus<long>`) | `iobuf_block_count` 暴露名 |
| 4 | 31B | `GlobalUpdate` | `var_running_server_count` | `global.cpp:236` | `expose_impl:148`(`PassiveStatus<int>`) | `rpc_server_count` 暴露名 |
| 5 | 31B | `GlobalUpdate` | `var_iobuf_block_memory` | `global.cpp:234` | `expose_impl:148`(`PassiveStatus<long>`) | `iobuf_block_memory` 暴露名 |
| 6 | 31B | `TimerThread::run` | `busy_seconds_second` | `timer_thread.cpp:344` | `expose_impl:148`(`WindowBase<PassiveStatus<double>>`→`expose_as`) | `usage` 暴露名 |
| 7 | 31B | `UpdateDerivedVars` | `uptime_st` | `server.cpp:330` | `expose_impl:148`(`PassiveStatus<timeval>`→`expose_as`) | `uptime` 暴露名 |

## 2. 均属已建立的 bvar/local 抛弃家族

本家族根因:brpc 长跑 bthread 在循环外声明函数级 local(bvar `PassiveStatus`/`PerSecond` + `vector` + `FileWatcher` + prefix string),`while(1)`+`bthread_usleep`,ESTOP 退出时 local 析构释放;**进程退出未干净停止 bthread → 被抛弃 → local 未析构 → 各自 string/缓冲失主泄露**。本 7 条是该家族的剩余实例,填补之前文档 anticipated 的空位:

- `UBSOCKET-UBTEST-DUMMY-SERVER-BVAR-STARTTIME-LEAK-ANALYSIS.ch.md` §1 已 anticipated `uptime_st`/`nbuiltinservice_st`/`nsessiondata_st` → 本 #7 = `uptime_st` ✓
- `UBSOCKET-UBTEST-GLOBALUPDATE-BVAR-LEAK-ANALYSIS.ch.md` §3 已列 `var_iobuf_block_count`/`var_iobuf_block_memory`/`var_running_server_count` → 本 #3/#4/#5 ✓
- `UBSOCKET-UBTEST-TIMERTHREAD-BVAR-LEAK-ANALYSIS.ch.md` §1 已列 `busy_seconds_second` → 本 #6 ✓
- #1(prefix string)、#2(FileWatcher)是同 bthread 的**非 bvar local**(prefix 串、FileWatcher 路径串),同根因不同对象

## 3. bvar/local 抛弃家族完整观测清单(3 bthread)

| bthread | 文件 | 已观测 local | 报告大小 |
|---------|------|--------------|---------|
| `Server::UpdateDerivedVars` | `server.cpp:315` | `prefix`(#1) + `uptime_st`(#7) + `start_time_st`(63B) + `nconn_st`(77B) + `nservice_st`(71B) + `nbuiltinservice_st`(46B) + `conns`/`internal_conns` vector(80B) | 33+31+63+77+71+46+80 |
| `bthread::TimerThread::run` | `timer_thread.cpp:318` | `nscheduled_second`(73B) + `ntriggered_second`(?预期) + `busy_seconds_second`(#6) + `tasks` vector(?预期) | 73+31 |
| `GlobalUpdate` | `global.cpp:222` | `var_iobuf_block_count`(#5) + `var_iobuf_block_count_hit_tls_threshold`(44B) + `var_iobuf_new_bigview_count`(?预期) + `var_iobuf_new_bigview_second`(61B) + `var_iobuf_block_memory`(#3) + `var_running_server_count`(#4) + `fw` FileWatcher(#2) + `conns` vector(?预期) | 31+44+61+31+31+31 |

三个 bthread 的全部函数级 local(含 bvar、vector、FileWatcher、prefix 串)在 bthread 抛弃时**整组泄露**,ASan 按分配栈逐个报告。

## 4. 根因与修复(同家族,不重复)

**根因**:brpc 长跑 bthread 退出流程不干净(进程 `exit` 未对各 bthread `bthread_join`/ESTOP),函数级 local 未走析构。

**统一修复**(覆盖三个 bthread 全部 local,无需逐个改):

### 方案 A【最彻底】bvar registry atexit 钩子统一 Unexpose

bvar `Variable` 全局 registry 注册 atexit 钩子,退出时遍历 Unexpose 释放所有暴露名 string。**覆盖全部 bvar string**(#1 prefix 非 bvar 不覆盖、#2 FileWatcher 非 bvar 不覆盖,但这两类量小)。

### 方案 B【根治全部 local 含非 bvar】brpc 全局退出干净 join 三个 bthread

brpc 全局退出(`bthread_stop_world`/atexit)对 `UpdateDerivedVars`/`GlobalUpdate`/`TimerThread` 三个 bthread 统一发送 ESTOP + `bthread_join`/`pthread_join`,确保 `while` 退出、函数返回、**全部函数级 local 析构**(bvar Unexpose + vector 释放 + FileWatcher 析构 + prefix 串释放)。**一次覆盖本 7 条 + 之前全部 bvar/vector 报告**。

### 方案 C【防御】local 改静态/全局

三个 bthread 的 local(bvar/FileWatcher/prefix)改 `static` 或全局,生命周期与进程一致,不依赖 bthread 析构时机。改动较大。

推荐**方案 B**(根治全部 local 类型),方案 A 作 bvar 专项兜底。

## 5. ubs-comm 侧动作

**无。** 全部 7 条与 ubsocket/umq 适配层无任何代码关联。属 brpc 库生命周期问题。

## 6. 关键代码位置索引

| 位置 | 作用 | 泄露相关性 |
|------|------|-----------|
| `server.cpp:309` | `Server::ServerPrefix` `string_printf` | #1 prefix 串分配 |
| `server.cpp:319` | `UpdateDerivedVars` 调 `ServerPrefix` | #1 frame #5 |
| `server.cpp:329-330` | `uptime_st` PassiveStatus<timeval> | #7 |
| `global.cpp:224-225` | `var_iobuf_block_count` | #5 |
| `global.cpp:233-234` | `var_iobuf_block_memory` | #3 |
| `global.cpp:235-236` | `var_running_server_count` | #4 |
| `global.cpp:238-239` | `butil::FileWatcher fw; init_from_not_exist` | #2 FileWatcher |
| `file_watcher.cpp:47` | `FileWatcher::init_from_not_exist` `_M_replace` | #2 路径串分配 |
| `timer_thread.cpp:344` | `busy_seconds_second.expose_as` | #6 |
| `variable.cpp:148` | `expose_impl` 拼暴露名 string | #3/#4/#5/#6/#7 string 分配 |
| `string_printf.cpp:74,93` | `string_printf_impl` | #1 prefix 串分配 |

## 7. 与其他泄露的关系

本 7 条均属 bvar/local 抛弃家族(同 `UBSOCKET-UBTEST-DUMMY-SERVER-BVAR-*-LEAK-ANALYSIS.ch.md`/`UBSOCKET-UBTEST-GLOBALUPDATE-BVAR-*-LEAK-ANALYSIS.ch.md`/`UBSOCKET-UBTEST-TIMERTHREAD-BVAR-LEAK-ANALYSIS.ch.md`),与 ubs-comm 析构链家族(`UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md` 等)、`UBSOCKET-ASYNCEVENTPOLL-EPOLLEVENT-LEAK-ANALYSIS.ch.md`、`UBSOCKET-UMQ-RX-LEAK-ANALYSIS.ch.md`、`UBSOCKET-UMQ-TX-EVENT-LEAK-ANALYSIS.ch.md`、`UBSOCKET-UBTEST-CLIENT-LEAK-ANALYSIS.ch.md`/`-DONE-CALLBACK-`/`-RUNTEST-`、`UBSOCKET-UBTEST-PROTO-META-*-LEAK-ANALYSIS.ch.md` **机理独立、修复点不重叠**。ubs-comm 侧无修复点。

## 参考

- `UBSOCKET-UBTEST-DUMMY-SERVER-BVAR-LEAK-ANALYSIS.ch.md` — `nconn_st` 77B(同 bthread)
- `UBSOCKET-UBTEST-DUMMY-SERVER-BVAR-SERVICE-LEAK-ANALYSIS.ch.md` — `nservice_st` 71B
- `UBSOCKET-UBTEST-DUMMY-SERVER-BVAR-STARTTIME-LEAK-ANALYSIS.ch.md` — `start_time_st` 63B(§1 anticipated `uptime_st`)
- `UBSOCKET-UBTEST-DUMMY-SERVER-BVAR-BUILTINSERVICE-LEAK-ANALYSIS.ch.md` — `nbuiltinservice_st` 46B
- `UBSOCKET-UBTEST-DUMMY-SERVER-LEAK-ANALYSIS.ch.md` — dummy vector 80B(同 bthread 抛弃类)
- `UBSOCKET-UBTEST-GLOBALUPDATE-BVAR-LEAK-ANALYSIS.ch.md` — `var_iobuf_new_bigview_second` 61B(§3 列出本 #3/#4/#5)
- `UBSOCKET-UBTEST-GLOBALUPDATE-BVAR-2-LEAK-ANALYSIS.ch.md` — `var_iobuf_block_count_hit_tls_threshold` 44B
- `UBSOCKET-UBTEST-TIMERTHREAD-BVAR-LEAK-ANALYSIS.ch.md` — `nscheduled_second` 73B(§1 列出本 #6)
- 其他 ubs-comm/ub_test/brpc 泄露文档
- brpc 源码:`src/brpc/server.cpp:309,315-345`、`src/brpc/global.cpp:222-256`、`src/bthread/timer_thread.cpp:318-345`、`src/butil/string_printf.cpp:74,93`、`src/butil/files/file_watcher.cpp:47`、`src/bvar/variable.cpp:148,155,943`
