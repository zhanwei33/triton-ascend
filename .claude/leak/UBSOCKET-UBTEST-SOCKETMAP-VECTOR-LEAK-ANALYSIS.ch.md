# brpc SocketMap vector 泄露分析(bvar/local 抛弃家族之四 bthread)

> **现象**:两条 ASan 报告,同 8B/1、同分配点 `SocketMap::List`(`socket_map.cpp:343`)`vector<ulong>::push_back`,brpc 调用者不同:
> ```
> 报告1(GlobalUpdate bthread):
>     #1 std::__new_allocator::allocate → vector::_M_allocate → _M_realloc_append → emplace_back → push_back
>     #6 brpc::SocketMap::List socket_map.cpp:343
>     #7 brpc::SocketMapList :122
>     #8 GlobalUpdate global.cpp:277
>
> 报告2(RunWatchConnections bthread):
>     #1-#6 同上(push_back in SocketMap::List)
>     #7 brpc::SocketMap::WatchConnections socket_map.cpp:384
>     #8 brpc::SocketMap::RunWatchConnections :368
> ```

## 1. 两条均属 bvar/local 抛弃家族(第四个 bthread)

| 报告 | bthread | 文件 | local vector | 行 | 状态 |
|------|---------|------|-------------|-----|------|
| 报告1 | `GlobalUpdate` | `global.cpp:222` | `conns`(`:244` 声明,`:277` `SocketMapList(&conns)` 填充) | `:277` | `UBSOCKET-UBTEST-GLOBALUPDATE-BVAR-LEAK-ANALYSIS.ch.md` §3 已 anticipated "conns vector(?预期)" |
| 报告2 | **`SocketMap::RunWatchConnections`**(第四个长跑 bthread) | `socket_map.cpp:367` | `main_sockets`(`:373` 声明,`:384` `List(&main_sockets)` 填充) | `:384` | 新 bthread |

`RunWatchConnections` 是 bvar/local 抛弃家族观测到的**第四个长跑 bthread**(前三:`UpdateDerivedVars`/`TimerThread`/`GlobalUpdate`)。

## 2. 泄露对象

- **对象**:`std::vector<SocketId>`(即 `vector<unsigned long>`)的内部堆缓冲。
- **分配点**:`SocketMap::List`(`socket_map.cpp:339-345`)中 `ids->push_back(it->second.socket->id())`(`:343`)触发 `_M_realloc_append` → `_M_allocate`。
- **大小**:8 字节 = 1 × `sizeof(SocketId)==sizeof(unsigned long)==8`(退出时刻 vector 内 1 个元素)。
- **归属 vector**:
  - 报告1:`GlobalUpdate` 的函数级 local `conns`(`global.cpp:244`)
  - 报告2:`WatchConnections` 的函数级 local `main_sockets`(`socket_map.cpp:373`)

## 3. 代码细节

### 报告1:GlobalUpdate 的 conns

`GlobalUpdate`(`global.cpp:222`)循环外声明 `conns`(`:244`),循环内 `SocketMapList(&conns)`(`:277`)填充:

```cpp
static void* GlobalUpdate(void*) {
    ...
    std::vector<SocketId> conns;                    // :244 函数级 local
    while (1) {
        ...
        SocketMapList(&conns);                       // :277 ← frame #8
        for (size_t i = 0; i < conns.size(); ++i) { ... }
        ...
    }
}
```

`SocketMapList`(`socket_map.cpp:122`)转调 `SocketMap::List`(`:339`)`clear()+push_back` 填充 `conns`。

### 报告2:WatchConnections 的 main_sockets(新 bthread)

`SocketMap::RunWatchConnections`(`socket_map.cpp:367-370`)是 brpc `SocketMap` 启动的长跑 bthread,调 `WatchConnections`:

```cpp
void* SocketMap::RunWatchConnections(void* arg) {
    static_cast<SocketMap*>(arg)->WatchConnections();   // :368
    return NULL;
}

void SocketMap::WatchConnections() {
    std::vector<SocketId> main_sockets;                 // :373 函数级 local
    std::vector<SocketId> pooled_sockets;               // :374 同
    std::vector<SocketMapKey> orphan_sockets;           // :375 同
    const uint64_t CHECK_INTERVAL_US = 1000000UL;
    while (bthread_usleep(CHECK_INTERVAL_US) == 0) {    // ESTOP 时 bthread_usleep 返回非0 → 退出
        ...
        List(&main_sockets);                             // :384 ← frame #7,clean+push_back 填充
        ...
    }
}
```

`WatchConnections` 与 `UpdateDerivedVars`/`GlobalUpdate` 同构:循环外 local vector + `while` + `bthread_usleep` + ESTOP 退出。

## 4. 为何泄露(同家族)

与前三个 bthread 的 bvar/vector 泄露完全相同:

- 正常路径:`bthread_usleep` 返回非0(ESTOP)→ `while` 退出 → 函数返回 → 函数级 local vector 析构 → 缓冲释放。**无泄露**。
- 泄露路径:进程退出未干净停止 bthread → 被抛弃 → 函数级 local(`conns` / `main_sockets`/`pooled_sockets`/`orphan_sockets` + bvar 等)全部未析构 → vector 缓冲失主 → ASan 标记。

报告1(8B)= `conns` 缓冲;报告2(8B)= `main_sockets` 缓冲。同家族不同 bthread 不同 local。

## 5. 为何 8B / 1 个

- `SocketMap::List` `push_back` 退出时刻 vector 内 1 个 SocketId(8B)。`clear()` 保 capacity,首次 `push_back` 分配 capacity(通常 1 或翻倍策略),`reserve` 不调。8B = 1 元素的最小 capacity。
- ASan 报 1 个 = 该 vector 的缓冲。`WatchConnections` 的 `pooled_sockets`/`orphan_sockets` 应另有报告(若退出时刻非空);`GlobalUpdate` 的 `conns` 即本报告1。

## 6. 触发条件

- brpc `SocketMap` 启动 `RunWatchConnections` bthread(任何 brpc 程序,含 dummy server)
- `GlobalUpdate` bthread 运行(同前)
- 进程退出未干净停止 bthread → 抛弃 → local vector 未析构
- 与 UB 配置无关,纯 brpc 库行为

## 7. 修复方案

**与 bvar/local 抛弃家族共用**,brpc 侧确保长跑 bthread 干净停止:

### 方案 A【根治】brpc 全局退出干净 join 全部长跑 bthread

brpc 全局退出(`bthread_stop_world`/atexit)对**四个**长跑 bthread(`UpdateDerivedVars`/`TimerThread`/`GlobalUpdate`/`RunWatchConnections`)统一发送 ESTOP + `bthread_join`/`pthread_join`,确保 `while` 退出 → 函数返回 → 全部函数级 local 析构(vector 缓冲 + bvar string + FileWatcher 等释放)。**一次覆盖家族全部报告**。

### 方案 B【bvar 专项】bvar registry atexit 钩子统一 Unexpose

仅覆盖 bvar string,不覆盖 vector 缓冲(本两报告是 vector,不适用)。需配合方案 A。

### 方案 C【防御】local 改静态/成员

`WatchConnections` 的 `main_sockets`/`pooled_sockets`/`orphan_sockets`、`GlobalUpdate` 的 `conns` 改 `static` 或 `SocketMap`/`Server` 成员,生命周期与进程/对象一致,不依赖 bthread 析构时机。

推荐**方案 A**(根治全部 local 类型,含 vector 与 bvar)。

## 8. ubs-comm 侧动作

**无。** 与 ubsocket/umq 适配层无任何代码关联。属 brpc 库生命周期问题。

## 9. 关键代码位置索引

| 位置 | 作用 | 泄露相关性 |
|------|------|-----------|
| `socket_map.cpp:339-345` | `SocketMap::List` `push_back` | **两条报告的共同分配点** |
| `socket_map.cpp:122` | `SocketMapList` 转调 `List` | 报告1 调用链 |
| `global.cpp:244` | `conns` vector 声明 | 报告1 local |
| `global.cpp:277` | `SocketMapList(&conns)` | 报告1 frame #8 |
| `socket_map.cpp:367-370` | `RunWatchConnections` bthread 入口 | 报告2 bthread(第四个) |
| `socket_map.cpp:372-389` | `WatchConnections` 函数级 local + while | 报告2 |
| `socket_map.cpp:373-375` | `main_sockets`/`pooled_sockets`/`orphan_sockets` | 报告2 local(本报告 main_sockets) |
| `socket_map.cpp:384` | `List(&main_sockets)` | 报告2 frame #7 |

## 10. 与其他泄露的关系

| 维度 | 本两报告(SocketMap vector 8B×2) | bvar 家族(前三 bthread) | UmqSocket 析构链 | AsyncEventPoll | TX Event | RX | RespClosure/done | RpcMeta |
|------|--------------------------------|------------------------|----------------|---------------|----------|----|------------------|---------|
| 归属 | brpc SocketMap + GlobalUpdate | brpc Server/bthread/bvar | ubsocket core | ubsocket core | ubsocket umq | ubsocket umq | ub_test | brpc/protobuf |
| 类别 | bthread 抛弃致 vector 未析构 | 同(bvar local + vector) | 析构不 delete | 析构不清 map | 退出未释放 | buffer 不回流 | 闭包/drain | 解析逃逸 |
| 与 bvar 家族关系 | **同家族(第四 bthread RunWatchConnections + GlobalUpdate conns)** | 自身 | 独立 | 独立 | 独立 | 独立 | 独立 | 独立 |
| ubs-comm 修复 | ❌ | ❌ | ✓ | ✓ | ✓ | ✓ | ❌ | ❌ |

本两报告属 bvar/local 抛弃家族,引入**第四个长跑 bthread `RunWatchConnections`**,与 `GlobalUpdate` 的 `conns` vector 同根,共用修复方案(方案 A:brpc 全局退出 join 四个 bthread)。ubs-comm 无修复点。

## 参考

- `UBSOCKET-UBTEST-GLOBALUPDATE-BVAR-LEAK-ANALYSIS.ch.md` — GlobalUpdate bvar(§3 已 anticipated `conns` vector,即报告1)
- `UBSOCKET-UBTEST-BVAR-ABANDONED-BTHREAD-CONSOLIDATED-LEAK-ANALYSIS.ch.md` — bvar/local 抛弃家族汇总(本报告扩展至第四 bthread)
- `UBSOCKET-UBTEST-DUMMY-SERVER-*-LEAK-ANALYSIS.ch.md` — UpdateDerivedVars bvar/vector 家族
- `UBSOCKET-UBTEST-TIMERTHREAD-BVAR-LEAK-ANALYSIS.ch.md` — TimerThread bvar 家族
- 其他 ubs-comm/ub_test/brpc 泄露文档
- brpc 源码:`src/brpc/socket_map.cpp:122,339-345,367-389`、`src/brpc/global.cpp:222-281`
