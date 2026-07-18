# UBSocket UmqConnectorOps non_aff_route_list vector 级联泄露分析(Connector 泄露之深层二)

> **现象**:ASan 报告(**Indirect leak**)
> ```
> Indirect leak of 80 byte(s) in 1 object(s) allocated from:
>     #1 std::__new_allocator<umq_route>::allocate
>     #2-#3 allocator_traits::allocate → _Vector_base<umq_route>::_M_allocate
>     #4 vector<umq_route>::_M_allocate_and_copy
>     #5 vector<umq_route>::operator= vector.tcc:238
>     #6 UmqConnectorOps::DoRoute umq_socket_connector.cpp:533
>     #7 ConnectNegotiate :472 → Negotiate :176 → Connector::Connect
>     ... → brpc::Socket::Connect socket.cpp:1343 → PerformanceTest::Init
> ```
>
> 本文确认其为 `UBSOCKET-UMQ-CONNECTOR-OPS-VECTOR-LEAK-ANALYSIS.ch.md`(back_routes_ 160B)§2 表已 anticipated 的**更深层级联**——`UmqConnectorOps::non_aff_route_list_`(`std::vector<umq_route>`)的内部缓冲,在 `DoRoute:533` `non_aff_route_list_ = back_routes`(copy assignment)时分配。

## 1. 与 back_routes_(160B)的同家族关系

`UmqConnectorOps` 内部有多个 `std::vector<umq_route>` 成员,均在 `DoRoute`(CLOS 光组网选路)中填充,均为 Connector→UmqConnectorOps(480B)的更深层级联:

| UmqConnectorOps 成员 | 类型 | 填充点 | 大小 | ASan 报告 |
|---------------------|------|--------|------|-----------|
| `back_routes_` | `std::vector<umq_route_t>` | `DoRoute:552` `push_back` | 160B(capacity 4) | 已文档(indirect) |
| **`non_aff_route_list_`** | **`std::vector<umq_route_t>`** | **`DoRoute:533` `operator=`(copy)** | **80B(capacity 2)** | **本报告(indirect)** |
| `peer_all_socket_ids_` | `std::vector<uint32_t>` | `ConnectNegotiate:464-466` | ? | 应有报告 |
| `umq_conn_info_.peer_ip` | `std::string` | 协商时赋值 | ? | 应有报告 |

两者同属 `UmqConnectorOps` 内部 vector 级联,随 `delete connector_` 一并消除。

## 2. 泄露对象

- **对象**:`std::vector<umq_route>` 的内部堆缓冲(`UmqConnectorOps::non_aff_route_list_` 的底层 array)。
- **分配点**:`UmqConnectorOps::DoRoute`(`umq_socket_connector.cpp:533`):
  ```cpp
  // DoRoute CLOS 分支(:519-553)
  std::vector<umq_route_t> main_routes;      // 亲和组
  std::vector<umq_route_t> back_routes;      // 不亲和组
  ...
  int getAffinityRes = GetCpuAffinityUmqRoute(filtered_list, main_routes, back_routes);
  ...
  non_aff_route_list_ = back_routes;          // :533 ← copy assignment → _M_allocate_and_copy → new[]
  ```
  `non_aff_route_list_ = back_routes` 调 `vector::operator=`(`vector.tcc:238`),`_M_allocate_and_copy` 分配新缓冲并复制 `back_routes`(不亲和组)内容。
- **大小**:80 字节。`sizeof(umq_route)=40B`(同 back_routes_ 报告),`80 / 40 = 2` → `non_aff_route_list_` 容纳 2 条不亲和路由(capacity 2)。
- **归属**:`UmqConnectorOps::non_aff_route_list_`(`umq_socket_connector.h:86` `std::vector<umq_route_t> non_aff_route_list_`),经 `UmqConnectorOps` → `Connector::connector_ops_`(`Ref`)→ `Connector` → `SocketBase::connector_`。

## 3. back_routes_ vs non_aff_route_list_ 的区别

| 维度 | `back_routes_`(160B) | `non_aff_route_list_`(80B,本报告) |
|------|----------------------|-----------------------------------|
| 语义 | RR 选出的备路组(最多 3 条) | 不亲和组路由(容灾重试用) |
| 填充方式 | `push_back` 循环(`:552`) | `operator=` copy(`:533`) |
| 内容来源 | `conn_back_routes`(RR 选择后) | `back_routes` local(GetCpuAffinityUmqRoute 输出的不亲组) |
| capacity | 4(最多 3 条 + 增长) | 2(本环境 2 条不亲和路由) |
| 用途 | 主备路通信 | `CheckOtherRouteForClos` 容灾重试时从此取备路 |

两者独立填充,各自分配独立缓冲,均为 `UmqConnectorOps` 成员,随 `~UmqConnectorOps` 默认析构 `~vector` 释放。

## 4. 为何泄露(同 Connector 家族,深层级联)

`UBSOCKET-UMQ-CONNECTOR-LEAK-ANALYSIS.ch.md` §4 已确认:`SocketBase::connector_` 全仓 **0 处 `delete`**。

级联效应(三层):
- `connector_` 不 delete → `~Connector` 从不执行
- `~Connector` 不执行 → `connector_ops_`(`Ref<ConnectorOps>`)析构不触发 → 不 DecreaseRef → `UmqConnectorOps` 不 `delete`
- `UmqConnectorOps` 不 `delete` → `~UmqConnectorOps`(编译器默认析构)不执行 → `non_aff_route_list_`(`std::vector`)析构不触发
- `~vector<umq_route>` 不执行 → 内部缓冲(80B)不释放 → 泄露
- 同时 `back_routes_`(160B)/`peer_all_socket_ids_`/`umq_conn_info_.peer_ip` 也不释放

**根因与 back_routes_(160B)/UmqConnectorOps(480B)/Connector(16B)泄露完全相同**:`connector_` 无 delete → Connector 全部子对象(含 UmqConnectorOps 及其内部全部 vector/string)级联失主。本 80B 是 `non_aff_route_list_` 层级的 indirect。

## 5. 为何 80B / 1 个

- `non_aff_route_list_ = back_routes` 复制 `back_routes` local(不亲和组),本环境 `GetCpuAffinityUmqRoute` 返回 2 条不亲和路由 → capacity 2 → `2 × 40B = 80B`。
- 1 个 = 单 `UmqConnectorOps` 的 `non_aff_route_list_` 缓冲。本次仅 1 个 UmqSocket 被销毁 → 1 个 80B 泄露。
- CLOS 光组网模式才有(`DoRoute` CLOS 分支);电组网走 FULLMESH_1D 不填 `non_aff_route_list_`。

## 6. 触发条件

- `UBSOCKET_UB_TRANS_MODE` 走 CLOS 光组网(`topo_type_==UMQ_TOPO_TYPE_CLOS`)→ `DoRoute` CLOS 分支 → `GetCpuAffinityUmqRoute` 分亲和/不亲组 → `non_aff_route_list_ = back_routes`
- 任一 UmqSocket 被销毁即泄露 Connector + 全部级联(含本 80B non_aff_route_list_ + 160B back_routes_ + 480B UmqConnectorOps + 更深层)
- 与 UB 配置无关(仅依赖 CLOS 组网)

## 7. 修复方案

**与 `UBSOCKET-UMQ-CONNECTOR-LEAK-ANALYSIS.ch.md` §7、`UBSOCKET-UMQ-CONNECTOR-OPS-LEAK-ANALYSIS.ch.md` §7、`UBSOCKET-UMQ-CONNECTOR-OPS-VECTOR-LEAK-ANALYSIS.ch.md` §7 等共用伞形修复**——`~SocketBase` 补 `delete connector_`:

```cpp
~SocketBase() override {
    delete acceptor_;
    acceptor_ = nullptr;
    delete connector_;           // ← 触发级联:~Connector → ~Ref<ConnectorOps> → DecreaseRef → delete UmqConnectorOps
    connector_ = nullptr;        //   → ~UmqConnectorOps → ~vector(non_aff_route_list_) → 释放 80B 本报告
                                //   + ~vector(back_routes_) → 释放 160B + ~vector(peer_all_socket_ids_) + ~string(peer_ip)
    // ... tx_/rx_/rxQueue/mutex_ 见各自文档
    if (GlobalSetting::UBS_TRACE_ENABLED) { ... }
}
```

`delete connector_` → `~Connector` → `~Ref<ConnectorOps>` → DecreaseRef → 0 → `delete UmqConnectorOps` → `~UmqConnectorOps`(默认析构)→ `~vector<umq_route>`(`non_aff_route_list_`)释放 80B + `~vector`(back_routes_)释放 160B + `~vector`(peer_all_socket_ids_)+ `~string`(peer_ip)。**一处 delete 覆盖 Connector 全部三层级联**(Connector 16B + UmqConnectorOps 480B + back_routes_ 160B + 本 non_aff_route_list_ 80B + 更深层)。

`UmqConnectorOps` 的 vector/string 成员由默认析构自动释放——无需额外代码,只要 `delete connector_` 触发整条链。

### 与伞形析构链 PR 的关系

本修复是伞形析构链 PR 的 `connector_` 子项(与 back_routes_ 160B 同,`delete connector_` 经 `~UmqConnectorOps` 自动覆盖全部 vector)。

## 8. 验证

修复后 ASan 重跑:本 80B indirect + back_routes_ 160B + UmqConnectorOps 480B + Connector 16B + 更深层(peer_all_socket_ids_/peer_ip)**同时消失**。可用小 `thread_num` + CLOS 模式短测验证。

## 9. 关键代码位置索引

| 位置 | 作用 | 泄露相关性 |
|------|------|-----------|
| `umq_socket_connector.cpp:533` | `non_aff_route_list_ = back_routes`(copy assignment) | **本报告 80B 分配点** |
| `umq_socket_connector.cpp:519-553` | `DoRoute` CLOS 分支,`GetCpuAffinityUmqRoute` 分组 | 触发 copy |
| `umq_socket_connector.h:86` | `std::vector<umq_route_t> non_aff_route_list_` 成员 | vector 归属 UmqConnectorOps |
| `umq_socket_connector.h:85` | `std::vector<umq_route_t> back_routes_` 成员 | 前报告(160B) |
| `umq_socket_connector.h:82` | `std::vector<uint32_t> peer_all_socket_ids_` | 更深层级联 |
| `umq_socket_connector.h:36-42` | `UmqConnInfo umq_conn_info_`(含 string peer_ip) | 更深层级联(string) |
| `ubsocket_socket.cpp:187` | `new UmqConnectorOps`(层级2,480B) | 父级联 |
| `ubsocket_socket.cpp:56` | `new Connector`(层级1,16B) | 根级联 |
| `ubsocket_socket.h:44-52,95` | `~SocketBase` 不 delete `connector_` | 根因 |

## 10. 与其他泄露的关系

| 维度 | 本泄露(non_aff 80B) | back_routes_ 160B | UmqConnectorOps 480B | Connector 16B | Acceptor 系列(288B+408B+512B+504B) | rxQueue 级联(48B+320B+128KB+2056B) | UmqTxOps/RxOps 72/96B | mutex 48B | EpollMapper 104B | TX Event | RX | RespClosure/done | bvar 家族 | RpcMeta | AsyncEventPoll |
|------|---------------------|-------------------|----------------------|--------------|----------------------------------|--------------------------------------|----------------------|-----------|-----------------|----------|----|------------------|-----------|---------|----------------|
| 归属 | ubsocket core(Connector 深层) | 同 | ubsocket core | ubsocket core | ubsocket core | ubsocket core(umq) | ubsocket core | ubsocket core | ubsocket core | ubsocket umq | ubsocket umq | ub_test | brpc | brpc/protobuf | ubsocket core |
| 类别 | Connector 级联子四(non_aff vector) | Connector 级联子三(back_routes vector) | Connector 级联子二(Ref 不减) | 析构不 delete | 析构不 delete + 级联 | 析构不 delete rxQueue + 级联 | 析构不 delete | 析构不 destroy | 全局 map 不清 | 退出未释放 | buffer 不回流 | 闭包/drain | bthread 抛弃 | 解析逃逸 | 析构不清 map |
| 与 Connector 关系 | **层级3 深层级联(non_aff_route_list_)** | 层级3(back_routes_) | 层级2 | 自身(层级1) | 独立 | 独立 | 独立 | 独立 | 独立 | 独立 | 独立 | 独立 | 独立 | 独立 | 同类不同对象 |
| ubs-comm 修复 | ✓(随 delete connector_ 级联消) | ✓(随 connector_) | ✓(随 connector_) | ✓(伞形) | ✓(伞形) | ✓(伞形) | ✓(伞形) | ✓(伞形) | ✓(独立) | ✓ | ✓ | ❌ | ❌ | ❌ | ✓ |

本泄露是 Connector 泄露的**第四层级联子对象**(`UmqConnectorOps::non_aff_route_list_` vector 缓冲),与 back_routes_(160B)、UmqConnectorOps(480B)、Connector(16B)同根,随 `~SocketBase` `delete connector_` 一并消除(经 `~UmqConnectorOps` 默认析构自动释放全部 vector)。属 ubs-comm 析构链伞形修复范围。

## 参考

- `UBSOCKET-UMQ-CONNECTOR-LEAK-ANALYSIS.ch.md` — Connector 16B direct(层级1)
- `UBSOCKET-UMQ-CONNECTOR-OPS-LEAK-ANALYSIS.ch.md` — UmqConnectorOps 480B indirect(层级2,§3 已 anticipated 更深层 vector)
- `UBSOCKET-UMQ-CONNECTOR-OPS-VECTOR-LEAK-ANALYSIS.ch.md` — back_routes_ 160B indirect(层级3,§2 表已 anticipated 本 non_aff_route_list_ 报告)
- `UBSOCKET-UMQ-ACCEPTOR-*-LEAK-ANALYSIS.ch.md` — Acceptor 系列(同析构链家族)
- `UBSOCKET-UMQ-DATAOPS-*-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-RXQUEUE-*-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-SOCKET-MUTEX-LEAK-ANALYSIS.ch.md` — 析构链家族其他子对象/级联
- `UBSOCKET-ASYNCEVENTPOLL-EPOLLEVENT-LEAK-ANALYSIS.ch.md` / `UBSOCKET-EPOLLMAPPER-LEAK-ANALYSIS.ch.md` — 析构清理缺失(同类不同对象)
- `UBSOCKET-UMQ-RX-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-TX-EVENT-LEAK-ANALYSIS.ch.md` — RX/TX Event 泄露(不同类)
- 其他 ub_test/brpc 泄露文档
- 源码:`src/ubsocket/csrc/core/umq/umq_socket_connector.cpp:519-556,472,176`、`umq_socket_connector.h:82-86`、`ubsocket_socket.cpp:56,187`、`ubsocket_socket.h:44-52,95`
