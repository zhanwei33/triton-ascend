# UBSocket UmqAcceptorOps 级联泄露分析(Acceptor 泄露之子三)

> **现象**:ASan 报告(**Indirect leak**——间接泄露,挂在父对象 `Acceptor` 之下)
> ```
> Indirect leak of 408 byte(s) in 1 object(s) allocated from:
>     #1 SocketBase::CreateAcceptorOps ubsocket_socket.cpp:161
>     #2 SocketBase::Create ubsocket_socket.cpp:43
>     #3 ubsocket_socket ubsocket_sock.cpp:38
>     ... → brpc::Socket::Connect socket.cpp:1334 → PerformanceTest::Init
> ```
>
> 本文确认其为 `UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md`(Acceptor 288B direct)§1 表已 anticipated 的**级联子对象**——`Acceptor::acceptor_ops_`(`Ref<AcceptorOps>`)持有的 `UmqAcceptorOps`,与 `UBSOCKET-UMQ-CONNECTOR-OPS-LEAK-ANALYSIS.ch.md`(Connector→UmqConnectorOps 480B)对称。

## 1. 与 Acceptor 泄露的级联关系

`Acceptor`(`SocketBase::acceptor_`,288B,direct)持 `Ref<AcceptorOps> acceptor_ops_` 智能指针(`ubsocket_socket_acceptor.h:108`)。`~Acceptor` 从不执行(`acceptor_` 全仓 0 处 `delete`)→ `~Ref` 不触发 → `UmqAcceptorOps` 引用不减 → 若 Acceptor 是唯一持有者则 `UmqAcceptorOps` 泄露。本 408B 即该级联:

| 层级 | 对象 | 成员 | 分配点 | 大小 | ASan 报告 |
|------|------|------|--------|------|-----------|
| 1 | `Acceptor` | `SocketBase::acceptor_` 裸指针 | `ubsocket_socket.cpp:55` `new Acceptor` | 288B | 已文档(direct) |
| 2a | `UmqAcceptorOps` | `Acceptor::acceptor_ops_`(`Ref<AcceptorOps>`) | `ubsocket_socket.cpp:161` `new UmqAcceptorOps` | **408B** | **本报告(indirect)** |
| 2b | `ubSocket_async_accept_info.lock` | mutex | Acceptor ctor(`:62`) | 48B | Acceptor 文档 §6(应有报告) |
| 2c | `ubSocket_async_accept_info.ready_queue` | `std::queue<tuple>` deque chunk | AsyncAcceptInfo 默认 ctor | 504B | 已文档(indirect) |
| 2d | `wakeup_event_.ready_event_queue_` | `std::queue<int>` deque chunk | UbsocketWakeupEvent ctor | 512B | 已文档(indirect) |
| 2e | `wakeup_event_.ready_event_mutex_` | mutex | `ubsocket_wakeup_event.cpp:18` | 48B | 应有报告(indirect) |

`UmqAcceptorOps` 内部还有 `peer_all_socket_ids_`/`back_routes_` 等 vector 与 `umq_conn_info_`(含 string peer_ip + 4×umq_eid_t)——更深层级联(若有独立 ASan 报告)。

## 2. 与 Connector→UmqConnectorOps 对称

两份 Ops 级联报告对称(均 `Ref<*Ops>` 持有的 `Umq*Ops`):

| 报告 | 父对象 | 持有者 | Ops 对象 | 大小 | 文档 |
|------|--------|--------|---------|------|------|
| 480B(indirect) | `Connector`(16B direct) | `Connector::connector_ops_`(`Ref<ConnectorOps>`) | `UmqConnectorOps` | 480B | 已文档(Connector Ops) |
| **408B(indirect)** | **`Acceptor`(288B direct)** | **`Acceptor::acceptor_ops_`(`Ref<AcceptorOps>`)** | **`UmqAcceptorOps`** | **408B** | **本报告** |

大小差异:`UmqAcceptorOps`(408B)< `UmqConnectorOps`(480B),因 `UmqAcceptorOps` 无 `non_aff_route_list_` vector 且成员略少(umq_socket_acceptor.h:22-82 vs umq_socket_connector.h:22-94)。

## 3. 泄露对象

- **对象**:`ock::ubs::umq::UmqAcceptorOps`(`umq_socket_acceptor.h:22`,继承 `AcceptorOps`)。
- **分配点**:`SocketBase::CreateAcceptorOps`(`ubsocket_socket.cpp:161`):
  ```cpp
  auto umqOps = new (std::nothrow) UmqAcceptorOps(sock->raw_socket_);
  ...
  acceptorOps = umqOps;   // :167 传出
  ```
  经 `SocketBase::Create`(`:43` `CreateAcceptorOps(...)`)→ `Acceptor` ctor(`ubsocket_socket.cpp:55` `new Acceptor(sock, acceptorOps)`)→ 存入 `Acceptor::acceptor_ops_`(`ubsocket_socket_acceptor.h:108` `Ref<AcceptorOps>`)。
- **大小**:408 字节。`UmqAcceptorOps` 成员(`umq_socket_acceptor.h:22-82`):
  - `UmqConnInfo umq_conn_info_`(`:44-50`:继承 `ConnInfo` + 4×`umq_eid_t` 各 16B = 64B)
  - `int peer_socket_id_` + `std::vector<uint32_t> peer_all_socket_ids_`(~24B shell)
  - `ub_trans_mode umq_trans_mode_` + `bool umq_enable_share_jfr_` + `dev_schedule_policy umq_schedule_policy_`/`peer_schedule_policy_`
  - `umq_topo_type_t topo_type_` + `umq_route_t conn_route_` + `std::vector<umq_route_t> back_routes_`(~24B)
  - `bool degradable_` + `OtherRouteMessage other_route_message_` + `UBHandshakeState retry_state_`
  - 继承 `AcceptorOps`(`ubsocket_socket_acceptor.h:23-54`):`RawConnInfoV4 conn_info`(string peer_ip + int + int + time_point)+ `int fd`/`event_fd` + ref count
  - 合计对齐 408B(vector/string 内部缓冲独立堆分配,不计入)。
- **归属**:`Acceptor::acceptor_ops_`(`Ref<AcceptorOps>`),经 `Acceptor` 归属 `SocketBase::acceptor_`。

## 4. 引用计数与级联

`Ref<AcceptorOps>` 引用计数智能指针(`ubsocket_ref.h`):

- `CreateAcceptorOps` `new UmqAcceptorOps` → ref_count=1(经 `acceptorOps = umqOps` 传入 Acceptor ctor 时 `Ref` 构造 IncreaseRef)。
- `Acceptor::acceptor_ops_` 是**唯一** `Ref<AcceptorOps>` 持有者。
- 正常清理:`~Acceptor` → `~Ref<AcceptorOps>` → DecreaseRef → ref 1→0 → `delete` UmqAcceptorOps(408B 释放)。
- 泄露:`~Acceptor` 从不执行(`acceptor_` 无 delete)→ `~Ref` 不触发 → ref 不减 → `UmqAcceptorOps` 不释放 → 408B 泄露。

`UmqAcceptorOps` 内部 `umq_conn_info_.peer_ip`(`std::string`)+ `peer_all_socket_ids_`/`back_routes_` 的 vector 内部缓冲是**更深层级联**——`~UmqAcceptorOps` 不执行 → 不释放(应有独立报告)。

## 5. 为何泄露(同 Acceptor 家族,级联)

`UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md` §5 已确认:`SocketBase::acceptor_` 全仓 **0 处 `delete`**,`~SocketBase`/`UnInitialize`/`~UmqSocket` 均不 delete。

级联效应:
- `acceptor_` 不 delete → `~Acceptor`(`ubsocket_socket_acceptor.cpp:300-305`)从不执行
- `~Acceptor` 不执行 → `acceptor_ops_`(`Ref<AcceptorOps>`)析构不触发
- `~Ref<AcceptorOps>` 不执行 → 不 DecreaseRef → `UmqAcceptorOps` ref 不归零 → 不 `delete` → 408B 泄露
- 同时 `UmqAcceptorOps` 内部 string/vector 缓冲也不释放(更深层级联)

**根因与 Acceptor/Connector Ops 泄露完全相同**:`acceptor_`/`connector_` 无 delete → Acceptor/Connector 全部子对象级联失主。本 408B 是 Acceptor 系列级联 indirect 之三(前两:512B wakeup queue、504B ready_queue)。

## 6. 为何 408B / 1 个

- `sizeof(UmqAcceptorOps)` = 408B(对象 shell,不含 vector/string 内部缓冲)。
- 1 个 = 单 Acceptor 的 `acceptor_ops_` 持有的 UmqAcceptorOps。本次仅 1 个 UmqSocket 被销毁 → 1 个 408B 泄露。
- 内部 vector/string 缓冲若非空,应有额外 indirect 报告。

## 7. 触发条件

与 Acceptor 家族完全一致:任一 UmqSocket 被销毁即泄露 Acceptor + 全部级联(含本 408B UmqAcceptorOps + 512B wakeup queue + 504B ready_queue + 两处 mutex + 更深层 string/vector)。与 UB 配置无关。

## 8. 修复方案

**与 `UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md` §7、`UBSOCKET-UMQ-CONNECTOR-OPS-LEAK-ANALYSIS.ch.md` §7 等共用伞形修复**——`~SocketBase` 补 `delete acceptor_`:

```cpp
~SocketBase() override {
    delete acceptor_;           // ← 触发级联:~Acceptor → ~Ref<AcceptorOps> → DecreaseRef → delete UmqAcceptorOps(408B)
    acceptor_ = nullptr;        //   + ~Acceptor → ~UbsocketWakeupEvent(512B queue+mutex) + ~AsyncAcceptInfo(504B queue+mutex)
    delete connector_;          //   + ~Connector → ~Ref<ConnectorOps> → delete UmqConnectorOps(480B)
    connector_ = nullptr;
    // ... tx_/rx_/rxQueue/mutex_ 见各自文档
    if (GlobalSetting::UBS_TRACE_ENABLED) { ... }
}
```

`delete acceptor_` → `~Acceptor` → `~Ref<AcceptorOps>` → DecreaseRef → 0 → `delete UmqAcceptorOps` → `~UmqAcceptorOps` → `~string`(peer_ip)+ `~vector`(peer_all_socket_ids_/back_routes_)释放内部缓冲。**一处 delete 覆盖 Acceptor 全部级联**(本 408B + 512B + 504B + 两 mutex + 更深层)。

`~Acceptor` 还需补 `LockRegistry::LOCK_OPS.destroy(ubSocket_async_accept_info.lock)`(Acceptor 文档 §6 方案2)——`u_mutex_t*` 是 `void*` 裸指针,`~Acceptor` 默认析构不调 destroy。

### 与伞形析构链 PR 的关系

本修复是伞形析构链 PR 的 `acceptor_` 子项(与 `delete connector_`、`~DataTx`/`~DataRx` delete tx_ops_/rx_ops_、`~UmqSocket` delete rxQueue + destroy mutex_ 一并合并)。至此 Acceptor 级联子对象(observed:288B+408B+504B+512B + 预期 2 mutex)+ Connector 级联(16B+480B)+ rxQueue 级联(48B+128KB+2056B)+ UmqTxOps/RxOps(72B/96B)+ mutex(48B)全部纳入伞形。

## 9. 验证

修复后 ASan 重跑:本 408B indirect + Acceptor 288B direct + Acceptor 全部级联(512B/504B/mutex/更深层)+ Connector 16B/480B 等**同时消失**。可用小 `thread_num` 短测验证。

## 10. 关键代码位置索引

| 位置 | 作用 | 泄露相关性 |
|------|------|-----------|
| `ubsocket_socket.cpp:161` | `new UmqAcceptorOps` in `CreateAcceptorOps` | **本报告 408B 分配点** |
| `ubsocket_socket.cpp:43,55` | `Create` 调 `CreateAcceptorOps` + `new Acceptor(sock, ops)` | 调用链 |
| `ubsocket_socket_acceptor.h:108` | `Ref<AcceptorOps> acceptor_ops_` 智能指针 | 持有 UmqAcceptorOps(ref) |
| `umq_socket_acceptor.h:22-82` | `UmqAcceptorOps` 类(408B) | 泄露对象 |
| `umq_socket_acceptor.h:44-50` | `UmqConnInfo umq_conn_info_`(含 string peer_ip + 4×eid) | 更深层级联(string) |
| `ubsocket_socket_acceptor.cpp:300-305` | `~Acceptor`(只 trace,从不被调) | 级联释放入口(失效) |
| `ubsocket_socket.h:44-52,94` | `~SocketBase` 不 delete `acceptor_` | 根因 |

## 11. 与其他泄露的关系

| 维度 | 本泄露(UmqAcceptorOps 408B) | Acceptor 288B + 级联(512B/504B) | Connector 16B + 级联(480B) | rxQueue 级联(48B+128KB+2056B) | UmqTxOps/RxOps 72/96B | mutex 48B | TX Event | RX | RespClosure/done | bvar 家族 | RpcMeta | AsyncEventPoll |
|------|------------------------------|----------------------------|---------------------------|----------------------------|----------------------|-----------|----------|----|------------------|-----------|---------|----------------|
| 归属 | ubsocket core(Acceptor 子) | ubsocket core | ubsocket core | ubsocket core(umq) | ubsocket core | ubsocket core | ubsocket umq | ubsocket umq | ub_test | brpc | brpc/protobuf | ubsocket core |
| 类别 | Acceptor 级联子(Ref 不减) | 析构不 delete + 级联 | 析构不 delete + 级联 | 析构不 delete rxQueue | 析构不 delete | 析构不 destroy | 退出未释放 | buffer 不回流 | 闭包/drain | bthread 抛弃 | 解析逃逸 | 析构不清 map |
| 与 Acceptor 关系 | **级联子三(UmqAcceptorOps)** | 自身 | 独立(Connector) | 独立(rxQueue) | 独立(DataTxOps) | 独立(UmqSocket) | 独立 | 独立 | 独立 | 独立 | 独立 | 同类不同对象 |
| direct/indirect | indirect | direct+indirect | direct+indirect | direct+indirect | direct | direct | — | — | — | — | — | — |
| ubs-comm 修复 | ✓(随 delete acceptor_ 级联消) | ✓(伞形) | ✓(伞形) | ✓(伞形) | ✓(伞形) | ✓(伞形) | ✓ | ✓ | ❌ | ❌ | ❌ | ✓ |

本泄露是 Acceptor 泄露的**第三个级联子对象**(`acceptor_ops_` 持有的 UmqAcceptorOps),与 Connector→UmqConnectorOps(480B)对称,随 `~SocketBase` `delete acceptor_` 一并消除。属 ubs-comm 析构链伞形修复范围。

## 参考

- `UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md` — Acceptor 288B direct 泄露(§1 表已 anticipated 本 acceptor_ops_ 级联)
- `UBSOCKET-UMQ-ACCEPTOR-WAKEUP-QUEUE-LEAK-ANALYSIS.ch.md` — wakeup queue 512B(Acceptor 级联子一)
- `UBSOCKET-UMQ-ACCEPTOR-ASYNCACCEPT-QUEUE-LEAK-ANALYSIS.ch.md` — ready_queue 504B(Acceptor 级联子二)
- `UBSOCKET-UMQ-CONNECTOR-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-CONNECTOR-OPS-LEAK-ANALYSIS.ch.md` — Connector 系列(对称,UmqConnectorOps 480B)
- `UBSOCKET-UMQ-DATAOPS-*-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-RXQUEUE-*-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-SOCKET-MUTEX-LEAK-ANALYSIS.ch.md` — 析构链家族其他子对象/级联
- `UBSOCKET-ASYNCEVENTPOLL-EPOLLEVENT-LEAK-ANALYSIS.ch.md` — AsyncEventPoll 析构清理(同类不同对象)
- `UBSOCKET-UMQ-RX-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-TX-EVENT-LEAK-ANALYSIS.ch.md` — RX/TX Event 泄露(不同类)
- 其他 ub_test/brpc 泄露文档
- 源码:`src/ubsocket/csrc/core/ubsocket_socket.cpp:43,55,152-171`、`ubsocket_socket_acceptor.h:23-54,108`、`umq_socket_acceptor.h:22-82`、`ubsocket_socket.h:44-52,94`
