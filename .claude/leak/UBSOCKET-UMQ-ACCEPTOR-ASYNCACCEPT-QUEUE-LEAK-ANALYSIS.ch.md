# UBSocket Acceptor async_accept ready_queue 级联泄露分析(Acceptor 泄露之子二)

> **现象**:ASan 报告(**Indirect leak**——间接泄露,挂在父对象 `Acceptor` 之下)
> ```
> Indirect leak of 504 byte(s) in 1 object(s) allocated from:
>     #1 std::__new_allocator<std::tuple<int, sockaddr, unsigned int>>::allocate
>     #2 _Deque_base<tuple>::_M_allocate_node stl_deque.h:583
>     #3 _M_create_nodes stl_deque.h:684
>     #4 _M_initialize_map stl_deque.h:658
>     #5 _Deque_base::_Deque_base() stl_deque.h:460
>     #6 std::deque<tuple>::deque() stl_deque.h:855
>     #7 std::queue<tuple, deque>::queue() stl_queue.h:167
>     #8 Acceptor::AsyncAcceptInfo::AsyncAcceptInfo()
>     #9 Acceptor::Acceptor(sock, acceptorOps)
>     #10 SocketBase::Create ubsocket_socket.cpp:55
>     ... → brpc::Socket::Connect socket.cpp:1334 → PerformanceTest::Init
> ```
>
> 本文确认其为 `UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md`(Acceptor 288B direct)的**第二个级联子对象**——`Acceptor::ubSocket_async_accept_info.ready_queue`(`std::queue<tuple<int,sockaddr,socklen_t>>`)的 deque chunk。`UBSOCKET-UMQ-ACCEPTOR-WAKEUP-QUEUE-LEAK-ANALYSIS.ch.md` §1 表已 anticipated("ready_queue 级联 deque 节点 应有报告"),本报告坐实。

## 1. 与前一份 wakeup queue 级联的关系

两份 Acceptor 级联子对象报告均源于 `acceptor_` 无 delete(`~Acceptor` 从不执行):

| 报告 | Acceptor 子对象 | 成员类型 | deque 元素 | chunk 大小 | 文档 |
|------|----------------|---------|-----------|-----------|------|
| 512B(indirect) | `wakeup_event_.ready_event_queue_` | `std::queue<int, std::deque<int>>` | `int`(4B) | 128 元素/chunk = 512B | 已文档(Wakeup Queue) |
| **504B(indirect)** | **`ubSocket_async_accept_info.ready_queue`** | **`std::queue<tuple<int,sockaddr,socklen_t>, std::deque<...>>`** | **`tuple<int,sockaddr,uint>`(24B)** | **21 元素/chunk = 504B** | **本报告** |

二者同属 Acceptor 析构链级联,根因与修复完全相同。

## 2. 泄露对象

- **对象**:`std::deque<std::tuple<int, struct sockaddr, unsigned int>>` 的一个内部 chunk 节点(`std::queue<tuple, deque>::ready_queue` 的底层 deque)。
- **分配点**:`Acceptor::AsyncAcceptInfo::AsyncAcceptInfo()`(frame #8,默认 ctor)成员默认初始化触发 `std::queue<tuple>::queue()`(默认 ctor)→ `std::deque<tuple>::deque()` → `_M_initialize_map(0)` → `_M_create_nodes` → `_M_allocate_node` → `new[]` 一个 deque chunk。
- **大小**:504 字节。`sizeof(std::tuple<int, struct sockaddr, unsigned int>)`:
  - `int`(4,offset 0)+ `struct sockaddr`(16,offset 4)+ `unsigned int`(4,offset 20)= 24 字节,alignof=4,无额外填充。
  - libstdc++ `std::deque<tuple>` chunk = `512 / sizeof(tuple)` 取整?实际 libstdc++ deque chunk 大小固定 512B,可容纳 `floor(512/24) = 21` 个 tuple → `21 × 24 = 504B`(512B chunk 内有效 504B)。与报告吻合。
- **归属**:`Acceptor::ubSocket_async_accept_info.ready_queue`(`ubsocket_socket_acceptor.h:110` `std::queue<std::tuple<int, struct sockaddr, socklen_t>> ready_queue`),经 `ubSocket_async_accept_info`(`AsyncAcceptInfo` 值成员,`:113`)归属 `SocketBase::acceptor_`(`:94`)。

## 3. 代码细节

`Acceptor`(`ubsocket_socket_acceptor.h:60-63,109-113`):

```cpp
Acceptor(const SocketPtr &sock, AcceptorOps *acceptorOps)
    : raw_fd_(sock->raw_socket_), acceptor_ops_(acceptorOps) {
    ubSocket_async_accept_info.lock = LockRegistry::LOCK_OPS.create(LT_EXCLUSIVE);
    // ubSocket_async_accept_info 默认构造 → ready_queue 默认构造 → deque 分配 1 个 504B chunk ← 本报告
}
...
struct AsyncAcceptInfo {
    std::queue<std::tuple<int, struct sockaddr, socklen_t>> ready_queue;   // :110 ← 本报告 queue
    std::atomic<int32_t> asyncTaskNum{0U};                                  // :111
    u_mutex_t *lock = nullptr;                                              // :112(级联 mutex,见 Acceptor 文档 §6)
} ubSocket_async_accept_info;                                               // :113 Acceptor 值成员
```

`ready_queue` 在运行时用于异步 accept 暂存就绪 fd(`ubsocket_socket_acceptor.cpp:106` `ready_queue.push(std::make_tuple(fd, addr_tmp, len_tmp))`、`:142-144` `front/pop`)。**默认构造即分配 1 个 deque chunk(504B)**(libstdc++ `_M_initialize_map(0)` 仍分配最少 1 节点),故空 queue 也占 504B。

`~AsyncAcceptInfo`(编译器生成默认)会析构 `ready_queue` → `~queue<tuple>` → `~deque<tuple>` 释放 chunk。但需 `~Acceptor` 触发,而 `~Acceptor` 因 `acceptor_` 无 delete 从不执行。

## 4. 为何泄露(同 Acceptor 家族,级联)

`UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md` §5 已确认:`SocketBase::acceptor_` 全仓 **0 处 `delete`**,`~SocketBase`/`UnInitialize`/`~UmqSocket` 均不 delete。

级联效应:
- `acceptor_` 不 delete → `~Acceptor`(`ubsocket_socket_acceptor.cpp:300-305`)从不执行
- `~Acceptor` 不执行 → `ubSocket_async_accept_info`(`AsyncAcceptInfo` 值成员)析构不触发
- `~AsyncAcceptInfo`(默认合成)不执行 → `ready_queue`(`std::queue<tuple>`)析构不触发
- `~queue<tuple>` / `~deque<tuple>` 不执行 → deque chunk(504B)+ 内部 map 不释放 → 泄露
- 同时 `ubSocket_async_accept_info.lock`(`:62` create)也不 destroy(级联 mutex,Acceptor 文档 §6)

**根因与 Acceptor/wakeup queue 泄露完全相同**:`acceptor_` 无 delete → Acceptor 全部子对象级联失主。本 504B 是其中 indirect 之二(前一份 512B 是其一)。

## 5. 为何 504B / 1 个

- libstdc++ `std::deque<tuple<int,sockaddr,uint>>` chunk = 512B(固定),容纳 `floor(512/24)=21` 个 24B tuple → 有效 504B。默认构造分配 1 个 chunk。
- 1 个 = 单 Acceptor 的 `ready_queue` 的 deque chunk。本次仅 1 个 UmqSocket 被销毁(同 Acceptor 288B/1 的"1 个")→ 1 个 504B chunk 泄露。
- queue 运行时可能增长(更多 chunk),但本报告仅 1 个 504B,说明退出时刻 queue 基本空(仅默认 chunk)。

## 6. 触发条件

与 Acceptor 家族完全一致:任一 UmqSocket 被销毁即泄露 Acceptor + 全部级联(含本 504B ready_queue chunk + 512B wakeup queue chunk + 级联 mutex 等)。与 UB 配置无关。

## 7. 修复方案

**与 `UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md` §7、`UBSOCKET-UMQ-ACCEPTOR-WAKEUP-QUEUE-LEAK-ANALYSIS.ch.md` §7 等共用伞形修复**——`~SocketBase` 补 `delete acceptor_`:

```cpp
~SocketBase() override {
    delete acceptor_;           // ← 触发级联:~Acceptor → ~AsyncAcceptInfo → ~queue<tuple> → 释放 504B chunk
    acceptor_ = nullptr;        //   + ~wakeup_event_ → ~queue<int> → 释放 512B chunk(前一份)
    delete connector_;          //   + ~Acceptor destroy lock(需 ~Acceptor 补)
    connector_ = nullptr;
    // ... tx_/rx_/rxQueue/mutex_ 见各自文档
    if (GlobalSetting::UBS_TRACE_ENABLED) { ... }
}
```

`delete acceptor_` → `~Acceptor` → 成员析构 → `~AsyncAcceptInfo` → `~queue<tuple>` → `~deque<tuple>` 释放 504B chunk + `~wakeup_event_` 释放 512B chunk + destroy 两处 mutex + 关 eventfd。**一处 delete 覆盖 Acceptor 全部级联子对象**。

`~Acceptor` 内部还需补 `LockRegistry::LOCK_OPS.destroy(ubSocket_async_accept_info.lock)`(Acceptor 文档 §6 方案2)——不过若 `~AsyncAcceptInfo` 默认析构不 destroy lock(它只 `delete` 即释放指针,但 `u_mutex_t*` 是 `void*` 指针,析构不调 `LockRegistry::LOCK_OPS.destroy`),需在 `~Acceptor` 显式 destroy。本报告不涉及 mutex,但修复时一并处理。

### 与伞形析构链 PR 的关系

本修复是伞形析构链 PR 的 `acceptor_` 子项(与 wakeup queue 512B、Connector 16B、UmqTxOps/RxOps、rxQueue 级联、mutex_ 等一并合并)。

## 8. 验证

修复后 ASan 重跑:本 504B indirect + wakeup queue 512B indirect + Acceptor 288B direct + Acceptor 全部级联(mutex/acceptor_ops_ 等)**同时消失**。可用小 `thread_num` 短测验证。

## 9. 关键代码位置索引

| 位置 | 作用 | 泄露相关性 |
|------|------|-----------|
| `ubsocket_socket_acceptor.h:110` | `std::queue<tuple<int,sockaddr,socklen_t>> ready_queue` 成员 | **本报告 queue 归属** |
| `ubsocket_socket_acceptor.h:109-113` | `AsyncAcceptInfo` 结构 | 含 ready_queue/asyncTaskNum/lock |
| `ubsocket_socket_acceptor.h:113` | `AsyncAcceptInfo ubSocket_async_accept_info` 值成员 | Acceptor 子成员 |
| `ubsocket_socket_acceptor.h:60-63` | `Acceptor` ctor(默认构造 `ubSocket_async_accept_info`) | frame #9,触发本 504B |
| `ubsocket_socket_acceptor.cpp:300-305` | `~Acceptor`(只 trace,从不被调) | 级联释放入口(失效) |
| `ubsocket_socket_acceptor.cpp:106,142-144` | `ready_queue.push/front/pop` 运行时用法 | queue 运行时增长(本报告仅默认 chunk) |
| `ubsocket_socket.cpp:55` | `new Acceptor` | 层级1 direct(Acceptor 文档) |
| `ubsocket_socket.h:44-52,94` | `~SocketBase` 不 delete `acceptor_` | 根因 |

## 10. 与其他泄露的关系

| 维度 | 本泄露(ready_queue 504B) | wakeup queue 512B | Acceptor 288B / Connector 16B | rxQueue 级联(48B+128KB+2056B) | UmqTxOps/RxOps 72/96B | mutex 48B | TX Event | RX | RespClosure/done | bvar 家族 | RpcMeta | AsyncEventPoll |
|------|--------------------------|-------------------|------------------------------|----------------------------|----------------------|-----------|----------|----|------------------|-----------|---------|----------------|
| 归属 | ubsocket core(Acceptor 子) | 同 | ubsocket core | ubsocket core(umq) | ubsocket core | ubsocket core | ubsocket umq | ubsocket umq | ub_test | brpc | brpc/protobuf | ubsocket core |
| 类别 | Acceptor 级联子(ready_queue chunk) | Acceptor 级联子(wakeup queue chunk) | 析构不 delete | 析构不 delete rxQueue | 析构不 delete | 析构不 destroy | 退出未释放 | buffer 不回流 | 闭包/drain | bthread 抛弃 | 解析逃逸 | 析构不清 map |
| 与 Acceptor 关系 | **级联子二(ready_queue)** | 级联子一(wakeup_event_) | 自身 | 独立(rxQueue) | 独立 | 独立 | 独立 | 独立 | 独立 | 独立 | 独立 | 同类不同对象 |
| direct/indirect | indirect | indirect | direct | direct+indirect | direct | direct | — | — | — | — | — | — |
| ubs-comm 修复 | ✓(随 delete acceptor_ 级联消) | ✓(随 acceptor_) | ✓(伞形) | ✓(伞形) | ✓(伞形) | ✓(伞形) | ✓ | ✓ | ❌ | ❌ | ❌ | ✓ |

本泄露是 Acceptor 泄露的**第二个级联子对象**(`ubSocket_async_accept_info.ready_queue` deque chunk),与 wakeup queue(512B)、Acceptor(288B)同根,随 `~SocketBase` `delete acceptor_` 一并消除。属 ubs-comm 析构链伞形修复范围。

## 参考

- `UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md` — Acceptor 288B direct 泄露(§6 已 anticipated Acceptor 内 mutex/级联)
- `UBSOCKET-UMQ-ACCEPTOR-WAKEUP-QUEUE-LEAK-ANALYSIS.ch.md` — wakeup queue 512B(§1 表已 anticipated 本 ready_queue 报告)
- `UBSOCKET-UMQ-CONNECTOR-LEAK-ANALYSIS.ch.md` — Connector 16B(同析构链家族)
- `UBSOCKET-UMQ-DATAOPS-*-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-RXQUEUE-*-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-SOCKET-MUTEX-LEAK-ANALYSIS.ch.md` — 析构链家族其他子对象/级联
- `UBSOCKET-ASYNCEVENTPOLL-EPOLLEVENT-LEAK-ANALYSIS.ch.md` — AsyncEventPoll 析构清理(同类不同对象)
- `UBSOCKET-UMQ-RX-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-TX-EVENT-LEAK-ANALYSIS.ch.md` — RX/TX Event 泄露(不同类)
- 其他 ub_test/brpc 泄露文档
- 源码:`src/ubsocket/csrc/core/ubsocket_socket_acceptor.h:60-63,109-113`、`ubsocket_socket_acceptor.cpp:106,142-144,300-305`、`ubsocket_socket.cpp:55`、`ubsocket_socket.h:44-52,94`
