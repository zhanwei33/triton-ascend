# UBSocket UmqSocket mutex 泄露分析(析构链家族之六)

> **现象**:ASan 报告
> ```
> Direct leak of 48 byte(s) in 1 object(s) allocated from:
>     #1 brpc_external_lock_create ubsocket_initializer.cpp:407
>     #2 ock::ubs::umq::UmqSocket::UmqSocket(int) umq_socket.h:48
>     #3 ock::ubs::MakeRef<UmqSocket, int> ubsocket_ref.h:215
>     #4 SocketBase::Create ubsocket_socket.cpp:28
>     #5 ubsocket_socket ubsocket_sock.cpp:38
>     #6 ubsocket_wrapper_socket ubsocket_wrapper.cpp:43
>     #7 brpc::Socket::Connect socket.cpp:1334
>     ...
>     #15 PerformanceTest::Init() ub_test/client.cpp
> ```
>
> 本文确认其为 ubs-comm 析构链家族的**第六个裸指针子对象**(`UmqSocket::mutex_`),与 Acceptor/Connector/UmqTxOps/UmqRxOps/UmqBufferReceiveQueue **同一次 UmqSocket 销毁、同一家族**,仅泄露子对象不同。

## 1. 析构链家族的六个裸指针子对象

`UmqSocket`/`SocketBase` 持有六个裸指针子对象,析构链全部不释放——同一缺陷家族,同一次 UmqSocket 销毁(ref→0)全部泄露:

| 子对象 | 成员 | 分配点 | 大小 | ASan 报告 |
|--------|------|--------|------|-----------|
| `Acceptor` | `SocketBase::acceptor_` | `ubsocket_socket.cpp:55` | 288B | 已文档(288B/1) |
| `Connector` | `SocketBase::connector_` | `ubsocket_socket.cpp:56` | ~? | 同家族(应有报告) |
| `UmqTxOps` | `DataTx::tx_ops_` | `ubsocket_socket.cpp:110` | 72B | 已文档(72B/1) |
| `UmqRxOps` | `DataRx::rx_ops_` | `ubsocket_socket.cpp:136` | 96B | 已文档(96B/1) |
| `UmqBufferReceiveQueue` | `UmqSocket::rxQueue` | `umq_socket.cpp:144` | 48B | 已文档(48B/1) |
| **`bthread::Mutex`(mutex_)** | **`UmqSocket::mutex_`** | **`umq_socket.h:48`** | **48B** | **本报告(48B/1)** |

六个子对象 + Acceptor 内部 mutex(`ubSocket_async_accept_info.lock`),同一次 UmqSocket 销毁全部泄露。

> 注:本 `mutex_` 是 `UmqSocket` 自己的 mutex(ctor 创建),与 Acceptor 内部 mutex(`Acceptor` 的 `ubSocket_async_accept_info.lock`,`UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md` §6)是**两个不同的 mutex**,均泄露。

## 2. 泄露对象

- **对象**:`bthread::Mutex`(brpc 注入的锁实现,经 `brpc_external_lock_create` `new` 出)。
- **分配点**:`brpc_external_lock_create`(`ubsocket_initializer.cpp:363` 左右,用户构建版本报 407):`auto* mutex = new(std::nothrow) bthread::Mutex();` → 经 `LockRegistry::LOCK_OPS.create(LT_EXCLUSIVE)` 返回 `u_mutex_t*`。
- **触发点**:`UmqSocket` 构造函数(`umq_socket.h:48`):
  ```cpp
  explicit UmqSocket(int fd) : SocketBase(fd, SocketType::SOCK_TYPE_UMQ)
  {
      mutex_ = LockRegistry::LOCK_OPS.create(LT_EXCLUSIVE);   // ← new bthread::Mutex (48B)
  }
  ```
- **大小**:48 字节(`sizeof(bthread::Mutex)` on aarch64)。
- **归属**:存入 `UmqSocket::mutex_`(`umq_socket.h:211` `u_mutex_t *mutex_;` 裸指针)。

## 3. 析构链缺陷:UmqSocket 是家族里唯一不 destroy mutex_ 的类

`LockRegistry::LOCK_OPS.create` 与 `destroy` 配对。全仓 grep `LockRegistry::LOCK_OPS.destroy` 确认,**所有同类都正确 destroy 自己的 mutex_,唯独 `UmqSocket` 漏**:

| 类 | create 位置 | destroy 位置 | 状态 |
|----|-----------|-------------|------|
| `MainUmqState` | ctor | `umq_eid_table.h:61` `~MainUmqState` | ✓ destroy |
| `UmqEidTable` | ctor | `umq_eid_table.h:206-207` `~UmqEidTable` | ✓ destroy(mutex + main_mutex) |
| `EidRegistry` | ctor | `umq_eid_table.h:278` `~EidRegistry` | ✓ destroy |
| `RouteListRegistry` | ctor | `umq_eid_table.h:328` `~RouteListRegistry` | ✓ destroy |
| `UmqTransportPool` | ctor | `umq_transport_pool.h:52` `~UmqTransportPool` | ✓ destroy |
| `UmqTpTxEpollRunnerOps` | ctor | `umq_tp_tx_epoll_runner_ops.h:39` dtor | ✓ destroy |
| `UmqTpEventEpollRunnerOps` | ctor | `umq_tp_event_epoll_runner_ops.h:29` dtor | ✓ destroy |
| `UmqShareJfrEpollRunnerOps` | ctor | `umq_share_jfr_epoll_runner_ops.h:42` dtor | ✓ destroy |
| `Acceptor` | ctor(`ubsocket_socket_acceptor.h:62`) | ❌ `~Acceptor`(`ubsocket_socket_acceptor.cpp:300-305`)不 destroy | ✗ 泄露(见 Acceptor 文档 §6) |
| **`UmqSocket`** | **ctor(`umq_socket.h:48`)** | **❌ `~UmqSocket`/`UnInitialize`/`~SocketBase` 均不 destroy** | **✗ 泄露(本报告)** |

`UmqSocket` 析构链:
```cpp
~UmqSocket() override { UnInitialize(); }   // umq_socket.h:51-54
// UnInitialize (umq_socket.cpp:35-50): DelEpollEvent + UnbindAndFlushRemoteUmq + DestroyLocalUmq
//   — 不 LockRegistry::LOCK_OPS.destroy(mutex_)
// ~SocketBase (ubsocket_socket.h:44-52): 只 trace
//   — 不 destroy(基类不知 mutex_)
```

因此:**每个被销毁的 UmqSocket(ref→0)都泄露其 `mutex_`**(`bthread::Mutex` 48B)。

## 4. 为何 ASan 只报 1 个(同家族)

可达性逻辑与前 5 子对象完全相同:

- `ArraySet<Socket>` 是 LeakySingleton(全局静态可达),其中的 UmqSocket → `mutex_` → `bthread::Mutex*` 链对 ASan **可达**,不标记。
- 只有 `ref_count_→0` 被 `delete` 的 UmqSocket,其析构链跑了但没 destroy `mutex_` → `bthread::Mutex*` 失主变为**不可达** → ASan 标记。
- 本次仅 1 个 UmqSocket 被销毁(某条 client 连接失败/重试后 `ubsocket_close`→ref 归零)→ 1 个 `bthread::Mutex` 泄露(48B)。

这与 Acceptor(288B/1)、UmqRxOps(96B/1)、UmqTxOps(72B/1)、UmqBufferReceiveQueue(48B/1)的"1 个"是**同一个 UmqSocket**——其析构泄露全部 6 个子对象 + Acceptor 内部 mutex。

## 5. 触发条件

与家族完全一致:任一 UmqSocket 被销毁(socket close 或 `ubsocket_uninit` 的 `ReleaseAll`)即泄露全部 6 子对象 + Acceptor mutex。与 UB 配置无关。

## 6. 修复方案

**与 `UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md`、`UBSOCKET-UMQ-DATAOPS-*-LEAK-ANALYSIS.ch.md`、`UBSOCKET-UMQ-RXQUEUE-LEAK-ANALYSIS.ch.md` 共用伞形修复**。`mutex_` 是 `UmqSocket` 特有成员,在 `~UmqSocket` 补 destroy:

```cpp
// umq_socket.h
~UmqSocket() override {
    UnInitialize();
    LockRegistry::LOCK_OPS.destroy(mutex_);   // ← 新增
    mutex_ = nullptr;
}
```

**顺序约束**:`UnInitialize` 中若任一路径 lock `mutex_`(需核查 UmqSocket 内联方法是否用 `Locker sLock(mutex_)`),则 destroy 必须在 `UnInitialize` 之后。当前析构顺序 `~UmqSocket`→`UnInitialize`(先)→ destroy mutex_(后)→ `~SocketBase`(基)满足。若 `mutex_` 实际未被任何方法使用(疑似 vestigial 成员),可考虑直接移除该成员及 ctor 的 create,根治。

### 伞形 PR 合并(覆盖全部 6 子对象 + Acceptor mutex)

| 子对象 | 修复位置 |
|--------|---------|
| `acceptor_`/`connector_` | `~SocketBase` 补 `delete` |
| `tx_ops_`/`rx_ops_` | `~DataTx`/`~DataRx` 补 `delete` |
| `rxQueue` | `~UmqSocket` 补 `delete` |
| **`mutex_`(本报告)** | **`~UmqSocket` 补 `LockRegistry::LOCK_OPS.destroy`** |
| Acceptor `ubSocket_async_accept_info.lock` | `~Acceptor` 补 `destroy` |

或更彻底:全部改 `std::unique_ptr`/RAII wrapper 根治。

## 7. 验证

修复后 ASan 重跑:Acceptor(288B/1)、UmqRxOps(96B/1)、UmqTxOps(72B/1)、UmqBufferReceiveQueue(48B/1)、本 mutex(48B/1)**同时消失**,且 `Connector` + Acceptor mutex 报告也应消失。可用小 `thread_num` 短测验证。

## 8. 关键代码位置索引

| 位置 | 作用 | 泄露相关性 |
|------|------|-----------|
| `umq_socket.h:48` | ctor `mutex_ = LOCK_OPS.create` | **泄露分配触发** |
| `umq_socket.h:211` | `u_mutex_t *mutex_` 裸指针 | 无 RAII |
| `umq_socket.h:51-54` | `~UmqSocket` 调 `UnInitialize` | 不 destroy mutex_ |
| `umq_socket.cpp:35-50` | `UnInitialize` | 不 destroy mutex_ |
| `ubsocket_initializer.cpp:363`(用户 407) | `brpc_external_lock_create` `new bthread::Mutex` | **堆分配点** |
| `ubsocket_initializer.cpp:371-379` | `brpc_external_lock_destroy` `delete` | 配对 release(从未被调) |
| 各同类(`umq_eid_table.h`/`umq_transport_pool.h`/三 EpollRunnerOps) | 正确 destroy 对照 | 唯 UmqSocket 漏 |

## 9. 与其他泄露的关系

| 维度 | 本泄露(mutex_ 48B) | rxQueue 48B / Acceptor 288B / UmqRxOps 96B / UmqTxOps 72B | TX Event | RespClosure/done | RX | bvar 家族 | RpcMeta |
|------|---------------------|--------------------------------------------------------|----------|------------------|----|-----------|---------|
| 归属 | ubsocket 核心 | ubsocket 核心 | ubsocket umq | ub_test | ubsocket umq | brpc | brpc/protobuf |
| 类别 | 析构不 destroy mutex | 析构不 delete | 退出未释放 | 闭包/drain | buffer 不回流 | bthread 抛弃 | 解析逃逸 |
| 与析构链家族关系 | **第六子对象同次销毁** | 自身 | 独立 | 独立 | 独立 | 独立 | 独立 |
| ubs-comm 修复 | ✓(伞形) | ✓(伞形) | ✓ | ❌ | ✓ | ❌ | ❌ |

本泄露与 Acceptor/Connector/UmqTxOps/UmqRxOps/UmqBufferReceiveQueue 是**同一次 UmqSocket 销毁的 6 子对象泄露**,应合并为一次伞形析构链修复。

## 参考

- `UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md` — Acceptor/Connector 泄露(同家族,Acceptor 内部 mutex 同类问题 §6)
- `UBSOCKET-UMQ-DATAOPS-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-DATAOPS-TX-LEAK-ANALYSIS.ch.md` — UmqRxOps/UmqTxOps 泄露(同家族)
- `UBSOCKET-UMQ-RXQUEUE-LEAK-ANALYSIS.ch.md` — rxQueue 泄露(同家族)
- `UBSOCKET-UMQ-RX-LEAK-ANALYSIS.ch.md` — RX 运行时泄露(不同类)
- `UBSOCKET-UMQ-TX-EVENT-LEAK-ANALYSIS.ch.md` — TX Event 泄露(不同类)
- 其他 ub_test/brpc 泄露文档
- 源码:`src/ubsocket/csrc/core/umq/umq_socket.h:48,51-54,211`、`umq_socket.cpp:35-50`、`src/brpc/ubsocket_initializer.cpp:357-379`
