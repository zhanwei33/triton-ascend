# UBSocket DataTxOps/DataRxOps 析构链泄露分析

> **现象**:ASan 报告
> ```
> Direct leak of 96 byte(s) in 1 object(s) allocated from:
>     #1 SocketBase::CreateRxOps ubsocket_socket.cpp:136
>     #2 GenerateSocketCommOps ubsocket_socket.cpp:87
>     #3 UmqConnectorOps::DoUbConnect umq_socket_connector.cpp:572
>     #4 CreateSocketResources umq_socket_connector.cpp:228
>     ...
>     #9 brpc::Socket::Connect socket.cpp:1343
>     ...
>     #17 PerformanceTest::Init() ub_test/client.cpp
> ```
> 本文确认其与 `UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md`(Acceptor 288B/1)是**同一家族、同一次 UmqSocket 销毁**的伴生泄露,仅泄露的子对象不同。

## 1. 与 Acceptor/Connector 泄露同家族

`SocketBase` 持有多个裸指针子对象,析构链全部不 `delete`——这是同一缺陷家族:

| 子对象 | 成员 | 分配点 | 大小 | ASan 报告 |
|--------|------|--------|------|-----------|
| `Acceptor` | `SocketBase::acceptor_` | `ubsocket_socket.cpp:55` | 288B | `UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md`(288B/1) |
| `Connector` | `SocketBase::connector_` | `ubsocket_socket.cpp:56` | ~? | 同家族(应有报告) |
| `UmqTxOps` | `DataTx::tx_ops_` | `ubsocket_socket.cpp:110` | ~? | 同家族(应有报告) |
| **`UmqRxOps`** | **`DataRx::rx_ops_`** | **`ubsocket_socket.cpp:136`** | **96B** | **本报告 96B/1** |

**同一次 UmqSocket 销毁(ref→0 → `delete this`)泄露全部 4 个子对象**。Acceptor 报告(288B/1)与本报告(96B/1)都是"1 个对象"——同一个被销毁的 UmqSocket,其析构链跑完但 4 个裸指针子对象全未释放。

## 2. 泄露对象

- **对象**:`ock::ubs::umq::UmqRxOps`(`ubsocket_data_rx_ops.h:27`,继承 `DataRxOps`)。
- **分配点**:`SocketBase::CreateRxOps`(`ubsocket_socket.cpp:136`):
  ```cpp
  auto umqOps = new (std::nothrow) UmqRxOps(umqSock->raw_socket_, umqSock->UmqHandle());
  ```
- **大小**:96 字节。`UmqRxOps` 成员:`fd_` + `rx_queue_avail_num_` + `ack_event_num_` + `epoll_event_num_`(atomic) + `expect_epoll_event_num_` + `get_and_ack_event_` + `poll_` + `BlockCache block_cache_`(~48B) + `remaining_size_` + `flow_control_failed_` + `local_umqh_` → 对齐 96B,吻合。
- **归属**:存入 `DataRx::rx_ops_`(`ubsocket_data_rx.h:85` 裸指针)。

## 3. 调用栈解读

```
ub_test client PerformanceTest::Init → stub.Test → Channel::CallMethod → IssueRPC
  → brpc Socket::Write → StartWrite → ConnectIfNot → DoConnect → Connect (socket.cpp:1343)
    → ubsocket_wrapper_connect → ubsocket_connect → Connector::Connect
      → CreateSocketResources (umq_socket_connector.cpp:228)
        → DoUbConnect (umq_socket_connector.cpp:572): CreateLocalUmq 后 GenerateSocketCommOps
          → GenerateSocketCommOps (ubsocket_socket.cpp:87): CreateTxOps + CreateRxOps
            → CreateRxOps (ubsocket_socket.cpp:136): new UmqRxOps  ← 泄露分配
```

client 建链 `DoUbConnect` 创建本端 UMQ 后调 `GenerateSocketCommOps` 生成 TX/RX ops,挂到 `SocketBase::tx_`/`rx_`。

## 4. 析构链缺陷(同 Acceptor 家族)

`DataTxOps`/`DataRxOps` 经 `CreateTxOps`/`CreateRxOps` `new` 出,以裸指针存入 `DataTx::tx_ops_`(`ubsocket_data_tx.h:88`)/`DataRx::rx_ops_`(`ubsocket_data_rx.h:85`)。**全仓 grep 确认**:

- `delete tx_ops_` / `delete rx_ops_`:**0 命中**
- `~DataTx` / `~DataRx`:**无用户定义**(grep 无匹配),`DataTx() = default;`(`ubsocket_data_tx.h:75`),编译器生成默认析构——**裸指针成员不会 `delete`**
- `~SocketBase`(`ubsocket_socket.h:44-52`):不 delete ops(只做 trace)
- `UmqSocket::UnInitialize`(`umq_socket.cpp:35-50`):不 delete ops
- 唯一 `delete txOps` 在 `ubsocket_socket.cpp:89`——`GenerateSocketCommOps` 失败路径(CreateRxOps 失败时释放尚未赋值的 txOps),**成功路径不清理**

因此:**每个被销毁的 UmqSocket(ref→0)都泄露其 `UmqTxOps` + `UmqRxOps`**(`~UmqSocket`→`UnInitialize`→`~SocketBase`→`~DataTx`/`~DataRx` 均不 delete)。

## 5. 为何 ASan 只报 1 个(同 Acceptor)

与 Acceptor 泄露完全相同的可达性逻辑:

- `ArraySet<Socket>` 是 LeakySingleton(全局静态可达),其中的 UmqSocket → `rx_.rx_ops_` → `UmqRxOps*` 链对 ASen **可达**,不标记。
- 只有 `ref_count_→0` 被 `delete` 的 UmqSocket,其 `~SocketBase`/`~DataRx` 跑了但没 delete `rx_ops_` → `UmqRxOps*` 失主变为**不可达** → ASan 标记。
- 本次仅 1 个 UmqSocket 被销毁(某条 client 连接失败/重试后 `ubsocket_close`→ref 归零)→ 1 个 `UmqRxOps` 泄露(96B)。

这与 Acceptor 报告(288B/1)的"1 个"是**同一个 UmqSocket**——其析构泄露了 Acceptor(288B)+ Connector + UmqTxOps + UmqRxOps(96B)。`Connector`/`UmqTxOps` 应另有独立 ASan 报告(未粘贴)。

## 6. 触发条件

与 Acceptor 家族完全一致:任一 UmqSocket 被销毁(socket close 或 `ubsocket_uninit` 的 `ReleaseAll`)即泄露 4 个子对象。与 UB 配置无关。

## 7. 修复方案

本泄露与 `UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md` **共用同一伞形修复**——在 `~SocketBase`(或 `UnInitialize`)统一 `delete` 所有裸指针子对象:

```cpp
~SocketBase() override {
    // UnInitialize 已在 ~UmqSocket 跑过(umq 资源已释放,DelEpollEvent 用过 tx_/rx_ ops)
    delete acceptor_;
    acceptor_ = nullptr;
    delete connector_;
    connector_ = nullptr;
    // tx_/rx_ 是值成员,其内部裸指针 ops 需由 DataTx/DataRx 析构释放:
    //   方案 A:DataTx/DataRx 加 ~DataTx(){ delete tx_ops_; } / ~DataRx(){ delete rx_ops_; }
    //   方案 B:在此处 tx_.Reset()/rx_.Reset() 显式释放(DataTx/DataRx 暴露 Reset 方法)
    if (GlobalSetting::UBS_TRACE_ENABLED) {
        Statistics::StatsMgr::SubMConnCount();
        if (IsClient()) { Statistics::StatsMgr::SubMActiveConnCount(); }
    }
}
```

推荐 **方案 A**(DataTx/DataRx 自管 ops):在 `ubsocket_data_tx.h`/`ubsocket_data_rx.h` 加:

```cpp
~DataTx() { delete tx_ops_; }
~DataRx() { delete rx_ops_; }
```

这样 `SocketBase` 析构 → `tx_`/`rx_` 值成员析构 → `~DataTx`/`~DataRx` → `delete tx_ops_`/`rx_ops_`。RAII 自洽,与 `acceptor_`/`connector_` 的 delete 修复一起覆盖全部 4 个子对象。

**或更彻底方案 C**:全部改 `std::unique_ptr`(acceptor_/connector_/tx_ops_/rx_ops_),RAII 根治,杜绝裸指针遗漏。

### 与 Acceptor 文档方案的关系

`UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md` 方案 1 修 `acceptor_`/`connector_`,方案 2 修 `~Acceptor` mutex。本泄露需在其基础上**额外修 `DataTx`/`DataRx` 析构**(`tx_ops_`/`rx_ops_`)。建议合并为一次伞形 PR:
- `~SocketBase` 补 `delete acceptor_/connector_`
- `~DataTx`/`~DataRx` 补 `delete tx_ops_`/`rx_ops_`
- `~Acceptor` 补 `LockRegistry::LOCK_OPS.destroy(lock)`

一次覆盖 Acceptor/Connector/UmqTxOps/UmqRxOps/mutex 全部子对象泄露。

## 8. 验证

修复后 ASan 重跑:Acceptor(288B/1)与本 UmqRxOps(96B/1)**同时消失**,且 `Connector`/`UmqTxOps` 报告也应消失。可用小 `thread_num` 短测验证。

## 9. 关键代码位置索引

| 位置 | 作用 | 泄露相关性 |
|------|------|-----------|
| `ubsocket_socket.cpp:136` | `new UmqRxOps` | **泄露分配点** |
| `ubsocket_socket.cpp:110` | `new UmqTxOps` | 同家族(应有报告) |
| `ubsocket_data_rx.h:85` | `DataRxOps *rx_ops_` 裸指针 | 无 RAII |
| `ubsocket_data_tx.h:88` | `DataTxOps *tx_ops_` 裸指针 | 无 RAII |
| `ubsocket_data_rx.h:26` | `DataRxOps` 析构 `= default` | 不 delete |
| `ubsocket_data_tx.h:73` | `DataTx() = default` 无用户析构 | 不 delete tx_ops_ |
| `ubsocket_socket.h:44-52` | `~SocketBase` | 不 delete ops |
| `umq_socket.cpp:35-50` | `UnInitialize` | 不 delete ops |
| `ubsocket_socket.cpp:87-95` | `GenerateSocketCommOps` 成功路径 | tx_/rx_ 持 ops,不释放 |
| `ubsocket_socket.cpp:89` | `delete txOps` | 仅失败路径,成功路径无 |

## 10. 与其他泄露的关系

| 维度 | 本泄露(UmqRxOps) | Acceptor/Connector | TX Event | RespClosure/done | RX | RpcMeta |
|------|-------------------|--------------------|----------|------------------|----|---------| 
| 归属 | ubsocket 核心(析构链) | ubsocket 核心 | ubsocket umq | ub_test 应用 | ubsocket umq | brpc/protobuf |
| 类别 | 析构不 delete ops | 析构不 delete | 退出未释放 | 闭包/drain | buffer 不回流 | 解析逃逸 |
| 与 Acceptor 关系 | **同家族同一次销毁** | 自身 | 独立 | 独立 | 独立 | 独立 |
| 修复合并 | 与 Acceptor 伞形修 | — | 独立 | 独立 | 独立 | 无 ubs-comm |

本泄露与 Acceptor/Connector 是**同根同家族**,应合并为一次伞形修复(`~SocketBase` delete acceptor_/connector_ + `~DataTx`/`~DataRx` delete tx_ops_/rx_ops_)。

## 参考

- `UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md` — Acceptor/Connector 泄露(同家族,伞形修复合并)
- `UBSOCKET-UMQ-TX-EVENT-LEAK-ANALYSIS.ch.md` — TX Event 启动期泄露(不同类)
- `UBSOCKET-UMQ-RX-LEAK-ANALYSIS.ch.md` — RX 运行时泄露(不同类)
- `UBSOCKET-UBTEST-CLIENT-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UBTEST-DONE-CALLBACK-LEAK-ANALYSIS.ch.md` — ub_test 应用层泄露
- `UBSOCKET-UBTEST-PROTO-META-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UBTEST-PROTO-META-STRING-LEAK-ANALYSIS.ch.md` — brpc/protobuf 层泄露
- 源码:`src/ubsocket/csrc/core/ubsocket_socket.cpp:87-139`、`ubsocket_data_tx.h:73-88`、`ubsocket_data_rx.h:26-85`
