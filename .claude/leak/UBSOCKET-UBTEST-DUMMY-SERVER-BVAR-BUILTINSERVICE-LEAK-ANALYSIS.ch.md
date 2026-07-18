# brpc Dummy Server bvar nbuiltinservice_st 字符串泄露分析(UpdateDerivedVars bvar 家族之四)

> **现象**:ASan 报告
> ```
> Direct leak of 46 byte(s) in 1 object(s) allocated from:
>     #1 std::__cxx11::basic_string::reserve
>     #2 bvar::Variable::expose_impl variable.cpp:148
>     #3 bvar::PassiveStatus<int>::expose_impl passive_status.h:173
>     #4 bvar::Variable::expose_as variable.h:162
>     #5 bvar::PassiveStatus<int>::PassiveStatus passive_status.h:95
>     #6 brpc::Server::UpdateDerivedVars server.cpp:342
>     #7 bthread::TaskGroup::task_runner
> ```
>
> 本文确认其为 `UBSOCKET-UBTEST-DUMMY-SERVER-BVAR-*-LEAK-ANALYSIS.ch.md`(77B `nconn_st` / 71B `nservice_st` / 63B `start_time_st`)的**同 bthread 同次抛弃伴生**,即 `start_time_st` 文档 §1 已 anticipated 的 `nbuiltinservice_st` 报告。

## 1. UpdateDerivedVars bvar local 全家谱(已观测 4/6)

`UpdateDerivedVars`(`server.cpp:315`)循环外声明 6 个 bvar local(`server.cpp:329-345`),`expose_as` 注册暴露名 string(超 SSO 堆分配)。bthread 被抛弃时全部未析构 → 各自 string 失主。已观测 4 个:

| local | 类型 | 行 | 暴露名 | ASan 大小 | 分配点 | 状态 |
|-------|------|-----|--------|-----------|--------|------|
| `uptime_st` | `PassiveStatus<timeval>` | :329-330 | `uptime` | ? | `to_underscored_name` | 同家族(应有报告) |
| `start_time_st` | `PassiveStatus<std::string>` | :332-333 | `start_time` | 63B | `to_underscored_name:943` | 已文档 |
| `nconn_st` | `PassiveStatus<int32_t>` | :335-336 | `connection_count` | 77B | `to_underscored_name:943` | 已文档 |
| `nservice_st` | `PassiveStatus<int32_t>` | :338-339 | `service_count` | 71B | `to_underscored_name:943` | 已文档 |
| **`nbuiltinservice_st`** | **`PassiveStatus<int32_t>`** | **:341-342** | **`builtin_service_count`** | **46B** | **`expose_impl:148`** | **本报告** |
| `nsessiondata_st` | `Passive<bvar::Vector<unsigned,2>>` | :344-345 | (无 expose_as) | ? | — | 同家族 |

## 2. 泄露对象

- **对象**:`std::__cxx11::basic_string` 堆缓冲(超 SSO 32B → 堆分配),bvar 暴露名。
- **分配点**:`bvar::Variable::expose_impl`(`variable.cpp:148`)`std::string::reserve`。
- **大小**:46 字节。
- **归属 string**:`<prefix>_builtin_service_count` 经 `expose_impl` 构造的暴露名。

### 分配点差异(为何 46B 报在 `expose_impl:148` 而非 `to_underscored_name:943`)

前 3 个报告(nconn_st/nservice_st/start_time_st)的 ASan 栈顶 `reserve` 在 `to_underscored_name`(`variable.cpp:943`);本报告栈顶在 `expose_impl`(`variable.cpp:148`)。说明 `expose_impl` 内有**两处** `std::string::reserve`/构造:

1. `to_underscored_name`(`:943`)——把 `name`(`builtin_service_count`)转下划线形式,产生一个 string(前 3 报告命中此);
2. `expose_impl`(`:148`)——把 `prefix` + 转换后的 name 拼成完整暴露名 `<prefix>_builtin_service_count` 并存储,产生另一个 string(本报告命中此)。

具体命中哪个,取决于 string 长度是否超 SSO 阈值及 `reserve` 时机。**两处 string 同属一次 `expose_as` 调用,均归 `nbuiltinservice_st` local 所有**,泄露机理与其他 3 个完全一致(bthread 抛弃 → local 未析构 → bvar 未 Unexpose → string 失主)。

> 注:前 3 个 local 的 `expose_impl:148` 那个 string 应也有对应 ASan 报告(未粘贴),或其长度恰好不超 SSO 不触发堆分配。本 46B 是 `nbuiltinservice_st` 在 `expose_impl:148` 的那份 string。

## 3. 代码细节

`UpdateDerivedVars`(`server.cpp:315`)循环外(`server.cpp:341-342`):

```cpp
bvar::PassiveStatus<int32_t> nbuiltinservice_st(
    prefix, "builtin_service_count", GetBuiltinServiceCount, server);   // :341-342 ← frame #6 本报告
```

`PassiveStatus` 构造 → `expose_as` → `expose_impl`(`variable.cpp:148` 拼暴露名 string + `:943` `to_underscored_name` 转换)→ 注册到 bvar registry。bthread 正常退出(ESTOP `return NULL`)时 local 析构 → bvar `Unexpose` 释放两份 string。

## 4. 为何泄露(同前 3 个)

- 正常路径:`Server::Stop()` → `bthread_usleep` 返回 ESTOP(`server.cpp:386-388` `return NULL`)→ 函数级 local 离开作用域析构 → bvar `Unexpose` 释放 string。**无泄露**。
- 泄露路径:client 退出未对 dummy server 干净 `Stop()`,`UpdateDerivedVars` bthread 被抛弃 → 函数级 local(`uptime_st`/`start_time_st`/`nconn_st`/`nservice_st`/`nbuiltinservice_st`/`nsessiondata_st` + `conns`/`internal_conns` vector)全部未析构 → 各自暴露名 string + vector 缓冲失主 → ASan 逐个标记。

## 5. 为何 46B / 1 个

- `expose_impl:148` 构造的完整暴露名 `<prefix>_builtin_service_count` 长度超 SSO 32B → 堆分配,capacity 46B。
- ASan 报 1 个 = `nbuiltinservice_st` 在 `expose_impl:148` 的暴露名 string。
- 与 `nconn_st`(77B)/`nservice_st`(71B)/`start_time_st`(63B)/vector(80B)是**同 bthread 同次抛弃的多个独立对象**,应一并观察。

## 6. 触发条件

与前 3 个完全一致:进程启动 brpc Server(含 dummy server)→ `UpdateDerivedVars` bthread 运行 → 进程退出未干净 `Server::Stop()` → bthread 抛弃。与 UB 配置无关。

## 7. 修复方案

**与前 3 个 bvar 报告共用**,修复一次消除全部 `UpdateDerivedVars` bvar local 泄露(63B start_time_st + 71B nservice_st + 77B nconn_st + 46B nbuiltinservice_st + uptime_st + nsessiondata_st + vector 80B):

### 方案 1【ub_test 侧】退出前显式 Stop dummy server

`ub_test/client.cpp:770` `StartDummyServerAt` 返回的 Server* 保存,`main` 返回前 `server->Stop(0)` + `server->Join()` → bthread ESTOP 退出 → 函数级 local 析构 → bvar Unexpose + vector 释放。**一次消除全家谱所有 string/vector 泄露**。

### 方案 2【brpc 侧】Server 析构/Stop 保证 bthread join

`~Server`/`Stop` 中 `bthread_join(_derived_vars_bthread)`,确保 bthread 退出且 local 析构后再返回。根治。

### 方案 3【防御】bvar local 改静态/Server 成员

`UpdateDerivedVars` 的 bvar local 改 `static` 或 Server 成员,生命周期与进程/Server 一致,避免依赖 bthread 析构时机。

## 8. ubs-comm 侧动作

**无。** 与 ubsocket/umq 适配层无任何代码关联。属 brpc 生命周期 + ub_test dummy server 启停问题。

## 9. 关键代码位置索引

| 位置 | 作用 | 泄露相关性 |
|------|------|-----------|
| `server.cpp:341-342` | `nbuiltinservice_st` PassiveStatus ctor | **frame #6,泄露触发** |
| `server.cpp:329-345` | 全部 PassiveStatus local | 同家族(uptime/start_time/nconn/nservice/nsessiondata) |
| `passive_status.h:95` | `PassiveStatus` ctor → `expose_as` | 调用链 |
| `variable.h:162` | `expose_as` → `expose_impl` | 调用链 |
| `variable.cpp:148` | `expose_impl` 拼暴露名 string | **本报告 string 堆分配点** |
| `variable.cpp:943` | `to_underscored_name` reserve | 前 3 报告 string 堆分配点 |
| `server.cpp:315-345` | `UpdateDerivedVars` 函数级 local | bthread 抛弃致不析构 |

## 10. 与其他泄露的关系

| 维度 | 本泄露(nbuiltinservice_st 46B) | nconn_st 77B / nservice_st 71B / start_time_st 63B | dummy vector 80B | TimerThread bvar 73B | GlobalUpdate bvar 61B | TX Event | RespClosure/done | RX | 析构链家族 | RpcMeta |
|------|--------------------------------|--------------------------------------------------|-----------------|---------------------|----------------------|----------|------------------|----|-----------|---------|
| 归属 | brpc Server + ub_test dummy | 同 | 同 | brpc bthread/bvar | brpc 全局 init | ubsocket umq | ub_test | ubsocket umq | ubsocket 核心 | brpc/protobuf |
| 类别 | bvar local 未析构 | 同 | bthread 抛弃致 vector 未析构 | 同(不同线程) | 同(第三线程) | 退出未释放 | 闭包/drain | buffer 不回流 | 析构不 delete | 解析逃逸 |
| 与 nconn_st 等关系 | **同 bthread 同次(第四 local)** | 自身 | 同 bthread 抛弃类 | 同家族不同线程 | 同家族第三线程 | 独立 | 独立 | 独立 | 独立 | 独立 |
| ubs-comm 修复 | ❌ | ❌ | ❌ | ❌ | ❌ | ✓ | ❌ | ✓ | ✓ | ❌ |

本泄露与 `nconn_st`(77B)/`nservice_st`(71B)/`start_time_st`(63B)是**同 bthread 同次抛弃的 PassiveStatus local 之四**,与 dummy vector(80B)是**同 bthread 不同 local 类型**,均 `UpdateDerivedVars` bthread 抛弃所致,共用修复方案。

## 参考

- `UBSOCKET-UBTEST-DUMMY-SERVER-BVAR-LEAK-ANALYSIS.ch.md` — `nconn_st` 77B(同 bthread 同次)
- `UBSOCKET-UBTEST-DUMMY-SERVER-BVAR-SERVICE-LEAK-ANALYSIS.ch.md` — `nservice_st` 71B
- `UBSOCKET-UBTEST-DUMMY-SERVER-BVAR-STARTTIME-LEAK-ANALYSIS.ch.md` — `start_time_st` 63B(§1 已 anticipated 本报告)
- `UBSOCKET-UBTEST-DUMMY-SERVER-LEAK-ANALYSIS.ch.md` — dummy vector 80B(同 bthread 抛弃类)
- `UBSOCKET-UBTEST-TIMERTHREAD-BVAR-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UBTEST-GLOBALUPDATE-BVAR-LEAK-ANALYSIS.ch.md` — 同家族不同线程
- 其他 ubs-comm/ub_test/brpc 泄露文档
- brpc 源码:`src/brpc/server.cpp:315-345,1261`、`src/bvar/variable.cpp:148,155,943`、`src/bvar/passive_status.h:95,173`
- ub_test 源码:`brpc/example/ub_test/client.cpp:770`
