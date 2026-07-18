# UBSocket UmqTxOps 析构链泄露分析(UmqRxOps 泄露的孪生)

> **现象**:ASan 报告
> ```
> Direct leak of 72 byte(s) in 1 object(s) allocated from:
>     #1 SocketBase::CreateTxOps ubsocket_socket.cpp:110
>     #2 GenerateSocketCommOps ubsocket_socket.cpp:81
>     #3 UmqConnectorOps::DoUbConnect umq_socket_connector.cpp:572
>     #4 CreateSocketResources umq_socket_connector.cpp:228
>     ...
>     #9 brpc::Socket::Connect socket.cpp:1343
>     ...
>     #17 PerformanceTest::Init() ub_test/client.cpp
> ```
>
> 本文确认其为 `UBSOCKET-UMQ-DATAOPS-LEAK-ANALYSIS.ch.md`(UmqRxOps 96B/1)的**孪生泄露**,与 Acceptor/Connector/UmqRxOps **同一次 UmqSocket 销毁、同一家族**,仅泄露子对象不同。

## 1. 与 UmqRxOps 泄露孪生(同根同次)

| 维度 | UmqRxOps 泄露(96B) | 本泄露(UmqTxOps 72B) |
|------|---------------------|---------------------|
| 调用点 frame #2 | `GenerateSocketCommOps ubsocket_socket.cpp:87` | `GenerateSocketCommOps ubsocket_socket.cpp:81` |
| 分配点 | `CreateRxOps` `ubsocket_socket.cpp:136` `new UmqRxOps` | `CreateTxOps` `ubsocket_socket.cpp:110` `new UmqTxOps` |
| 对象 | `UmqRxOps`(96B) | `UmqTxOps`(72B) |
| 持有者 | `DataRx::rx_ops_` 裸指针 | `DataTx::tx_ops_` 裸指针 |
| 根因 | `~DataRx` 无用户析构,裸指针不 delete | **同**(`~DataTx` 无用户析构) |
| 数量 | 1 | **1(同一次 UmqSocket 销毁)** |

`GenerateSocketCommOps`(`ubsocket_socket.cpp:77-96`)在同一次调用里 `CreateTxOps`(`:81`)+ `CreateRxOps`(`:87`),分别 `new UmqTxOps` + `new UmqRxOps` 挂到 `DataTx::tx_ops_`/`DataRx::rx_ops_`。该 UmqSocket 被 `delete`(ref→0)时,`~DataTx`/`~DataRx` 均无用户析构(编译器默认不 delete 裸指针)→ 两者同时泄露。

## 2. 泄露对象

- **对象**:`ock::ubs::umq::UmqTxOps`(`umq_data_tx_ops.h:24`,继承 `DataTxOps`)。
- **分配点**:`SocketBase::CreateTxOps`(`ubsocket_socket.cpp:110`):
  ```cpp
  auto umqOps = new (std::nothrow) UmqTxOps(umqSock->raw_socket_, umqSock->UmqHandle());
  ```
- **大小**:72 字节。`UmqTxOps` 成员:`local_umqh_`(uint64)+ `head_buf_`/`tail_buf_`(`umq_buf_list_t`)+ `unsolicited_bytes_`/`unsolicited_wr_num_`/`unsignaled_wr_num_` + `successful_post_count_`(atomic)+ 继承 `DataTxOps` 的 `fd_`/`tx_queue_avail_num_`(atomic)/`ack_event_num_`/`get_and_ack_event_`/`epoll_event_num_`(atomic)/`expect_epoll_event_num_`/`need_fc_awake_`(atomic)→ 对齐 72B,吻合。
- **归属**:存入 `DataTx::tx_ops_`(`ubsocket_data_tx.h:88` 裸指针)。

## 3. 析构链缺陷(同 UmqRxOps 家族)

全仓 grep 确认(见 `UBSOCKET-UMQ-DATAOPS-LEAK-ANALYSIS.ch.md` §4):

- `delete tx_ops_` / `delete rx_ops_`:**0 命中**
- `~DataTx` / `~DataRx`:**无用户定义**(`DataTx() = default`,`ubsocket_data_tx.h:75`),编译器默认析构不 delete 裸指针
- `~SocketBase`(`ubsocket_socket.h:44-52`):不 delete ops
- `UmqSocket::UnInitialize`(`umq_socket.cpp:35-50`):不 delete ops
- 唯一 `delete txOps` 在 `ubsocket_socket.cpp:89`——`GenerateSocketCommOps` 失败路径(CreateRxOps 失败时释放尚未赋值的 txOps),**成功路径无**

因此:**每个被销毁的 UmqSocket(ref→0)都泄露其 `UmqTxOps` + `UmqRxOps`**(以及 `Acceptor` + `Connector`)。

## 4. 为何 ASan 只报 1 个(同 UmqRxOps)

可达性逻辑与 UmqRxOps 泄露完全相同:

- `ArraySet<Socket>` 是 LeakySingleton(全局静态可达),其中的 UmqSocket → `tx_.tx_ops_` → `UmqTxOps*` 链对 ASan **可达**,不标记。
- 只有 `ref_count_→0` 被 `delete` 的 UmqSocket,其 `~SocketBase`/`~DataTx` 跑了但没 delete `tx_ops_` → `UmqTxOps*` 失主变为**不可达** → ASan 标记。
- 本次仅 1 个 UmqSocket 被销毁(某条 client 连接失败/重试后 `ubsocket_close`→ref 归零)→ 1 个 `UmqTxOps` 泄露(72B)。

这与 Acceptor(288B/1)、UmqRxOps(96B/1)、本 UmqTxOps(72B/1)的"1 个"是**同一个 UmqSocket**——其析构泄露 4 个子对象(Acceptor + Connector + UmqTxOps + UmqRxOps)。`Connector` 应另有独立 ASan 报告(未粘贴)。

## 5. 触发条件

与 UmqRxOps/Acceptor 家族完全一致:任一 UmqSocket 被销毁(socket close 或 `ubsocket_uninit` 的 `ReleaseAll`)即泄露全部 4 个子对象。与 UB 配置无关。

## 6. 修复方案

**与 `UBSOCKET-UMQ-DATAOPS-LEAK-ANALYSIS.ch.md` §7、`UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md` §7 完全共用伞形修复**——`~DataTx`/`~DataRx` 补 delete ops:

```cpp
// ubsocket_data_tx.h
~DataTx() { delete tx_ops_; }

// ubsocket_data_rx.h
~DataRx() { delete rx_ops_; }
```

`SocketBase` 析构 → `tx_`/`rx_` 值成员析构 → `~DataTx`/`~DataRx` → `delete tx_ops_`/`rx_ops_`。一次覆盖 UmqTxOps + UmqRxOps。

**伞形 PR 合并**(覆盖全部 4 子对象):
- `~SocketBase` 补 `delete acceptor_/connector_`(消 Acceptor/Connector)
- `~DataTx`/`~DataRx` 补 `delete tx_ops_`/`rx_ops_`(消 UmqTxOps/UmqRxOps)
- `~Acceptor` 补 `LockRegistry::LOCK_OPS.destroy(lock)`(消 mutex)

或更彻底:全部改 `std::unique_ptr` RAII 根治。

## 7. 验证

修复后 ASan 重跑:Acceptor(288B/1)、UmqRxOps(96B/1)、本 UmqTxOps(72B/1)**同时消失**,且 `Connector` 报告也应消失。可用小 `thread_num` 短测验证。

## 8. 关键代码位置索引

| 位置 | 作用 | 泄露相关性 |
|------|------|-----------|
| `ubsocket_socket.cpp:110` | `new UmqTxOps` | **泄露分配点** |
| `ubsocket_socket.cpp:81` | `GenerateSocketCommOps` 调 `CreateTxOps` | 调用点 |
| `ubsocket_data_tx.h:88` | `DataTxOps *tx_ops_` 裸指针 | 无 RAII |
| `ubsocket_data_tx.h:73-75` | `DataTx` 无用户析构 | 不 delete tx_ops_ |
| `ubsocket_socket.h:44-52` | `~SocketBase` | 不 delete ops |
| `umq_socket.cpp:35-50` | `UnInitialize` | 不 delete ops |
| `ubsocket_socket.cpp:89` | `delete txOps` | 仅失败路径 |

## 9. 与其他泄露的关系

| 维度 | 本泄露(UmqTxOps) | UmqRxOps | Acceptor/Connector | TX Event | RespClosure/done | RX | RpcMeta | dummy/bvar |
|------|-------------------|----------|--------------------|---------|------------------|----|---------|------------|
| 归属 | ubsocket 核心 | ubsocket 核心 | ubsocket 核心 | ubsocket umq | ub_test | ubsocket umq | brpc/protobuf | brpc |
| 与 UmqRxOps 关系 | **孪生同根同次** | 自身 | 同家族同次销毁 | 独立 | 独立 | 独立 | 独立 | 独立 |
| 修复合并 | 与 UmqRxOps/Acceptor 伞形 | — | — | 独立 | 独立 | 独立 | 无 | 无 |

本泄露与 UmqRxOps(96B)、Acceptor(288B)、Connector 是**同一次 UmqSocket 销毁的 4 子对象泄露**,应合并为一次伞形析构链修复。

## 参考

- `UBSOCKET-UMQ-DATAOPS-LEAK-ANALYSIS.ch.md` — UmqRxOps 泄露(孪生,伞形修复合并)
- `UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md` — Acceptor/Connector 泄露(同家族同次销毁)
- `UBSOCKET-UMQ-TX-EVENT-LEAK-ANALYSIS.ch.md` — TX Event 泄露(不同类)
- `UBSOCKET-UMQ-RX-LEAK-ANALYSIS.ch.md` — RX 泄露(不同类)
- 其他 ub_test/brpc 泄露文档
- 源码:`src/ubsocket/csrc/core/ubsocket_socket.cpp:77-139`、`ubsocket_data_tx.h:73-88`
