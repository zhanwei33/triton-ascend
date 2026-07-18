# brpc Dummy Server bvar nservice_st 字符串泄露分析(UpdateDerivedVars bvar 家族之二)

> **现象**:ASan 报告
> ```
> Direct leak of 71 byte(s) in 1 object(s) allocated from:
>     #1 std::__cxx11::basic_string::reserve
>     #2 bvar::to_underscored_name variable.cpp:943
>     #3 bvar::Variable::expose_impl variable.cpp:155
>     #4 bvar::PassiveStatus<int>::expose_impl passive_status.h:173
>     #5 bvar::Variable::expose_as variable.h:162
>     #6 bvar::PassiveStatus<int>::PassiveStatus passive_status.h:95
>     #7 brpc::Server::UpdateDerivedVars server.cpp:339
>     #8 bthread::TaskGroup::task_runner
> ```
>
> 本文确认其为 `UBSOCKET-UBTEST-DUMMY-SERVER-BVAR-LEAK-ANALYSIS.ch.md`(77B,`nconn_st` @ `:336`)的**同 bthread 同次抛弃伴生**,仅 `PassiveStatus` local 不同(`nservice_st` @ `:339`)。该文档 §5 已 anticipated "其余 `nservice_st`/`nbuiltinservice_st`/`nsessiondata_st` 的暴露名 string 应另有独立 ASan 报告",本报告即其一。

## 1. 与 77B bvar 泄露同根同次

| 维度 | 77B bvar 泄露(nconn_st) | 本泄露(71B,nservice_st) |
|------|------------------------|--------------------------|
| bthread | `Server::UpdateDerivedVars`(`server.cpp:315`) | **同** |
| frame #7 | `server.cpp:336`(`nconn_st` ctor) | `server.cpp:339`(`nservice_st` ctor) |
| PassiveStatus local | `nconn_st`("connection_count") | `nservice_st`("service_count") |
| 泄露对象 | 暴露名 string(77B) | 暴露名 string(71B) |
| 分配点 | `to_underscored_name` reserve(`variable.cpp:943`) | **同** |
| 根因 | bthread 抛弃致 bvar local 未析构 | **同** |
| 修复 | 干净 `Server::Stop` 让 bthread ESTOP 退出 | **同** |

两者都是 `UpdateDerivedVars` bthread 的**函数级 PassiveStatus local**(`server.cpp:335-345`),`expose_as` 注册暴露名 string(超 SSO 堆分配);进程退出时 bthread 被抛弃 → local 未析构 → bvar 未 Unexpose → string 失主泄露。

## 2. 泄露对象

- **对象**:`std::__cxx11::basic_string` 堆缓冲(超 SSO 32B → 堆分配),bvar 暴露名。
- **分配点**:`bvar::to_underscored_name`(`variable.cpp:943`)`reserve`,经 `PassiveStatus<int>::expose_impl`(`passive_status.h:173`)→ `expose_impl` → `expose_as` → `PassiveStatus` 构造。
- **大小**:71 字节。
- **归属 string**:`<prefix>_service_count` 经 `to_underscored_name` 转换后的暴露名。比 `nconn_st` 的 `<prefix>_connection_count` 短("service_count" < "connection_count"),故 71B < 77B。

## 3. 代码细节

`UpdateDerivedVars`(`server.cpp:315`)循环外声明多个 `PassiveStatus` local:

```cpp
bvar::PassiveStatus<int32_t> nconn_st(
    prefix, "connection_count", GetConnectionCount, server);   // :335-336 — 77B 泄露
bvar::PassiveStatus<int32_t> nservice_st(
    prefix, "service_count", GetServiceCount, server);          // :338-339 — 本 71B 泄露 ← frame #7
bvar::PassiveStatus<int32_t> nbuiltinservice_st(
    prefix, "builtin_service_count", GetBuiltinServiceCount, server);  // :341-342 — 同类(应有报告)
bvar::PassiveStatus<bvar::Vector<unsigned, 2>> nsessiondata_st(...);   // :344-345 — 同类(应有报告)
```

每个 `PassiveStatus` 构造即 `expose` 到全局 bvar registry,`expose_impl` 调 `to_underscored_name` 构造暴露名 string(超 SSO → 堆分配)。bthread 正常退出(ESTOP `return NULL`)时 local 析构 → bvar `Unexpose` → string 释放。

## 4. 为何泄露(同 77B)

与 `UBSOCKET-UBTEST-DUMMY-SERVER-BVAR-LEAK-ANALYSIS.ch.md` §4 完全相同:

- 正常路径:`Server::Stop()` → `bthread_usleep` 返回 ESTOP(`server.cpp:386-388` `return NULL`)→ 函数级 local 离开作用域析构 → bvar `Unexpose` 释放 string。**无泄露**。
- 泄露路径:client 退出未对 dummy server 干净 `Stop()`,`UpdateDerivedVars` bthread 被抛弃 → 函数级 local(`nconn_st`/`nservice_st`/`nbuiltinservice_st`/`nsessiondata_st` + `conns`/`internal_conns` vector)全部未析构 → 各自暴露名 string(77B/71B/...)+ vector 缓冲(80B)等失主 → ASan 逐个标记。

## 5. 为何 71B / 1 个

- `to_underscored_name(prefix, "service_count")` 结果长度超 SSO 32B → 堆分配,capacity 71B。
- ASan 报 1 个 = `nservice_st` 的暴露名 string(frame #7 `:339` 命中)。
- 与 77B(`nconn_st`)是**同 bthread 同次抛弃的两个独立 string 对象**,应一并观察。

## 6. 触发条件

与 77B bvar 泄露完全一致:进程启动 brpc Server(含 dummy server)→ `UpdateDerivedVars` bthread 运行 → 进程退出未干净 `Server::Stop()` → bthread 抛弃。与 UB 配置无关。

## 7. 修复方案

**与 `UBSOCKET-UBTEST-DUMMY-SERVER-BVAR-LEAK-ANALYSIS.ch.md` §7 完全共用**,修复一次消除全部 `UpdateDerivedVars` bvar local 泄露(77B nconn_st + 71B nservice_st + nbuiltinservice_st + nsessiondata_st)+ vector 80B:

### 方案 1【ub_test 侧】退出前显式 Stop dummy server

`ub_test/client.cpp:770` `StartDummyServerAt` 返回的 Server* 保存,`main` 返回前 `server->Stop(0)` + `server->Join()` → bthread ESTOP 退出 → 函数级 local 析构 → bvar Unexpose + vector 释放。

### 方案 2【brpc 侧】Server 析构/Stop 保证 bthread join

`~Server`/`Stop` 中 `bthread_join(_derived_vars_bthread)`,确保 bthread 退出且 local 析构后再返回。根治。

### 方案 3【防御】bvar PassiveStatus 改静态/Server 成员

`UpdateDerivedVars` 的 `nconn_st`/`nservice_st` 等改为 `static` 或 Server 成员,生命周期与进程/Server 一致,避免依赖 bthread 析构时机。

## 8. ubs-comm 侧动作

**无。** 与 ubsocket/umq 适配层无任何代码关联。属 brpc 生命周期 + ub_test dummy server 启停问题。

## 9. 关键代码位置索引

| 位置 | 作用 | 泄露相关性 |
|------|------|-----------|
| `server.cpp:338-339` | `nservice_st` PassiveStatus ctor | **frame #7,泄露触发** |
| `server.cpp:335-345` | 全部 PassiveStatus local | 同家族(nconn_st/nbuiltinservice_st/nsessiondata_st) |
| `passive_status.h:95` | `PassiveStatus` ctor → `expose_as` | 调用链 |
| `variable.h:162` | `expose_as` → `expose_impl` | 调用链 |
| `variable.cpp:155` | `expose_impl` → `to_underscored_name` | 调用链 |
| `variable.cpp:943` | `to_underscored_name` `reserve` | **string 堆分配点** |
| `server.cpp:315-345` | `UpdateDerivedVars` 函数级 local | bthread 抛弃致不析构 |

## 10. 与其他泄露的关系

| 维度 | 本泄露(nservice_st 71B) | nconn_st 77B | dummy vector 80B | TimerThread bvar 73B | TX Event | RespClosure/done | RX | 析构链家族 | RpcMeta |
|------|------------------------|-------------|-----------------|---------------------|----------|------------------|----|-----------|---------|
| 归属 | brpc Server + ub_test dummy | 同 | 同 | brpc bthread/bvar | ubsocket umq | ub_test | ubsocket umq | ubsocket 核心 | brpc/protobuf |
| 类别 | bvar local 未析构 | 同 | bthread 抛弃致 vector 未析构 | 同(不同线程) | 退出未释放 | 闭包/drain | buffer 不回流 | 析构不 delete | 解析逃逸 |
| 与 nconn_st 77B 关系 | **同 bthread 同次** | 自身 | 同 bthread 抛弃类 | 同家族不同线程 | 独立 | 独立 | 独立 | 独立 | 独立 |
| ubs-comm 修复 | ❌ | ❌ | ❌ | ❌ | ✓ | ❌ | ✓ | ✓ | ❌ |

本泄露与 `nconn_st`(77B)是**同 bthread 同次抛弃的两个 PassiveStatus local**,与 dummy vector(80B)是**同 bthread 不同 local 类型**,三者均 `UpdateDerivedVars` bthread 抛弃所致,共用修复方案。`TimerThread` bvar(73B)是同家族不同线程。

## 参考

- `UBSOCKET-UBTEST-DUMMY-SERVER-BVAR-LEAK-ANALYSIS.ch.md` — `nconn_st` 77B 泄露(同 bthread 同次,伞形修复合并)
- `UBSOCKET-UBTEST-DUMMY-SERVER-LEAK-ANALYSIS.ch.md` — dummy vector 80B 泄露(同 bthread 抛弃类)
- `UBSOCKET-UBTEST-TIMERTHREAD-BVAR-LEAK-ANALYSIS.ch.md` — TimerThread bvar 73B 泄露(同家族不同线程)
- 其他 ubs-comm/ub_test/brpc 泄露文档
- brpc 源码:`src/brpc/server.cpp:315-345,1261`、`src/bvar/variable.cpp:155,943`、`src/bvar/passive_status.h:95,173`
- ub_test 源码:`brpc/example/ub_test/client.cpp:770`
