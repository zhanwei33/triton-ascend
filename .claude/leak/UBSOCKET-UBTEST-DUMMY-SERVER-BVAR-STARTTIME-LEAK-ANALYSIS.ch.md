# brpc Dummy Server bvar start_time_st 字符串泄露分析(UpdateDerivedVars bvar 家族之三)

> **现象**:ASan 报告
> ```
> Direct leak of 63 byte(s) in 1 object(s) allocated from:
>     #1 std::__cxx11::basic_string::reserve
>     #2 bvar::to_underscored_name variable.cpp:943
>     #3 bvar::Variable::expose_impl variable.cpp:155
>     #4 bvar::Variable::expose_as variable.h:162
>     #5 bvar::PassiveStatus<std::string>::PassiveStatus(prefix, name, void(*)(ostream&,void*), void*) passive_status.h:212
>     #6 brpc::Server::UpdateDerivedVars server.cpp:333
>     #7 bthread::TaskGroup::task_runner
> ```
>
> 本文确认其为 `UBSOCKET-UBTEST-DUMMY-SERVER-BVAR-LEAK-ANALYSIS.ch.md`(77B `nconn_st`)/`UBSOCKET-UBTEST-DUMMY-SERVER-BVAR-SERVICE-LEAK-ANALYSIS.ch.md`(71B `nservice_st`)的**同 bthread 同次抛弃伴生**,仅 `PassiveStatus` local 不同(`start_time_st` @ `:333`,且是 `PassiveStatus<std::string>` ostream 回调变体)。

## 1. UpdateDerivedVars bvar local 全家谱

`UpdateDerivedVars`(`server.cpp:315`)循环外声明一串 bvar local(`server.cpp:329-345`),每个 `expose_as` 注册暴露名 string(超 SSO 堆分配)。bthread 被抛弃时全部未析构 → 各自 string 失主。已观测到的成员:

| local | 类型 | 行 | 暴露名 | ASan 大小 | 状态 |
|-------|------|-----|--------|-----------|------|
| `uptime_st` | `PassiveStatus<timeval>` | :329-330 | `uptime` | ? | 同家族(应有报告) |
| **`start_time_st`** | **`PassiveStatus<std::string>`** | **:332-333** | **`start_time`** | **63B** | **本报告** |
| `nconn_st` | `PassiveStatus<int32_t>` | :335-336 | `connection_count` | 77B | 已文档 |
| `nservice_st` | `PassiveStatus<int32_t>` | :338-339 | `service_count` | 71B | 已文档 |
| `nbuiltinservice_st` | `PassiveStatus<int32_t>` | :341-342 | `builtin_service_count` | ? | 同家族(应有报告) |
| `nsessiondata_st` | `PassiveStatus<bvar::Vector<unsigned,2>>` | :344-345 | (无 expose_as,默认) | ? | 同家族 |

大小递减与暴露名长度一致:`start_time`(10字符)< `service_count`(13)< `connection_count`(16),故 63B < 71B < 77B。

## 2. 泄露对象

- **对象**:`std::__cxx11::basic_string` 堆缓冲(超 SSO 32B → 堆分配),bvar 暴露名。
- **分配点**:`bvar::to_underscored_name`(`variable.cpp:943`)`reserve`,经 `PassiveStatus<std::string>::expose_impl`(`passive_status.h:212` ostream 回调变体)→ `expose_impl` → `expose_as` → `PassiveStatus` 构造。
- **大小**:63 字节。
- **归属 string**:`<prefix>_start_time` 经 `to_underscored_name` 转换后的暴露名。

## 3. 代码细节

`UpdateDerivedVars`(`server.cpp:315`)循环外:

```cpp
bvar::PassiveStatus<timeval> uptime_st(
    prefix, "uptime", GetUptime, (void*)(intptr_t)start_us);          // :329-330

bvar::PassiveStatus<std::string> start_time_st(
    prefix, "start_time", PrintStartTime, server);                     // :332-333 ← frame #6 本报告

bvar::PassiveStatus<int32_t> nconn_st(
    prefix, "connection_count", GetConnectionCount, server);          // :335-336 — 77B
bvar::PassiveStatus<int32_t> nservice_st(
    prefix, "service_count", GetServiceCount, server);                 // :338-339 — 71B
...
```

`start_time_st` 是 `PassiveStatus<std::string>`(值类型为 string,回调 `PrintStartTime` 用 ostream 输出),与 `nconn_st`/`nservice_st`(int 值 + int 回调)是不同的 PassiveStatus 模板特化,但**expose 链路与泄露机理完全一致**。

## 4. 为何泄露(同 nconn_st/nservice_st)

与 `UBSOCKET-UBTEST-DUMMY-SERVER-BVAR-LEAK-ANALYSIS.ch.md` §4 完全相同:

- 正常路径:`Server::Stop()` → `bthread_usleep` 返回 ESTOP(`server.cpp:386-388` `return NULL`)→ 函数级 local 离开作用域析构 → bvar `Unexpose` 释放 string。**无泄露**。
- 泄露路径:client 退出未对 dummy server 干净 `Stop()`,`UpdateDerivedVars` bthread 被抛弃 → 函数级 local(`uptime_st`/`start_time_st`/`nconn_st`/`nservice_st`/...+ `conns`/`internal_conns` vector)全部未析构 → 各自暴露名 string(63B/71B/77B/...)+ vector 缓冲(80B)等失主 → ASan 逐个标记。

## 5. 为何 63B / 1 个

- `to_underscored_name(prefix, "start_time")` 结果长度超 SSO 32B → 堆分配,capacity 63B。
- ASan 报 1 个 = `start_time_st` 的暴露名 string(frame #6 `:333` 命中)。
- 与 77B(`nconn_st`)/71B(`nservice_st`)/80B(vector)是**同 bthread 同次抛弃的多个独立对象**,应一并观察。

## 6. 触发条件

与 nconn_st/nservice_st 完全一致:进程启动 brpc Server(含 dummy server)→ `UpdateDerivedVars` bthread 运行 → 进程退出未干净 `Server::Stop()` → bthread 抛弃。与 UB 配置无关。

## 7. 修复方案

**与 `UBSOCKET-UBTEST-DUMMY-SERVER-BVAR-LEAK-ANALYSIS.ch.md` §7、`UBSOCKET-UBTEST-DUMMY-SERVER-BVAR-SERVICE-LEAK-ANALYSIS.ch.md` §7 完全共用**,修复一次消除全部 `UpdateDerivedVars` bvar local 泄露(63B start_time_st + 71B nservice_st + 77B nconn_st + uptime_st/nbuiltinservice_st + vector 80B):

### 方案 1【ub_test 侧】退出前显式 Stop dummy server

`ub_test/client.cpp:770` `StartDummyServerAt` 返回的 Server* 保存,`main` 返回前 `server->Stop(0)` + `server->Join()` → bthread ESTOP 退出 → 函数级 local 析构 → bvar Unexpose + vector 释放。**一次消除全家谱所有 string/vector 泄露**。

### 方案 2【brpc 侧】Server 析构/Stop 保证 bthread join

`~Server`/`Stop` 中 `bthread_join(_derived_vars_bthread)`,确保 bthread 退出且 local 析构后再返回。根治。

### 方案 3【防御】bvar local 改静态/Server 成员

`UpdateDerivedVars` 的 `uptime_st`/`start_time_st`/`nconn_st` 等改为 `static` 或 Server 成员,生命周期与进程/Server 一致,避免依赖 bthread 析构时机。

## 8. ubs-comm 侧动作

**无。** 与 ubsocket/umq 适配层无任何代码关联。属 brpc 生命周期 + ub_test dummy server 启停问题。

## 9. 关键代码位置索引

| 位置 | 作用 | 泄露相关性 |
|------|------|-----------|
| `server.cpp:332-333` | `start_time_st` PassiveStatus<std::string> ctor | **frame #6,泄露触发** |
| `server.cpp:329-345` | 全部 bvar PassiveStatus local | 同家族(uptime/nconn/nservice/...) |
| `passive_status.h:212` | `PassiveStatus<std::string>` ctor(ostream 变体)→ `expose_as` | 调用链 |
| `variable.h:162` | `expose_as` → `expose_impl` | 调用链 |
| `variable.cpp:155` | `expose_impl` → `to_underscored_name` | 调用链 |
| `variable.cpp:943` | `to_underscored_name` `reserve` | **string 堆分配点** |
| `server.cpp:315-345` | `UpdateDerivedVars` 函数级 local | bthread 抛弃致不析构 |

## 10. 与其他泄露的关系

| 维度 | 本泄露(start_time_st 63B) | nconn_st 77B / nservice_st 71B | dummy vector 80B | TimerThread bvar 73B | TX Event | RespClosure/done | RX | 析构链家族 | RpcMeta |
|------|---------------------------|-------------------------------|-----------------|---------------------|----------|------------------|----|-----------|---------|
| 归属 | brpc Server + ub_test dummy | 同 | 同 | brpc bthread/bvar | ubsocket umq | ub_test | ubsocket umq | ubsocket 核心 | brpc/protobuf |
| 类别 | bvar local 未析构 | 同 | bthread 抛弃致 vector 未析构 | 同(不同线程) | 退出未释放 | 闭包/drain | buffer 不回流 | 析构不 delete | 解析逃逸 |
| 与 nconn_st/nservice_st 关系 | **同 bthread 同次** | 自身 | 同 bthread 抛弃类 | 同家族不同线程 | 独立 | 独立 | 独立 | 独立 | 独立 |
| ubs-comm 修复 | ❌ | ❌ | ❌ | ❌ | ✓ | ❌ | ✓ | ✓ | ❌ |

本泄露与 `nconn_st`(77B)/`nservice_st`(71B)是**同 bthread 同次抛弃的 PassiveStatus local 之三**,与 dummy vector(80B)是**同 bthread 不同 local 类型**,均 `UpdateDerivedVars` bthread 抛弃所致,共用修复方案。`TimerThread` bvar(73B)是同家族不同线程。

## 参考

- `UBSOCKET-UBTEST-DUMMY-SERVER-BVAR-LEAK-ANALYSIS.ch.md` — `nconn_st` 77B(同 bthread 同次,伞形修复合并)
- `UBSOCKET-UBTEST-DUMMY-SERVER-BVAR-SERVICE-LEAK-ANALYSIS.ch.md` — `nservice_st` 71B(同 bthread 同次)
- `UBSOCKET-UBTEST-DUMMY-SERVER-LEAK-ANALYSIS.ch.md` — dummy vector 80B(同 bthread 抛弃类)
- `UBSOCKET-UBTEST-TIMERTHREAD-BVAR-LEAK-ANALYSIS.ch.md` — TimerThread bvar 73B(同家族不同线程)
- 其他 ubs-comm/ub_test/brpc 泄露文档
- brpc 源码:`src/brpc/server.cpp:315-345,1261`、`src/bvar/variable.cpp:155,943`、`src/bvar/passive_status.h:173,212`
- ub_test 源码:`brpc/example/ub_test/client.cpp:770`
