# UBSocket UmqBufferReceiveQueue 析构链泄露分析(析构链家族之五)

> **现象**:ASan 报告
> ```
> Direct leak of 48 byte(s) in 1 object(s) allocated from:
>     #1 ock::ubs::umq::UmqSocket::CreateLocalUmq umq_socket.cpp:144
>     #2 UmqConnectorOps::DoUbConnect umq_socket_connector.cpp:570
>     #3 CreateSocketResources umq_socket_connector.cpp:228
>     ...
>     #8 brpc::Socket::Connect socket.cpp:1343
>     ...
>     #16 PerformanceTest::Init() ub_test/client.cpp
> ```
>
> 本文确认其为 ubs-comm 析构链家族的**第五个裸指针子对象**(`UmqSocket::rxQueue`),与 Acceptor/Connector/UmqTxOps/UmqRxOps **同一次 UmqSocket 销毁、同一家族**,仅泄露子对象不同。

## 1. 析构链家族的五个裸指针子对象

`UmqSocket`/`SocketBase` 持有五个裸指针子对象,析构链全部不 `delete`——同一缺陷家族,同一次 UmqSocket 销毁(ref→0)全部泄露:

| 子对象 | 成员 | 分配点 | 大小 | ASan 报告 |
|--------|------|--------|------|-----------|
| `Acceptor` | `SocketBase::acceptor_` | `ubsocket_socket.cpp:55` | 288B | 已文档(288B/1) |
| `Connector` | `SocketBase::connector_` | `ubsocket_socket.cpp:56` | ~? | 同家族(应有报告) |
| `UmqTxOps` | `DataTx::tx_ops_` | `ubsocket_socket.cpp:110` | 72B | 已文档(72B/1) |
| `UmqRxOps` | `DataRx::rx_ops_` | `ubsocket_socket.cpp:136` | 96B | 已文档(96B/1) |
| **`UmqBufferReceiveQueue`** | **`UmqSocket::rxQueue`** | **`umq_socket.cpp:144`** | **48B** | **本报告(48B/1)** |

五个子对象 + Acceptor 内部 mutex,同一次 UmqSocket 销毁全部泄露。

## 2. 泄露对象

- **对象**:`ock::ubs::umq::UmqBufferReceiveQueue`(`umq_buffer_receive_queue.h:26`)。
- **分配点**:`UmqSocket::CreateLocalUmq`(`umq_socket.cpp:144`):
  ```cpp
  rxQueue = new (std::nothrow) UmqBufferReceiveQueue();
  ```
- **大小**:48 字节。`UmqBufferReceiveQueue` 成员:`SPSCRingQueue<umq_buf_t*>* receive_queue`(指针 8B)+ `FastHeap<...>* out_of_order_queue`(指针 8B)+ `m_expect_sn`(uint32)+ `O3QueueComparator comp`+ `use_o3_`/`is_shutdown_`(bool)+ `m_max_ooo_gap`(uint32)+ `m_ooo_timeout_ns`/`m_ooo_start_time_ns`(uint64×2)→ 对齐 48B,吻合。
- **归属**:存入 `UmqSocket::rxQueue`(`umq_socket.h:214` 裸指针)。

## 3. 调用栈解读

```
ub_test client PerformanceTest::Init → stub.Test → Channel::CallMethod → IssueRPC
  → brpc Socket::Write → StartWrite → ConnectIfNot → DoConnect → Connect (socket.cpp:1343)
    → ubsocket_wrapper_connect → ubsocket_connect → Connector::Connect
      → CreateSocketResources (umq_socket_connector.cpp:228)
        → DoUbConnect (umq_socket_connector.cpp:570): CreateLocalUmq
          → UmqSocket::CreateLocalUmq (umq_socket.cpp:144): new UmqBufferReceiveQueue  ← 泄露分配
```

client 建链 `DoUbConnect` 调 `CreateLocalUmq` 创建本端 UMQ 后,`new UmqBufferReceiveQueue` 挂到 `UmqSocket::rxQueue`(共享 JFR RX 缓存队列)。

## 4. 析构链缺陷(同家族)

`rxQueue` 是 `UmqSocket` 的裸指针成员(`umq_socket.h:214`)。**全仓 grep 确认**:

- `delete rxQueue`:**0 命中**
- `~UmqSocket`(`umq_socket.h:51-54` 调 `UnInitialize`):不 delete rxQueue
- `UmqSocket::UnInitialize`(`umq_socket.cpp:35-50`):不 delete rxQueue
- `~SocketBase`(`ubsocket_socket.h:44-52`):不 delete rxQueue
- `UmqSocket::FlushRxQueue`(`umq_socket.cpp:452-460`):**只 `rxQueue->Shutdown()`(置 `is_shutdown_=true`),不 `delete`**

```cpp
void UmqSocket::FlushRxQueue() {
    if (rxQueue == nullptr) { return; }
    rxQueue->Shutdown();   // 仅标记,不释放
    return;
}
```

因此:**每个被销毁的 UmqSocket(ref→0)都泄露其 `rxQueue`**——`~UmqSocket`→`UnInitialize`→`~SocketBase`→`~Socket` 均不 delete。

## 5. 级联泄露:`~UmqBufferReceiveQueue` 清内部但从未被调

`UmqBufferReceiveQueue` 析构函数(`umq_buffer_receive_queue.cpp:56-60`)本身**会清理内部**:

```cpp
UmqBufferReceiveQueue::~UmqBufferReceiveQueue() {
    Shutdown();
    ClearAllocations();   // delete receive_queue + out_of_order_queue + 释放 vector 缓冲
}
```

`ClearAllocations`(`:118-132`)会 `delete receive_queue`/`out_of_order_queue` 并释放其内部 `std::vector` 缓冲。**但前提是析构函数被调用**——由于无人 `delete rxQueue`,该析构函数从不执行,导致**级联泄露**:

| 层级 | 对象 | 分配点 | ASan 报告 |
|------|------|--------|-----------|
| 1 | `UmqBufferReceiveQueue` 本体(48B) | `umq_socket.cpp:144` `new UmqBufferReceiveQueue` | **本报告** |
| 2 | `SPSCRingQueue<umq_buf_t*>`(内部) | `umq_buffer_receive_queue.cpp:34` `new SPSCRingQueue` | 应有独立报告 |
| 3 | `SPSCRingQueue::buffer_`(vector 缓冲) | `ubsocket_spsc_ring_queue.h:24` vector ctor | 应有独立报告 |
| 4 | `FastHeap`(o3 模式) | `umq_buffer_receive_queue.cpp:47` `new FastHeap` | 应有独立报告(RM_CTP 模式) |

即本 48B 报告是 `rxQueue` 泄露的**顶层对象**,其内部 SPSCRingQueue/vector/FastHeap 应另有 ASan 报告(未粘贴)。

## 6. 为何 ASan 只报 1 个(同家族)

可达性逻辑与 Acceptor/UmqRxOps/UmqTxOps 完全相同:

- `ArraySet<Socket>` 是 LeakySingleton(全局静态可达),其中的 UmqSocket → `rxQueue` → `UmqBufferReceiveQueue*` 链对 ASan **可达**,不标记。
- 只有 `ref_count_→0` 被 `delete` 的 UmqSocket,其析构链跑了但没 delete `rxQueue` → `UmqBufferReceiveQueue*` 失主变为**不可达** → ASan 标记。
- 本次仅 1 个 UmqSocket 被销毁(某条 client 连接失败/重试后 `ubsocket_close`→ref 归零)→ 1 个 `UmqBufferReceiveQueue` 泄露(48B)。

这与 Acceptor(288B/1)、UmqRxOps(96B/1)、UmqTxOps(72B/1)的"1 个"是**同一个 UmqSocket**——其析构泄露全部 5 个子对象(Acceptor + Connector + UmqTxOps + UmqRxOps + UmqBufferReceiveQueue)+ Acceptor mutex。

## 7. 触发条件

与家族完全一致:任一 UmqSocket 被销毁(socket close 或 `ubsocket_uninit` 的 `ReleaseAll`)即泄露全部 5 子对象。与 UB 配置无关。

## 8. 修复方案

**与 `UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md`、`UBSOCKET-UMQ-DATAOPS-LEAK-ANALYSIS.ch.md`、`UBSOCKET-UMQ-DATAOPS-TX-LEAK-ANALYSIS.ch.md` 共用伞形修复**。`rxQueue` 是 `UmqSocket`(派生类)特有成员,在 `~UmqSocket` 或 `UnInitialize` 补 delete:

```cpp
// umq_socket.cpp UnInitialize 末尾 或 ~UmqSocket
void UmqSocket::UnInitialize() noexcept {
    ...
    UnbindAndFlushRemoteUmq(this);
    DestroyLocalUmq();
    delete rxQueue;          // ← 新增
    rxQueue = nullptr;
}
```

或 `~UmqSocket`(`umq_socket.h:51-54`):
```cpp
~UmqSocket() override {
    UnInitialize();
    delete rxQueue;          // ← 新增(UnInitialize 之后,umq 资源已释放)
    rxQueue = nullptr;
}
```

**顺序约束**:`UnInitialize` 中 `FlushRxQueue`/`UnbindAndFlushRemoteUmq` 可能用 `rxQueue`(共享 JFR 模式 `FlushRxQueue`),故 `delete rxQueue` 必须在 `UnInitialize` 之后(派生析构 `~UmqSocket` 先 `UnInitialize` 再 delete,基类 `~SocketBase` 最后)。当前析构顺序 `~UmqSocket`→`~SocketBase` 满足。

### 伞形 PR 合并(覆盖全部 5 子对象 + mutex)

| 子对象 | 修复位置 |
|--------|---------|
| `acceptor_`/`connector_` | `~SocketBase` 补 `delete` |
| `tx_ops_`/`rx_ops_` | `~DataTx`/`~DataRx` 补 `delete` |
| **`rxQueue`** | **`~UmqSocket` 或 `UnInitialize` 补 `delete`** |
| Acceptor mutex | `~Acceptor` 补 `LockRegistry::LOCK_OPS.destroy` |

或更彻底:全部改 `std::unique_ptr` RAII 根治。

## 9. 验证

修复后 ASan 重跑:Acceptor(288B/1)、UmqRxOps(96B/1)、UmqTxOps(72B/1)、本 UmqBufferReceiveQueue(48B/1)**同时消失**,且 `Connector` + 内部 SPSCRingQueue/vector/FastHeap 报告也应消失。可用小 `thread_num` 短测验证。

## 10. 关键代码位置索引

| 位置 | 作用 | 泄露相关性 |
|------|------|-----------|
| `umq_socket.cpp:144` | `new UmqBufferReceiveQueue` | **泄露分配点** |
| `umq_socket.h:214` | `UmqBufferReceiveQueue *rxQueue` 裸指针 | 无 RAII |
| `umq_socket.cpp:452-460` | `FlushRxQueue` 只 `Shutdown` 不 delete | 清理不完整 |
| `umq_socket.h:51-54` | `~UmqSocket` 调 `UnInitialize` | 不 delete rxQueue |
| `umq_socket.cpp:35-50` | `UnInitialize` | 不 delete rxQueue |
| `umq_socket.h:44-52` | `~SocketBase` | 不 delete(基类不知 rxQueue) |
| `umq_buffer_receive_queue.cpp:56-60` | `~UmqBufferReceiveQueue` 清内部 | 析构会清,但从不被调 |

## 11. 与其他泄露的关系

| 维度 | 本泄露(rxQueue 48B) | Acceptor 288B / UmqRxOps 96B / UmqTxOps 72B | TX Event | RespClosure/done | RX | bvar 家族 | RpcMeta |
|------|---------------------|--------------------------------------------|----------|------------------|----|-----------|---------|
| 归属 | ubsocket 核心 | ubsocket 核心 | ubsocket umq | ub_test | ubsocket umq | brpc | brpc/protobuf |
| 类别 | 析构不 delete rxQueue | 析构不 delete | 退出未释放 | 闭包/drain | buffer 不回流 | bthread 抛弃 | 解析逃逸 |
| 与析构链家族关系 | **第五子对象同次销毁** | 自身 | 独立 | 独立 | 独立 | 独立 | 独立 |
| ubs-comm 修复 | ✓(伞形) | ✓(伞形) | ✓ | ❌ | ✓ | ❌ | ❌ |

本泄露与 Acceptor/Connector/UmqTxOps/UmqRxOps 是**同一次 UmqSocket 销毁的 5 子对象泄露**,应合并为一次伞形析构链修复(`~SocketBase` delete acceptor_/connector_ + `~DataTx`/`~DataRx` delete tx_ops_/rx_ops_ + `~UmqSocket` delete rxQueue + `~Acceptor` destroy mutex)。

## 参考

- `UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md` — Acceptor/Connector 泄露(同家族,伞形修复合并)
- `UBSOCKET-UMQ-DATAOPS-LEAK-ANALYSIS.ch.md` — UmqRxOps 泄露(同家族)
- `UBSOCKET-UMQ-DATAOPS-TX-LEAK-ANALYSIS.ch.md` — UmqTxOps 泄露(同家族)
- `UBSOCKET-UMQ-RX-LEAK-ANALYSIS.ch.md` — RX 运行时泄露(不同类)
- `UBSOCKET-UMQ-TX-EVENT-LEAK-ANALYSIS.ch.md` — TX Event 泄露(不同类)
- 其他 ub_test/brpc 泄露文档
- 源码:`src/ubsocket/csrc/core/umq/umq_socket.cpp:35-50,140-148,452-460`、`umq_socket.h:51-54,214`、`umq_buffer_receive_queue.cpp:30-60,118-132`
