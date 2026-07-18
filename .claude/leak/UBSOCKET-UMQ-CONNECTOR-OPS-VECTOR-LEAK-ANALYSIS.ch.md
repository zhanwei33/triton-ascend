# UBSocket UmqConnectorOps back_routes vector 级联泄露分析(Connector 泄露之深层)

> **现象**:ASan 报告(**Indirect leak**——间接泄露,挂在 `UmqConnectorOps` 之下,经 `Connector` 到 `connector_`)
> ```
> Indirect leak of 160 byte(s) in 1 object(s) allocated from:
>     #1 std::__new_allocator<umq_route>::allocate
>     #2 allocator_traits::allocate → _Vector_base<umq_route>::_M_allocate stl_vector.h:383
>     #3 vector<umq_route>::_M_realloc_append
>     #4 vector<umq_route>::push_back stl_vector.h:1307
>     #5 UmqConnectorOps::DoRoute umq_socket_connector.cpp:552
>     #6 UmqConnectorOps::ConnectNegotiate umq_socket_connector.cpp:472
>     #7 UmqConnectorOps::Negotiate :176
>     ... → Connector::Connect → ubsocket_connect → brpc::Socket::Connect → PerformanceTest::Init
> ```
>
> 本文确认其为 `UBSOCKET-UMQ-CONNECTOR-OPS-LEAK-ANALYSIS.ch.md`(UmqConnectorOps 480B indirect)§3 已 anticipated 的**更深层级联**——`UmqConnectorOps::back_routes_`(`std::vector<umq_route>`)的内部缓冲。

## 1. 级联关系(Connector 泄露的第三层)

`UBSOCKET-UMQ-CONNECTOR-LEAK-ANALYSIS.ch.md`(Connector 16B direct)+ `UBSOCKET-UMQ-CONNECTOR-OPS-LEAK-ANALYSIS.ch.md`(UmqConnectorOps 480B indirect)已建立级联链,本报告是其**第三层更深级联**:

| 层级 | 对象 | 持有者 | 分配点 | 大小 | ASan 报告 |
|------|------|--------|--------|------|-----------|
| 1 | `Connector` | `SocketBase::connector_` 裸指针 | `ubsocket_socket.cpp:56` `new Connector` | 16B | 已文档(direct) |
| 2 | `UmqConnectorOps` | `Connector::connector_ops_`(`Ref<ConnectorOps>`) | `ubsocket_socket.cpp:187` `new UmqConnectorOps` | 480B | 已文档(indirect) |
| **3** | **`UmqConnectorOps::back_routes_` vector 内部缓冲** | **`UmqConnectorOps::back_routes_`(`std::vector<umq_route>`)** | **`umq_socket_connector.cpp:552` `push_back` 触发 `_M_realloc_append`** | **160B** | **本报告(indirect)** |

`UmqConnectorOps` 内部还有其他 vector/string(更深层级联,应有独立报告):

| UmqConnectorOps 成员 | 类型 | 分配/填充点 | 预期报告 |
|---------------------|------|------------|----------|
| **`back_routes_`** | **`std::vector<umq_route_t>`** | **`DoRoute:552` `push_back`** | **本报告(160B)** |
| `non_aff_route_list_` | `std::vector<umq_route_t>` | `DoRoute:533` 赋值 | 应有报告(CLOS 模式) |
| `peer_all_socket_ids_` | `std::vector<uint32_t>` | `ConnectNegotiate:464-466` `push_back` | 应有报告 |
| `umq_conn_info_.peer_ip` | `std::string` | 协商时赋值 | 应有报告 |

## 2. 泄露对象

- **对象**:`std::vector<umq_route>` 的内部堆缓冲(`UmqConnectorOps::back_routes_` 的底层 array)。
- **分配点**:`UmqConnectorOps::DoRoute`(`umq_socket_connector.cpp:552`):
  ```cpp
  // DoRoute CLOS 分支(:549-553)
  back_routes_.clear();
  for (const auto &br : conn_back_routes) {
      back_routes_.push_back(br);   // :552 ← push_back 触发 _M_realloc_append → _M_allocate → new[]
  }
  ```
  `back_routes_` 在 `clear()` 后 `push_back` `conn_back_routes`(最多 `NegotiateRoute::BACK_ROUTE_MAX_NUM=3` 条备路)。libstdc++ vector 增长 1→2→4,push 3 条 → capacity=4 → 分配 `4 × sizeof(umq_route)` 缓冲。
- **大小**:160 字节。`160 / 4 = 40` → `sizeof(umq_route)=40B`(umq 库 `umq_route`/`umq_route_t` 结构,含 `src_port`/`dst_port`/`src_eid`/`dst_eid` 字段组合,具体布局见 `umq_types.h`)。capacity=4 × 40B = 160B。
- **归属**:`UmqConnectorOps::back_routes_`(`umq_socket_connector.h:85` `std::vector<umq_route_t> back_routes_`),经 `UmqConnectorOps` → `Connector::connector_ops_`(`Ref`)→ `Connector` → `SocketBase::connector_`。

## 3. 调用栈解读

```
ub_test client PerformanceTest::Init → stub.Test → Channel::CallMethod → IssueRPC
  → brpc Socket::Write → ... → Connect (socket.cpp:1343)
    → ubsocket_wrapper_connect → ubsocket_connect → Connector::Connect
      → Negotiate (umq_socket_connector.cpp:176)
        → ConnectNegotiate (:472): DoRoute
          → DoRoute (:552): back_routes_.push_back(br)  ← 泄露分配(160B vector 缓冲)
```

client 建链 `Connector::Connect` → `Negotiate` → `ConnectNegotiate` → `DoRoute`(CLOS 光组网选路)→ `back_routes_.push_back` 填充备路组(最多 3 条 + capacity 4)。

## 4. 为何泄露(同 Connector 家族,深层级联)

`UBSOCKET-UMQ-CONNECTOR-LEAK-ANALYSIS.ch.md` §4 已确认:`SocketBase::connector_` 全仓 **0 处 `delete`**。

级联效应(三层):
- `connector_` 不 delete → `~Connector` 从不执行
- `~Connector` 不执行 → `connector_ops_`(`Ref<ConnectorOps>`)析构不触发 → 不 DecreaseRef → `UmqConnectorOps` 不 `delete`
- `UmqConnectorOps` 不 `delete` → `~UmqConnectorOps`(编译器默认析构)不执行 → `back_routes_`(`std::vector`)析构不触发
- `~vector<umq_route>` 不执行 → 内部缓冲(160B)不释放 → 泄露
- 同时 `non_aff_route_list_`/`peer_all_socket_ids_`/`umq_conn_info_.peer_ip` 也不释放(更深层级联,应有独立报告)

**根因与 Connector/UmqConnectorOps 泄露完全相同**:`connector_` 无 delete → Connector 全部子对象(含 UmqConnectorOps 及其内部 vector/string)级联失主。本 160B 是第三层 indirect。

## 5. 为何 160B / 1 个

- `back_routes_` 经 `clear()` + `push_back` 最多 3 条备路 → capacity 增长到 4 → `4 × 40B = 160B`。
- 1 个 = 单 `UmqConnectorOps` 的 `back_routes_` 缓冲。本次仅 1 个 UmqSocket 被销毁(同 Connector 16B/1 的"1 个")→ 1 个 160B 缓冲泄露。
- CLOS 光组网模式下 `DoRoute` 走 `back_routes_.push_back` 分支(`:552`);电组网(FULLMESH_1D)走 `back_routes_.clear()`(`:518`)不 push_back,无本报告。本报告存在说明环境用 CLOS 模式。

## 6. 触发条件

- `UBSOCKET_UB_TRANS_MODE` 走 CLOS 光组网(`topo_type_==UMQ_TOPO_TYPE_CLOS`)→ `DoRoute` CLOS 分支 → `back_routes_.push_back`
- 任一 UmqSocket 被销毁即泄露 Connector + 全部级联(含本 160B back_routes_ 缓冲 + UmqConnectorOps 480B + 更深层)
- 与 UB 配置无关(仅依赖 CLOS 组网)

## 7. 修复方案

**与 `UBSOCKET-UMQ-CONNECTOR-LEAK-ANALYSIS.ch.md` §7、`UBSOCKET-UMQ-CONNECTOR-OPS-LEAK-ANALYSIS.ch.md` §7 等共用伞形修复**——`~SocketBase` 补 `delete connector_`:

```cpp
~SocketBase() override {
    delete acceptor_;
    acceptor_ = nullptr;
    delete connector_;           // ← 触发级联:~Connector → ~Ref<ConnectorOps> → DecreaseRef → delete UmqConnectorOps
    connector_ = nullptr;        //   → ~UmqConnectorOps → ~vector<umq_route>(back_routes_) → 释放 160B 本报告
                                //   + ~vector(non_aff_route_list_/peer_all_socket_ids_) + ~string(peer_ip) 释放更深层
    // ... tx_/rx_/rxQueue/mutex_ 见各自文档
    if (GlobalSetting::UBS_TRACE_ENABLED) { ... }
}
```

`delete connector_` → `~Connector` → `~Ref<ConnectorOps>` → DecreaseRef → 0 → `delete UmqConnectorOps` → `~UmqConnectorOps`(默认析构)→ `~vector<umq_route>`(back_routes_)释放 160B + `~vector`(non_aff_route_list_/peer_all_socket_ids_)+ `~string`(peer_ip)。**一处 delete 覆盖 Connector 全部三层级联**(Connector 16B + UmqConnectorOps 480B + 本 160B + 更深层 vector/string)。

### 与伞形析构链 PR 的关系

本修复是伞形析构链 PR 的 `connector_` 子项(与 `delete acceptor_`、`~DataTx`/`~DataRx` delete tx_ops_/rx_ops_、`~UmqSocket` delete rxQueue + destroy mutex_ 一并合并)。`UmqConnectorOps` 是 `Ref<ConnectorOps>` 智能指针持有,`~UmqConnectorOps` 默认析构自动释放其 vector/string 成员——无需额外代码,只要 `delete connector_` 触发整条链。

## 8. 验证

修复后 ASan 重跑:本 160B indirect + UmqConnectorOps 480B indirect + Connector 16B direct + 更深层(non_aff_route_list_/peer_all_socket_ids_/peer_ip)**同时消失**。可用小 `thread_num` + CLOS 模式短测验证。

## 9. 关键代码位置索引

| 位置 | 作用 | 泄露相关性 |
|------|------|-----------|
| `umq_socket_connector.cpp:552` | `back_routes_.push_back(br)` in `DoRoute` | **本报告 160B 分配点** |
| `umq_socket_connector.cpp:549-553` | `DoRoute` CLOS 分支填 `back_routes_` | 触发 push_back |
| `umq_socket_connector.h:85` | `std::vector<umq_route_t> back_routes_` 成员 | vector 归属 UmqConnectorOps |
| `umq_socket_connector.h:86` | `std::vector<umq_route_t> non_aff_route_list_` | 更深层级联(CLOS) |
| `umq_socket_connector.h:82` | `std::vector<uint32_t> peer_all_socket_ids_` | 更深层级联 |
| `umq_socket_connector.h:36-42` | `UmqConnInfo umq_conn_info_`(含 string peer_ip) | 更深层级联(string) |
| `ubsocket_socket.cpp:187` | `new UmqConnectorOps`(层级2) | 父级联(480B) |
| `ubsocket_socket.cpp:56` | `new Connector`(层级1) | 根级联(16B) |
| `ubsocket_socket.h:44-52,95` | `~SocketBase` 不 delete `connector_` | 根因 |

## 10. 与其他泄露的关系

| 维度 | 本泄露(back_routes_ 160B 层级3) | UmqConnectorOps 480B 层级2 | Connector 16B 层级1 | Acceptor 系列(288B+408B+512B+504B) | rxQueue 级联(48B+320B+128KB+2056B) | UmqTxOps/RxOps 72/96B | mutex 48B | TX Event | RX | RespClosure/done | bvar 家族 | RpcMeta | AsyncEventPoll |
|------|------------------------------|---------------------------|---------------------|----------------------------------|--------------------------------------|----------------------|-----------|----------|----|------------------|-----------|---------|----------------|
| 归属 | ubsocket core(Connector 深层) | ubsocket core | ubsocket core | ubsocket core | ubsocket core(umq) | ubsocket core | ubsocket core | ubsocket umq | ubsocket umq | ub_test | brpc | brpc/protobuf | ubsocket core |
| 类别 | Connector 级联子三(vector 缓冲) | Connector 级联子二(Ref 不减) | 析构不 delete | 析构不 delete + 级联 | 析构不 delete rxQueue + 级联 | 析构不 delete | 析构不 destroy | 退出未释放 | buffer 不回流 | 闭包/drain | bthread 抛弃 | 解析逃逸 | 析构不清 map |
| 与 Connector 关系 | **层级3 深层级联(back_routes_)** | 层级2 | 自身(层级1) | 独立(Acceptor) | 独立(rxQueue) | 独立(DataTxOps) | 独立(UmqSocket) | 独立 | 独立 | 独立 | 独立 | 独立 | 同类不同对象 |
| direct/indirect | indirect | indirect | direct | direct+indirect | direct+indirect | direct | direct | — | — | — | — | — | — |
| ubs-comm 修复 | ✓(随 delete connector_ 级联消) | ✓(随 connector_) | ✓(伞形) | ✓(伞形) | ✓(伞形) | ✓(伞形) | ✓(伞形) | ✓ | ✓ | ❌ | ❌ | ❌ | ✓ |

本泄露是 Connector 泄露的**第三层级联子对象**(`UmqConnectorOps::back_routes_` vector 缓冲),与 Connector 16B、UmqConnectorOps 480B 同根,随 `~SocketBase` `delete connector_` 一并消除(经 `~UmqConnectorOps` 默认析构自动释放 vector)。属 ubs-comm 析构链伞形修复范围。

## 参考

- `UBSOCKET-UMQ-CONNECTOR-LEAK-ANALYSIS.ch.md` — Connector 16B direct(层级1)
- `UBSOCKET-UMQ-CONNECTOR-OPS-LEAK-ANALYSIS.ch.md` — UmqConnectorOps 480B indirect(层级2,§3 已 anticipated 本更深层级联)
- `UBSOCKET-UMQ-ACCEPTOR-*-LEAK-ANALYSIS.ch.md` — Acceptor 系列(同析构链家族)
- `UBSOCKET-UMQ-DATAOPS-*-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-RXQUEUE-*-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-SOCKET-MUTEX-LEAK-ANALYSIS.ch.md` — 析构链家族其他子对象/级联
- `UBSOCKET-ASYNCEVENTPOLL-EPOLLEVENT-LEAK-ANALYSIS.ch.md` — AsyncEventPoll 析构清理(同类不同对象)
- `UBSOCKET-UMQ-RX-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-TX-EVENT-LEAK-ANALYSIS.ch.md` — RX/TX Event 泄露(不同类)
- 其他 ub_test/brpc 泄露文档
- 源码:`src/ubsocket/csrc/core/umq/umq_socket_connector.cpp:546-556,472,176`、`umq_socket_connector.h:82-86`、`ubsocket_socket.cpp:56,187`、`ubsocket_socket.h:44-52,95`
