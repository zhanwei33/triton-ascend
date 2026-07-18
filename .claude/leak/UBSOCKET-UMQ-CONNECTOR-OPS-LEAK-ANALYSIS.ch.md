# UBSocket UmqConnectorOps 级联泄露分析(Connector 泄露之子)

> **现象**:ASan 报告(**Indirect leak**——间接泄露,挂在父对象 `Connector` 之下)
> ```
> Indirect leak of 480 byte(s) in 1 object(s) allocated from:
>     #1 SocketBase::CreateConnectorOps ubsocket_socket.cpp:187
>     #2 SocketBase::Create ubsocket_socket.cpp:49
>     #3 ubsocket_socket ubsocket_sock.cpp:38
>     ... → brpc::Socket::Connect socket.cpp:1334 → PerformanceTest::Init
> ```
>
> 本文确认其为 `UBSOCKET-UMQ-CONNECTOR-LEAK-ANALYSIS.ch.md`(Connector 16B direct)§4 已 anticipated 的**级联子对象**——`Connector::connector_ops_`(`Ref<ConnectorOps>`)持有的 `UmqConnectorOps`。

## 1. 与 Connector 泄露的级联关系

`Connector`(`SocketBase::connector_`,16B,direct)持 `Ref<ConnectorOps> connector_ops_` 智能指针。`~Connector` 从不执行(`connector_` 全仓 0 处 `delete`,见 `UBSOCKET-UMQ-CONNECTOR-LEAK-ANALYSIS.ch.md` §4)→ `~Ref` 不触发 → `UmqConnectorOps` 引用不减 → 若 Connector 是唯一持有者则 `UmqConnectorOps` 泄露。本 480B 即该级联:

| 层级 | 对象 | 成员 | 分配点 | 大小 | ASan 报告 |
|------|------|------|--------|------|-----------|
| 1 | `Connector` | `SocketBase::connector_` 裸指针 | `ubsocket_socket.cpp:56` `new Connector` | 16B | 已文档(direct) |
| 2 | **`UmqConnectorOps`** | **`Connector::connector_ops_`(`Ref<ConnectorOps>`)** | **`ubsocket_socket.cpp:187` `new UmqConnectorOps`** | **480B** | **本报告(indirect)** |

`UmqConnectorOps` 内部还有 `peer_all_socket_ids_`/`back_routes_`/`non_aff_route_list_` 等 `std::vector` 与 `umq_conn_info_`(含 string peer_ip + 4×umq_eid_t)——这些 vector 的内部缓冲与 string 是**更深层级联**(若有独立 ASan 报告)。

## 2. 泄露对象

- **对象**:`ock::ubs::umq::UmqConnectorOps`(`umq_socket_connector.h:22`,继承 `ConnectorOps`)。
- **分配点**:`SocketBase::CreateConnectorOps`(`ubsocket_socket.cpp:187`):
  ```cpp
  auto umqOps = new (std::nothrow) UmqConnectorOps(sock->raw_socket_);
  ...
  connectorOps = umqOps;   // :193 传出
  ```
  经 `SocketBase::Create`(`:49` `CreateConnectorOps(...)`)→ `Connector` ctor(`ubsocket_socket.cpp:56` `new Connector(sock, connectorOps)`)→ 存入 `Connector::connector_ops_`(`ubsocket_socket_connector.h:72` `Ref<ConnectorOps>`)。
- **大小**:480 字节。`UmqConnectorOps` 成员(`umq_socket_connector.h:22-94`):
  - `bool use_round_robin_` + `int peer_socket_id_` + `std::vector<uint32_t> peer_all_socket_ids_`(~24B shell)+ `umq_route_t conn_route_` + `std::vector<umq_route_t> back_routes_`(~24B)+ `std::vector<umq_route_t> non_aff_route_list_`(~24B)+ `umq_topo_type_t topo_type_` + `bool degradable_` + `OtherRouteMessage other_route_message_` + `umq_route_t other_conn_route` + `umq_route_t other_back_conn_route` + `UBHandshakeState retry_state_`
  - `UmqConnInfo umq_conn_info_`(`:36-42`):继承 `ConnInfo`(`string peer_ip` + int + int + time_point)+ 4×`umq_eid_t`(各 16B = 64B)
  - 继承 `ConnectorOps`(`ubsocket_socket_connector.h:22-43`):`RawConnInfoV4 conn_info` + `int raw_fd_` + ref count
  - 合计对齐 480B,吻合(vector/string 的内部缓冲是独立堆分配,不计入 480B shell)。
- **归属**:`Connector::connector_ops_`(`Ref<ConnectorOps>` 智能指针),经 `Connector` 归属 `SocketBase::connector_`。

## 3. 引用计数与级联

`Ref<ConnectorOps>` 是引用计数智能指针(`ubsocket_ref.h`):

- `CreateConnectorOps` `new UmqConnectorOps` → ref_count=1(经 `connectorOps = umqOps` 传入 Connector ctor 时 `Ref` 构造 IncreaseRef)。
- `Connector::connector_ops_` 是**唯一** `Ref<ConnectorOps>` 持有者(`SocketBase::Create` 的 `connectorOps` 局部裸指针在赋给 Connector 后不再有其他 Ref 持有)。
- 正常清理:`~Connector` → `~Ref<ConnectorOps>` → DecreaseRef → ref 1→0 → `delete` UmqConnectorOps(480B 释放)。
- 泄露:`~Connector` 从不执行(`connector_` 无 delete)→ `~Ref` 不触发 → ref 不减 → `UmqConnectorOps` 不释放 → 480B 泄露。

`UmqConnectorOps` 内部 `umq_conn_info_.peer_ip`(`std::string`)+ 3 个 vector 的内部缓冲是**更深层级联**——`~UmqConnectorOps` 不执行 → 这些 string/vector 缓冲也不释放(应有独立 ASan 报告)。

## 4. 为何泄露(同 Connector 家族,级联)

`UBSOCKET-UMQ-CONNECTOR-LEAK-ANALYSIS.ch.md` §4 已确认:`SocketBase::connector_` 全仓 **0 处 `delete`**,`~SocketBase`/`UnInitialize`/`~UmqSocket` 均不 delete。

级联效应:
- `connector_` 不 delete → `~Connector`(`ubsocket_socket_connector.cpp:74-80`)从不执行
- `~Connector` 不执行 → `connector_ops_`(`Ref<ConnectorOps>`)析构不触发
- `~Ref<ConnectorOps>` 不执行 → 不 DecreaseRef → `UmqConnectorOps` ref 不归零 → 不 `delete` → 480B 泄露
- 同时 `UmqConnectorOps` 内部 string/vector 缓冲也不释放(更深层级联)

**根因与 Connector 泄露完全相同**:`connector_` 无 delete → Connector 全部子对象级联失主。本 480B 是其中 indirect 之一。

## 5. 为何 480B / 1 个

- `sizeof(UmqConnectorOps)` = 480B(对象 shell,不含 vector/string 内部缓冲)。
- 1 个 = 单 Connector 的 `connector_ops_` 持有的 UmqConnectorOps。本次仅 1 个 UmqSocket 被销毁(同 Connector 16B/1 的"1 个")→ 1 个 480B 泄露。
- 内部 vector/string 缓冲若非空,应有额外 indirect 报告(本环境退出时刻 vector 基本空,故仅 480B shell)。

## 6. 触发条件

与 Connector 家族完全一致:任一 UmqSocket 被销毁(socket close 或 `ubsocket_uninit` 的 `ReleaseAll`)即泄露 Connector + 全部级联(含本 480B UmqConnectorOps + 更深层 string/vector 缓冲)。与 UB 配置无关。

## 7. 修复方案

**与 `UBSOCKET-UMQ-CONNECTOR-LEAK-ANALYSIS.ch.md` §7、`UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md` §7 等共用伞形修复**——`~SocketBase` 补 `delete connector_`:

```cpp
~SocketBase() override {
    delete acceptor_;
    acceptor_ = nullptr;
    delete connector_;           // ← 触发级联:~Connector → ~Ref<ConnectorOps> → DecreaseRef → delete UmqConnectorOps(480B)
    connector_ = nullptr;        //   + ~UmqConnectorOps → ~string/~vector 释放内部缓冲(更深层级联)
    // ... tx_/rx_/rxQueue/mutex_ 见各自文档
    if (GlobalSetting::UBS_TRACE_ENABLED) { ... }
}
```

`delete connector_` → `~Connector` → `~Ref<ConnectorOps>` → DecreaseRef → ref 0 → `delete UmqConnectorOps` → `~UmqConnectorOps` → `~string`(peer_ip)+ `~vector`(peer_all_socket_ids_/back_routes_/non_aff_route_list_)释放内部缓冲。**一处 delete 覆盖 Connector 全部级联子对象**(UmqConnectorOps 480B + 内部 string/vector 缓冲)。

### 与伞形析构链 PR 的关系

本修复是伞形析构链 PR 的 `connector_` 子项(与 `delete acceptor_`、`~DataTx`/`~DataRx` delete tx_ops_/rx_ops_、`~UmqSocket` delete rxQueue + destroy mutex_ 一并合并)。

## 8. 验证

修复后 ASan 重跑:本 480B indirect + Connector 16B direct + UmqConnectorOps 内部 string/vector 缓冲(若有)**同时消失**。可用小 `thread_num` 短测验证。

## 9. 关键代码位置索引

| 位置 | 作用 | 泄露相关性 |
|------|------|-----------|
| `ubsocket_socket.cpp:187` | `new UmqConnectorOps` in `CreateConnectorOps` | **本报告 480B 分配点** |
| `ubsocket_socket.cpp:49,56` | `Create` 调 `CreateConnectorOps` + `new Connector(sock, ops)` | 调用链 |
| `ubsocket_socket_connector.h:72` | `Ref<ConnectorOps> connector_ops_` 智能指针 | 持有 UmqConnectorOps(ref) |
| `umq_socket_connector.h:22-94` | `UmqConnectorOps` 类(480B) | 泄露对象 |
| `umq_socket_connector.h:36-42` | `UmqConnInfo umq_conn_info_`(含 string peer_ip + 4×eid) | 更深层级联(string) |
| `ubsocket_socket_connector.cpp:74-80` | `~Connector`(只 trace,从不被调) | 级联释放入口(失效) |
| `ubsocket_socket.h:44-52,95` | `~SocketBase` 不 delete `connector_` | 根因 |

## 10. 与其他泄露的关系

| 维度 | 本泄露(UmqConnectorOps 480B) | Connector 16B | Acceptor 288B + 级联(512B+504B) | rxQueue 级联(48B+128KB+2056B) | UmqTxOps/RxOps 72/96B | mutex 48B | TX Event | RX | RespClosure/done | bvar 家族 | RpcMeta | AsyncEventPoll |
|------|------------------------------|--------------|-------------------------------|----------------------------|----------------------|-----------|----------|----|------------------|-----------|---------|----------------|
| 归属 | ubsocket core(Connector 子) | ubsocket core | ubsocket core | ubsocket core(umq) | ubsocket core | ubsocket core | ubsocket umq | ubsocket umq | ub_test | brpc | brpc/protobuf | ubsocket core |
| 类别 | Connector 级联子(Ref 不减) | 析构不 delete | 析构不 delete + 级联 | 析构不 delete rxQueue | 析构不 delete | 析构不 destroy | 退出未释放 | buffer 不回流 | 闭包/drain | bthread 抛弃 | 解析逃逸 | 析构不清 map |
| 与 Connector 关系 | **级联子(UmqConnectorOps)** | 自身 | 独立(Acceptor) | 独立(rxQueue) | 独立(DataTxOps) | 独立(UmqSocket) | 独立 | 独立 | 独立 | 独立 | 独立 | 同类不同对象 |
| direct/indirect | indirect | direct | direct+indirect | direct+indirect | direct | direct | — | — | — | — | — | — |
| ubs-comm 修复 | ✓(随 delete connector_ 级联消) | ✓(伞形) | ✓(伞形) | ✓(伞形) | ✓(伞形) | ✓(伞形) | ✓ | ✓ | ❌ | ❌ | ❌ | ✓ |

本泄露是 Connector 泄露的**级联子对象**(`Ref<ConnectorOps>` 持有的 UmqConnectorOps),与 Connector 16B、Acceptor 系列同根,随 `~SocketBase` `delete connector_` 一并消除。属 ubs-comm 析构链伞形修复范围。

## 参考

- `UBSOCKET-UMQ-CONNECTOR-LEAK-ANALYSIS.ch.md` — Connector 16B direct 泄露(§4 已 anticipated 本 UmqConnectorOps 级联报告)
- `UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-ACCEPTOR-WAKEUP-QUEUE-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-ACCEPTOR-ASYNCACCEPT-QUEUE-LEAK-ANALYSIS.ch.md` — Acceptor 系列级联(同析构链家族)
- `UBSOCKET-UMQ-DATAOPS-*-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-RXQUEUE-*-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-SOCKET-MUTEX-LEAK-ANALYSIS.ch.md` — 析构链家族其他子对象/级联
- `UBSOCKET-ASYNCEVENTPOLL-EPOLLEVENT-LEAK-ANALYSIS.ch.md` — AsyncEventPoll 析构清理(同类不同对象)
- `UBSOCKET-UMQ-RX-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-TX-EVENT-LEAK-ANALYSIS.ch.md` — RX/TX Event 泄露(不同类)
- 其他 ub_test/brpc 泄露文档
- 源码:`src/ubsocket/csrc/core/ubsocket_socket.cpp:49,56,178-199`、`ubsocket_socket_connector.h:47-73`、`umq_socket_connector.h:22-94`、`ubsocket_socket.h:44-52,95`
