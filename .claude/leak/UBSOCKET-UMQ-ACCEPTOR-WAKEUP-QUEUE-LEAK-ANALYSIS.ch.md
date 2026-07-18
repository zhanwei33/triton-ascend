# UBSocket Acceptor wakeup_event queue 级联泄露分析(Acceptor 泄露之子)

> **现象**:ASan 报告(**Indirect leak**——间接泄露,挂在父对象 `Acceptor` 之下)
> ```
> Indirect leak of 512 byte(s) in 1 object(s) allocated from:
>     #1 std::__new_allocator<int>::allocate
>     #2 allocator_traits::allocate → _Deque_base::_M_allocate_node stl_deque.h:583
>     #3 _M_create_nodes stl_deque.h:684
>     #4 _M_initialize_map stl_deque.h:460
>     #5 _Deque_base::_Deque_base() stl_deque.h:460
>     #6 std::deque<int>::deque() stl_deque.h:855
>     #7 std::queue<int, std::deque<int>>::queue() stl_queue.h:167
>     #8 UbsocketWakeupEvent::UbsocketWakeupEvent() ubsocket_wakeup_event.cpp:16
>     #9 Acceptor::Acceptor(sock, acceptorOps)
>     #10 SocketBase::Create ubsocket_socket.cpp:55
>     ... → brpc::Socket::Connect socket.cpp:1334 → PerformanceTest::Init
> ```
>
> 本文确认其为 `UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md`(Acceptor 288B direct)的**级联子对象**——`Acceptor::wakeup_event_`(`UbsocketWakeupEvent`)内部 `ready_event_queue_`(`std::queue<int>`)的 deque 节点。

## 1. 级联关系(Acceptor 泄露的子对象)

`Acceptor`(`SocketBase::acceptor_`,288B,direct)持多个子对象,`~Acceptor` 从不执行(`acceptor_` 全仓 0 处 `delete`,见 `UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md` §5)→ 全部级联泄露。本报告是其中之一:

| Acceptor 子对象 | 成员 | 分配点 | 大小 | ASan 报告 |
|----------------|------|--------|------|-----------|
| `acceptor_ops_` | `Ref<AcceptorOps>` | Acceptor ctor | 级联 | 应有报告(AcceptorOps) |
| `ubSocket_async_accept_info.lock` | `u_mutex_t*` | Acceptor ctor(`:62`) | 48B | Acceptor 文档 §6(应有报告) |
| `ubSocket_async_accept_info.ready_queue` | `std::queue<tuple<int,sockaddr,socklen_t>>` | Acceptor ctor | 级联 deque 节点 | 应有报告 |
| **`wakeup_event_`** | **`UbsocketWakeupEvent`** | **Acceptor ctor 成员初始化** | **含本 512B 级联** | **本报告路径** |
| └ `wakeup_event_.ready_event_mutex_` | `u_mutex_t*` | `ubsocket_wakeup_event.cpp:18` create | 48B | 应有报告(级联 mutex) |
| └ **`wakeup_event_.ready_event_queue_`** | **`std::queue<int, std::deque<int>>`** | **`ubsocket_wakeup_event.cpp:16` ctor** | **512B(deque 节点)** | **本报告(indirect)** |
| └ `wakeup_event_.readyEventFd_` | `int`(eventfd) | `Initialize` 时 eventfd | — | fd 泄露(ASan 不跟踪) |

**Indirect leak** 标记说明 ASan 判定本 512B 被层级1 `Acceptor` 288B direct 间接持有(经 `wakeup_event_` → `ready_event_queue_`),与级联分析一致。

## 2. 泄露对象

- **对象**:`std::deque<int>` 的一个内部 chunk 节点(`std::queue<int, std::deque<int>>::ready_event_queue_` 的底层 deque)。
- **分配点**:`UbsocketWakeupEvent::UbsocketWakeupEvent()`(`ubsocket_wakeup_event.cpp:16`)的成员默认初始化触发 `std::queue<int>::queue()`(默认 ctor)→ `std::deque<int>::deque()` → `_M_initialize_map(0)` → `_M_create_nodes` → `_M_allocate_node` → `new[]` 一个 deque chunk。
- **大小**:512 字节。libstdc++ `std::deque<int>` 的 chunk 大小 = `512 / sizeof(int) = 128` 个 int/节点。**默认构造即分配 1 个 chunk**(libstdc++ `_M_initialize_map` 即使 num_elements=0 也分配最少 1 个节点),故空 queue 也占 512B。
- **归属**:`UbsocketWakeupEvent::ready_event_queue_`(`std::queue<int>`,见 `ubsocket_wakeup_event.cpp:72-73` `.empty()`/`.pop()` 用法),经 `Acceptor::wakeup_event_`(`ubsocket_wakeup_event.h` 成员,`ubsocket_socket_acceptor.h:115`)归属 `SocketBase::acceptor_`。

## 3. 代码细节

`UbsocketWakeupEvent`(`ubsocket_wakeup_event.cpp:16-19`)ctor:

```cpp
UbsocketWakeupEvent::UbsocketWakeupEvent() : epollFd_(-1), readyEventFd_(-1), ready_event_mutex_(nullptr)
{
    ready_event_mutex_ = LockRegistry::LOCK_OPS.create(LT_EXCLUSIVE);   // ← 级联 mutex(应有报告)
    // ready_event_queue_ 默认构造 → deque 分配 1 个 512B chunk ← 本报告
}
```

`~UbsocketWakeupEvent`(`ubsocket_wakeup_event.cpp:21-26`)会清理:

```cpp
UbsocketWakeupEvent::~UbsocketWakeupEvent() {
    if (ready_event_mutex_ != nullptr) {
        LockRegistry::LOCK_OPS.destroy(ready_event_mutex_);            // ← 销毁 mutex
        ready_event_mutex_ = nullptr;
    }
    // ready_event_queue_ 析构 → ~deque → 释放 chunk(本 512B)
    // readyEventFd_ 关闭(若 Initialize 过)
}
```

但 `~UbsocketWakeupEvent` **从不执行**——`Acceptor::wakeup_event_` 是值成员,需 `~Acceptor` 触发其析构,而 `~Acceptor` 因 `acceptor_` 无 delete 从不执行。

## 4. 为何泄露(同 Acceptor 家族,级联)

`UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md` §5 已确认:`SocketBase::acceptor_` 全仓 **0 处 `delete`**,`~SocketBase`/`UnInitialize`/`~UmqSocket` 均不 delete。

级联效应:
- `acceptor_` 不 delete → `~Acceptor`(`ubsocket_socket_acceptor.cpp:300-305`)从不执行
- `~Acceptor` 不执行 → `wakeup_event_`(`UbsocketWakeupEvent` 值成员)析构不触发
- `~UbsocketWakeupEvent` 不执行 → `ready_event_queue_`(`std::queue<int>`)析构不触发
- `~queue<int>` / `~deque<int>` 不执行 → deque chunk(512B)+ 内部 map 不释放 → 泄露
- 同时 `ready_event_mutex_` 也不 destroy(级联 mutex,应有独立报告)

**根因与 Acceptor 泄露完全相同**:`acceptor_` 无 delete → Acceptor 全部子对象(含 `wakeup_event_` 及其内部 queue/mutex/eventfd)级联失主。本 512B 是其中 indirect 之一。

## 5. 为何 512B / 1 个

- libstdc++ `std::deque<int>` chunk = 512B(128 ints),默认构造分配 1 个 chunk。
- 1 个 = 单 Acceptor 的 `wakeup_event_.ready_event_queue_` 的 deque chunk。本次仅 1 个 UmqSocket 被销毁(同 Acceptor 288B/1 的"1 个")→ 1 个 512B chunk 泄露。
- queue 在运行时可能增长(更多 chunk),但本报告仅 1 个 512B,说明退出时刻 queue 基本空(仅默认 chunk)。

## 6. 触发条件

与 Acceptor 家族完全一致:任一 UmqSocket 被销毁(socket close 或 `ubsocket_uninit` 的 `ReleaseAll`)即泄露 Acceptor + 全部级联子对象(含本 512B queue chunk + 级联 mutex)。与 UB 配置无关。

## 7. 修复方案

**与 `UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md` §7、`UBSOCKET-UMQ-CONNECTOR-LEAK-ANALYSIS.ch.md` §7 等共用伞形修复**——`~SocketBase` 补 `delete acceptor_`:

```cpp
~SocketBase() override {
    delete acceptor_;           // ← 触发级联:~Acceptor → ~UbsocketWakeupEvent → ~queue<int> → 释放 512B chunk
    acceptor_ = nullptr;
    delete connector_;
    connector_ = nullptr;
    // ... tx_/rx_/rxQueue/mutex_ 见各自文档
    if (GlobalSetting::UBS_TRACE_ENABLED) { ... }
}
```

`delete acceptor_` → `~Acceptor` → 成员析构 → `~UbsocketWakeupEvent` → `~queue<int>` → `~deque<int>` 释放 512B chunk + destroy `ready_event_mutex_` + 关 `readyEventFd_`。**一处 delete 覆盖 Acceptor 全部级联子对象**(acceptor_ops_/mutex/ready_queue/wakeup_event_ 及其内部)。

### 与伞形析构链 PR 的关系

本修复是伞形析构链 PR 的 `acceptor_` 子项,与 `delete connector_`、`~DataTx`/`~DataRx` delete tx_ops_/rx_ops_、`~UmqSocket` delete rxQueue + destroy mutex_ 一并合并。`~Acceptor` 内部还需补 `LockRegistry::LOCK_OPS.destroy(ubSocket_async_accept_info.lock)`(Acceptor 文档 §6 方案2)。

## 8. 验证

修复后 ASan 重跑:本 512B indirect + Acceptor 288B direct + Acceptor 级联(mutex/ready_queue/ready_event_mutex_ 等)**同时消失**。可用小 `thread_num` 短测验证。

## 9. 关键代码位置索引

| 位置 | 作用 | 泄露相关性 |
|------|------|-----------|
| `ubsocket_wakeup_event.cpp:16-19` | `UbsocketWakeupEvent` ctor,`ready_event_queue_` 默认构造 | **本报告 512B 分配触发(frame #8)** |
| `ubsocket_wakeup_event.cpp:21-26` | `~UbsocketWakeupEvent` 清 queue/mutex(从不被调) | 级联释放入口(失效) |
| `ubsocket_wakeup_event.cpp:72-73` | `ready_event_queue_.empty()/.pop()` 用法 | queue 成员确认 |
| `ubsocket_wakeup_event.h` | `UbsocketWakeupEvent` 类,`ready_event_queue_` 成员 | 归属 |
| `ubsocket_socket_acceptor.h:115` | `Acceptor::wakeup_event_` 值成员 | 级联归属 Acceptor |
| `ubsocket_socket_acceptor.h:60-63` | `Acceptor` ctor 初始化 `wakeup_event_` | frame #9 |
| `ubsocket_socket.cpp:55` | `new Acceptor` | 层级1 direct(Acceptor 文档) |
| `ubsocket_socket.h:44-52,94` | `~SocketBase` 不 delete `acceptor_` | 根因 |

## 10. 与其他泄露的关系

| 维度 | 本泄露(wakeup queue 512B) | Acceptor 288B / Connector 16B | rxQueue 级联(48B+128KB+2056B) | UmqTxOps/RxOps 72/96B | mutex 48B | TX Event | RX | RespClosure/done | bvar 家族 | RpcMeta | AsyncEventPoll |
|------|--------------------------|------------------------------|----------------------------|----------------------|-----------|----------|----|------------------|-----------|---------|----------------|
| 归属 | ubsocket core(Acceptor 子) | ubsocket core | ubsocket core(umq) | ubsocket core | ubsocket core | ubsocket umq | ubsocket umq | ub_test | brpc | brpc/protobuf | ubsocket core |
| 类别 | Acceptor 级联子(queue chunk) | 析构不 delete | 析构不 delete rxQueue | 析构不 delete | 析构不 destroy | 退出未释放 | buffer 不回流 | 闭包/drain | bthread 抛弃 | 解析逃逸 | 析构不清 map |
| 与 Acceptor 关系 | **级联子(wakeup_event_)** | 自身 | 独立(rxQueue) | 独立(DataTxOps) | 独立(UmqSocket) | 独立 | 独立 | 独立 | 独立 | 独立 | 同类不同对象 |
| direct/indirect | indirect | direct | direct+indirect | direct | direct | — | — | — | — | — | — |
| ubs-comm 修复 | ✓(随 delete acceptor_ 级联消) | ✓(伞形) | ✓(伞形) | ✓(伞形) | ✓(伞形) | ✓ | ✓ | ❌ | ❌ | ❌ | ✓ |

本泄露是 Acceptor 泄露的**级联子对象**(`wakeup_event_.ready_event_queue_` deque chunk),与 Acceptor 同根,随 `~SocketBase` `delete acceptor_` 一并消除。属 ubs-comm 析构链伞形修复范围。

## 参考

- `UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md` — Acceptor 288B direct 泄露(§6 已 anticipated Acceptor 内 mutex,本报告是其 wakeup_event_ 子级联)
- `UBSOCKET-UMQ-CONNECTOR-LEAK-ANALYSIS.ch.md` — Connector 16B(同析构链家族)
- `UBSOCKET-UMQ-DATAOPS-*-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-RXQUEUE-*-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-SOCKET-MUTEX-LEAK-ANALYSIS.ch.md` — 析构链家族其他子对象/级联
- `UBSOCKET-ASYNCEVENTPOLL-EPOLLEVENT-LEAK-ANALYSIS.ch.md` — AsyncEventPoll 析构清理(同类不同对象)
- `UBSOCKET-UMQ-RX-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-TX-EVENT-LEAK-ANALYSIS.ch.md` — RX/TX Event 泄露(不同类)
- 其他 ub_test/brpc 泄露文档
- 源码:`src/ubsocket/csrc/core/ubsocket_wakeup_event.cpp:16-26,72-73`、`ubsocket_wakeup_event.h`、`src/ubsocket/csrc/core/ubsocket_socket_acceptor.h:60-63,115`、`ubsocket_socket.cpp:55`、`ubsocket_socket.h:44-52,94`
