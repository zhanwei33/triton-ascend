# UBSocket Connector 析构链泄露分析(析构链家族之七,补齐)

> **现象**:ASan 报告
> ```
> Direct leak of 16 byte(s) in 1 object(s) allocated from:
>     #1 SocketBase::Create ubsocket_socket.cpp:56
>     #2 ubsocket_socket ubsocket_sock.cpp:38
>     #3 ubsocket_wrapper_socket ubsocket_wrapper.cpp:43
>     #4 brpc::Socket::Connect socket.cpp:1334
>     ...
>     #12 PerformanceTest::Init() ub_test/client.cpp
> ```
>
> 本文确认其为 ubs-comm 析构链家族的**第七个裸指针子对象**(`SocketBase::connector_`),即 `UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md`(Acceptor 288B)anticipated 的 `Connector` 报告(该文档 §1 表注 "Connector ~? 同家族(应有报告)")。本报告补齐家族全部子对象观测。

## 1. 析构链家族全部子对象(已观测 7/7)

`UmqSocket`/`SocketBase` 持有七个裸指针子对象,析构链全部不释放——同一缺陷家族,同一次 UmqSocket 销毁(ref→0)全部泄露。至此全部观测到:

| 子对象 | 成员 | 分配点 | 大小 | ASan 报告 |
|--------|------|--------|------|-----------|
| `Acceptor` | `SocketBase::acceptor_` | `ubsocket_socket.cpp:55` | 288B | 已文档(288B/1) |
| **`Connector`** | **`SocketBase::connector_`** | **`ubsocket_socket.cpp:56`** | **16B** | **本报告(16B/1)** |
| `UmqTxOps` | `DataTx::tx_ops_` | `ubsocket_socket.cpp:110` | 72B | 已文档(72B/1) |
| `UmqRxOps` | `DataRx::rx_ops_` | `ubsocket_socket.cpp:136` | 96B | 已文档(96B/1) |
| `UmqBufferReceiveQueue` | `UmqSocket::rxQueue` | `umq_socket.cpp:144` | 48B | 已文档(48B/1) |
| `bthread::Mutex` | `UmqSocket::mutex_` | `umq_socket.h:48` | 48B | 已文档(48B/1) |
| Acceptor 内 mutex | `Acceptor::ubSocket_async_accept_info.lock` | `ubsocket_socket_acceptor.h:62` | 48B | Acceptor 文档 §6(应有报告) |

七个子对象 + Acceptor 内 mutex,同一次 UmqSocket 销毁全部泄露。

## 2. 泄露对象

- **对象**:`ock::ubs::Connector`(`ubsocket_socket_connector.h:47`)。
- **分配点**:`SocketBase::Create`(`ubsocket_socket.cpp:56`):
  ```cpp
  sockBase->connector_ = new Connector(sock, connectorOps);
  ```
- **大小**:16 字节。`Connector` 成员(`ubsocket_socket_connector.h:69-72`):
  ```cpp
  int raw_fd_ = -1;                          // 4
  int event_fd_ = -1;                        // 4
  Ref<ConnectorOps> connector_ops_ = nullptr;// 8 (指针)
  ```
  4+4+8 = 16B,吻合。
- **归属**:存入 `SocketBase::connector_`(`ubsocket_socket.h:95` 裸指针)。

## 3. 调用栈解读

```
ub_test client PerformanceTest::Init → stub.Test → Channel::CallMethod → IssueRPC
  → brpc Socket::Write → StartWrite → ConnectIfNot → DoConnect → Connect (socket.cpp:1334)
    → ubsocket_wrapper_socket → ubsocket_socket → SocketBase::Create
      → new Connector (ubsocket_socket.cpp:56)  ← 泄露分配
```

`SocketBase::Create` 在创建 UmqSocket 时 `new Acceptor`(`:55`)+ `new Connector`(`:56`),挂到 `SocketBase::acceptor_`/`connector_`。

## 4. 析构链缺陷(同家族)

`connector_` 是 `SocketBase` 的裸指针成员(`ubsocket_socket.h:95`)。全仓 grep 确认(见 `UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md` §5):

- `delete connector_`:**0 命中**
- `~SocketBase`(`ubsocket_socket.h:44-52`):只做 trace,**不 delete connector_**
- `UmqSocket::UnInitialize`(`umq_socket.cpp:35-50`):不 delete
- `~Connector`(`ubsocket_socket_connector.cpp:74-80`)存在(只做 trace),但**从未被调用**(无人 `delete connector_`)

因此:**每个被销毁的 UmqSocket(ref→0)都泄露其 `connector_`**。

### 级联:`Ref<ConnectorOps> connector_ops_` 也不释放

`Connector` 持 `Ref<ConnectorOps> connector_ops_`(智能指针)。`~Connector` 会 ~Ref → DecreaseRef ConnectorOps。但 `~Connector` 从不被调用 → `connector_ops_` Ref 不减 → 若该 `UmqConnectorOps` 仅由此 Connector 持有,则 `UmqConnectorOps` 也泄露(应另有 ASan 报告)。与 Acceptor 的 `Ref<AcceptorOps> acceptor_ops_` 同理(Acceptor 文档 §6 的 mutex 之外,AcceptorOps 也会级联)。

## 5. 为何 ASan 只报 1 个(同家族)

可达性逻辑与前 6 子对象完全相同:

- `ArraySet<Socket>` 是 LeakySingleton(全局静态可达),其中的 UmqSocket → `connector_` → `Connector*` 链对 ASan **可达**,不标记。
- 只有 `ref_count_→0` 被 `delete` 的 UmqSocket,其 `~SocketBase` 跑了但没 delete `connector_` → `Connector*` 失主变为**不可达** → ASan 标记。
- 本次仅 1 个 UmqSocket 被销毁(某条 client 连接失败/重试后 `ubsocket_close`→ref 归零)→ 1 个 `Connector` 泄露(16B)。

这与 Acceptor(288B/1)、UmqTxOps(72B/1)、UmqRxOps(96B/1)、UmqBufferReceiveQueue(48B/1)、mutex(48B/1)的"1 个"是**同一个 UmqSocket**——其析构泄露全部 7 个子对象 + Acceptor 内 mutex。

## 6. 触发条件

与家族完全一致:任一 UmqSocket 被销毁(socket close 或 `ubsocket_uninit` 的 `ReleaseAll`)即泄露全部 7 子对象 + Acceptor mutex。与 UB 配置无关。

## 7. 修复方案

**与 `UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md`、`UBSOCKET-UMQ-DATAOPS-*-LEAK-ANALYSIS.ch.md`、`UBSOCKET-UMQ-RXQUEUE-LEAK-ANALYSIS.ch.md`、`UBSOCKET-UMQ-SOCKET-MUTEX-LEAK-ANALYSIS.ch.md` 共用伞形修复**。`connector_` 是 `SocketBase` 成员,在 `~SocketBase` 补 delete:

```cpp
~SocketBase() override {
    delete acceptor_;
    acceptor_ = nullptr;
    delete connector_;          // ← 新增(消本报告)
    connector_ = nullptr;
    // tx_/rx_ 由 ~DataTx/~DataRx delete tx_ops_/rx_ops_(见 DataOps 文档)
    // rxQueue/mutex_ 由 ~UmqSocket 处理(见 rxQueue/mutex 文档)
    if (GlobalSetting::UBS_TRACE_ENABLED) {
        Statistics::StatsMgr::SubMConnCount();
        if (IsClient()) { Statistics::StatsMgr::SubMActiveConnCount(); }
    }
}
```

### 伞形 PR 合并(覆盖全部 7 子对象 + Acceptor mutex)

| 子对象 | 修复位置 |
|--------|---------|
| `acceptor_`/`connector_` | `~SocketBase` 补 `delete`(消 Acceptor 288B + 本 Connector 16B) |
| `tx_ops_`/`rx_ops_` | `~DataTx`/`~DataRx` 补 `delete`(消 UmqTxOps 72B + UmqRxOps 96B) |
| `rxQueue` | `~UmqSocket` 补 `delete`(消 UmqBufferReceiveQueue 48B) |
| `mutex_` | `~UmqSocket` 补 `LockRegistry::LOCK_OPS.destroy`(消 mutex 48B) |
| Acceptor `ubSocket_async_accept_info.lock` | `~Acceptor` 补 `destroy`(消 Acceptor mutex 48B) |

或更彻底:全部改 `std::unique_ptr` RAII 根治。

## 8. 验证

修复后 ASan 重跑:Acceptor(288B/1)、本 Connector(16B/1)、UmqTxOps(72B/1)、UmqRxOps(96B/1)、UmqBufferReceiveQueue(48B/1)、mutex(48B/1)**全部同时消失**,且 Acceptor 内 mutex + 级联的 AcceptorOps/ConnectorOps 报告也应消失。可用小 `thread_num` 短测验证。

## 9. 关键代码位置索引

| 位置 | 作用 | 泄露相关性 |
|------|------|-----------|
| `ubsocket_socket.cpp:56` | `new Connector` | **泄露分配点** |
| `ubsocket_socket.h:95` | `Connector *connector_` 裸指针 | 无 RAII |
| `ubsocket_socket.h:44-52` | `~SocketBase` | **不 delete connector_** |
| `umq_socket.cpp:35-50` | `UnInitialize` | 不 delete |
| `ubsocket_socket_connector.cpp:74-80` | `~Connector` 存在但从不被调 | AcceptorOps Ref 级联不释放 |
| `ubsocket_socket_connector.h:69-72` | `Connector` 成员(16B) | 大小来源 |

## 10. 与其他泄露的关系

| 维度 | 本泄露(Connector 16B) | Acceptor 288B / UmqRxOps 96B / UmqTxOps 72B / rxQueue 48B / mutex 48B | TX Event | RespClosure/done | RX | bvar 家族 | RpcMeta | AsyncEventPoll |
|------|----------------------|--------------------------------------------------------|----------|------------------|----|-----------|---------|----------------|
| 归属 | ubsocket 核心 | ubsocket 核心 | ubsocket umq | ub_test | ubsocket umq | brpc | brpc/protobuf | ubsocket core |
| 类别 | 析构不 delete connector_ | 析构不 delete | 退出未释放 | 闭包/drain | buffer 不回流 | bthread 抛弃 | 解析逃逸 | 析构不清 map |
| 与析构链家族关系 | **第七子对象同次销毁(补齐)** | 自身 | 独立 | 独立 | 独立 | 独立 | 独立 | 同类不同对象 |
| ubs-comm 修复 | ✓(伞形) | ✓(伞形) | ✓ | ❌ | ✓ | ❌ | ❌ | ✓ |

本泄露补齐析构链家族全部 7 子对象观测,与 Acceptor/Connector/UmqTxOps/UmqRxOps/UmqBufferReceiveQueue/mutex 是**同一次 UmqSocket 销毁的 7 子对象泄露**,应合并为一次伞形析构链修复。至此家族观测完整。

## 参考

- `UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md` — Acceptor/Connector 泄露(§1 已 anticipated 本 Connector 报告)
- `UBSOCKET-UMQ-DATAOPS-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-DATAOPS-TX-LEAK-ANALYSIS.ch.md` — UmqRxOps/UmqTxOps 泄露
- `UBSOCKET-UMQ-RXQUEUE-LEAK-ANALYSIS.ch.md` — rxQueue 泄露
- `UBSOCKET-UMQ-SOCKET-MUTEX-LEAK-ANALYSIS.ch.md` — mutex_ 泄露
- `UBSOCKET-ASYNCEVENTPOLL-EPOLLEVENT-LEAK-ANALYSIS.ch.md` — AsyncEventPoll 析构清理(同类不同对象)
- `UBSOCKET-UMQ-RX-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-TX-EVENT-LEAK-ANALYSIS.ch.md` — RX/TX Event 泄露(不同类)
- 其他 ub_test/brpc 泄露文档
- 源码:`src/ubsocket/csrc/core/ubsocket_socket.cpp:55-56`、`ubsocket_socket.h:44-52,94-95`、`umq_socket.cpp:35-50`、`ubsocket_socket_connector.cpp:74-80`、`ubsocket_socket_connector.h:47-73`
