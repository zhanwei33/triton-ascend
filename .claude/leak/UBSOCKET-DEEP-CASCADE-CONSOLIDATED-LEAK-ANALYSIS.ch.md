# UBSocket 深层级联泄露汇总分析(四条 anticipated 报告)

> **现象**:四条 ASan indirect leak,分属三个既有家族的更深层级联:
> - 40B:FastHeap 对象本体(rxQueue 级联 Level 2b)
> - 16B × 2:EpollMapper `epoll_set_` hash 节点(EpollMapper 级联 Level 2c)
> - 8B:UmqConnectorOps `peer_all_socket_ids_` vector 缓冲(Connector 级联更深层)
>
> **SUMMARY**: AddressSanitizer: 286535 byte(s) leaked in 4872 allocation(s)(全量汇总)

## 1. 四条泄露映射

| # | 大小 | 分配点 | 父对象 | 家族 | 对应文档 anticipated |
|---|------|--------|--------|------|---------------------|
| 1 | 40B | `UmqBufferReceiveQueue ctor :52`(`new FastHeap`) | `UmqBufferReceiveQueue::out_of_order_queue` | rxQueue 级联 Level 2b | `UBSOCKET-UMQ-RXQUEUE-FASTHEAP-LEAK-ANALYSIS.ch.md` §1(FastHeap 级联) |
| 2a | 16B | `EpollMapper::Add :83`(`unordered_set::insert` → `_Hash_node<int>`) | `EpollMapper::epoll_set_`(退出 Stop 路径) | EpollMapper 级联 Level 2c | `UBSOCKET-EPOLLMAPPER-LEAK-ANALYSIS.ch.md` §1(bucket 数组 104B 之更深层) |
| 2b | 16B | 同上(运行时 AddConsumer 路径) | `EpollMapper::epoll_set_`(运行时建链) | EpollMapper 级联 Level 2c | 同上 |
| 3 | 8B | `UmqConnectorOps::ConnectNegotiate :463`(`vector<uint32_t>::reserve`) | `UmqConnectorOps::peer_all_socket_ids_` | Connector 级联更深层 | `UBSOCKET-UMQ-CONNECTOR-OPS-VECTOR-LEAK-ANALYSIS.ch.md` §2 表(peer_all_socket_ids_ 应有报告) |

## 2. 各条详解

### 报告1:FastHeap 对象本体(40B,rxQueue 级联 Level 2b)

- **对象**:`FastHeap<umq_buf_t*, O3QueueComparator>` 对象本体(`ubsocket_fast_heap.h`),不是其内部 `InitHeap` 数组(2056B 已文档)。
- **分配点**:`UmqBufferReceiveQueue` ctor(`umq_buffer_receive_queue.cpp:47/52`):
  ```cpp
  out_of_order_queue = new (std::nothrow) FastHeap<umq_buf_t*, O3QueueComparator>(o3_queue_depth, o3_queue_depth);
  ```
- **大小**:40B = `sizeof(FastHeap<umq_buf_t*, O3QueueComparator>)`(对象 shell,不含 `InitHeap` 内部数组)。
- **rxQueue 级联完整层级(至此 5 层全部观测)**:

| 层级 | 对象 | 大小 | 状态 |
|------|------|------|------|
| 1 | `UmqBufferReceiveQueue` 本体 | 48B(direct) | 已文档 |
| 2a | `SPSCRingQueue` 对象本体 | 320B(indirect) | 已文档 |
| 3a | `SPSCRingQueue::buffer_` vector | 128KB(indirect) | 已文档 |
| 2b | **`FastHeap` 对象本体** | **40B(indirect)** | **本报告** |
| 4 | `FastHeap::InitHeap` 内部数组 | 2056B(indirect) | 已文档 |

`delete rxQueue` → `~UmqBufferReceiveQueue` → `ClearAllocations` → `delete out_of_order_queue`(`~FastHeap` 释放 40B 本体 + `InitHeap` 2056B)+ `delete receive_queue`(`~SPSCRingQueue` 释放 320B + 128KB)。**一处 `delete rxQueue` 覆盖全部 5 层。**

### 报告2a/2b:EpollMapper epoll_set_ hash 节点(16B × 2,EpollMapper 级联 Level 2c)

- **对象**:`std::_Hash_node<int, false>`(libstdc++ `unordered_set<int>` 的单个 hash 节点)。
- **分配点**:`EpollMapper::Add`(`ubsocket_event_epoll.h:83`)`epoll_set_.insert(epoll_fd)` → `_M_allocate_node` → `new _Hash_node<int>`(16B)。
- **大小**:16B = `sizeof(_Hash_node<int>)` = `_Hash_node_base*`(8B next) + `int`(4B value) + padding(4B) = 16B。
- **两条报告对应两条 `EpollMapper::Add` 路径**:
  - 2a:`EventDispatcher::Stop`(`:106`)→ exit(退出时 Stop 再 insert)
  - 2b:`EventDispatcher::AddConsumer`(`:172`)→ `Socket::ResetFileDescriptor`(`:643`)(运行时建链)
- **EpollMapper 级联完整层级(至此 4 层全部观测)**:

| 层级 | 对象 | 大小 | 状态 |
|------|------|------|------|
| 1 | `EpollMapper` 对象本体 | 72B | 已文档 |
| 2a | `epoll_set_` bucket 数组 | 104B | 已文档 |
| 2b | `mutex_` | 48B | 已文档(MUTEX 汇总) |
| 2c | **`epoll_set_` hash 节点** | **16B × N** | **本报告(2 个)** |

`ubsocket_uninit` 清全局 map `delete` 每个 EpollMapper → `~EpollMapper`(需补 destroy mutex)→ `~unordered_set` 释放 bucket(104B)+ hash 节点(16B × N)。**方案1+2 覆盖全部 4 层。**

### 报告3:UmqConnectorOps peer_all_socket_ids_ vector 缓冲(8B,Connector 级联更深层)

- **对象**:`std::vector<uint32_t>` 内部缓冲(`UmqConnectorOps::peer_all_socket_ids_`)。
- **分配点**:`UmqConnectorOps::ConnectNegotiate`(`umq_socket_connector.cpp:463`):
  ```cpp
  peer_all_socket_ids_.reserve(rsp.socket_id_count);   // :463 ← reserve → _M_allocate → new[]
  ```
- **大小**:8B = 1 × `sizeof(uint32_t)`(`reserve(1)` 分配 1 个槽)。
- **Connector 级联层级**:

| 层级 | 对象 | 大小 | 状态 |
|------|------|------|------|
| 1 | `Connector` 本体 | 16B(direct) | 已文档 |
| 2 | `UmqConnectorOps` 本体 | 480B(indirect) | 已文档 |
| 3a | `back_routes_` vector 缓冲 | 160B(indirect) | 已文档 |
| 3b | `non_aff_route_list_` vector 缓冲 | 80B(indirect) | 已文档 |
| 3c | **`peer_all_socket_ids_` vector 缓冲** | **8B(indirect)** | **本报告** |
| 更深 | `umq_conn_info_.peer_ip` string | ?(应有报告) | anticipated |

`delete connector_` → `~Connector` → `~Ref<ConnectorOps>` → `delete UmqConnectorOps` → `~UmqConnectorOps`(默认析构)→ `~vector<uint32_t>`(peer_all_socket_ids_)释放 8B + `~vector<umq_route>`(back_routes_/non_aff_route_list_) + `~string`(peer_ip)。**一处 `delete connector_` 覆盖全部层级。**

## 3. 全量 SUMMARY 解读

```
SUMMARY: AddressSanitizer: 286535 byte(s) leaked in 4872 allocation(s).
```

- **286535 字节** ≈ 280KB(全部 ASan 报告的泄露总量)
- **4872 个分配**(全部泄露对象数)
- 绝大多数来自 brpc bvar/local 抛弃家族(bthread 退出不干净的小 string/vector,每个几十字节但数量多)+ ubs-comm 析构链家族(rxQueue 128KB + 各子对象)+ EpollMapper 全局 map

**ubs-comm 侧可修的泄露量**:rxQueue 级联(48B+320B+128KB+40B+2056B ≈ 131KB)+ 析构链其他子对象(288B+408B+72B+96B+48B+16B+480B+160B+80B+8B ≈ 1.7KB)+ AsyncEventPoll(40B × 2)+ EpollMapper(72B × 2 + 104B × 2 + 48B × 2 + 16B × 2 ≈ 0.5KB)≈ **133KB**。

**brpc 侧泄露量**:bvar/local 抛弃(数百个 string/vector,每个几十字节)+ RespClosure/done(24B × 80 + 32B × 7 ≈ 2.1KB)+ RpcMeta(280B + 224B ≈ 0.5KB)≈ **剩余 ~145KB**(4872 - ubs-comm 对象数 = brpc 侧对象数,主要是 bvar string)。

## 4. 修复归属

| 报告 | 修复方案 | 修复文档 |
|------|---------|---------|
| 1(FastHeap 40B) | `~UmqSocket` `delete rxQueue`(随 rxQueue 级联消) | `UBSOCKET-UMQ-RXQUEUE-LEAK-ANALYSIS.ch.md` §8 |
| 2a/2b(EpollMapper hash node 16B × 2) | `ubsocket_uninit` 清全局 map `delete` EpollMapper + `~EpollMapper` 补 destroy | `UBSOCKET-EPOLLMAPPER-LEAK-ANALYSIS.ch.md` §7 |
| 3(peer_all_socket_ids_ 8B) | `~SocketBase` `delete connector_`(随 Connector 级联消) | `UBSOCKET-UMQ-CONNECTOR-LEAK-ANALYSIS.ch.md` §7 |

四条均随各自家族的伞形修复一并消除,无需额外代码。

## 5. 关键代码位置索引

| 位置 | 作用 | 泄露相关性 |
|------|------|-----------|
| `umq_buffer_receive_queue.cpp:47/52` | `new FastHeap` | 报告1 FastHeap 40B 分配点 |
| `ubsocket_fast_heap.h` | `FastHeap` 类(40B) | 报告1 对象 |
| `ubsocket_event_epoll.h:83` | `EpollMapper::Add` `unordered_set::insert` | 报告2a/2b hash node 16B 分配点 |
| `umq_socket_connector.cpp:463` | `peer_all_socket_ids_.reserve` | 报告3 vector 8B 分配点 |
| `umq_socket_connector.h:82` | `std::vector<uint32_t> peer_all_socket_ids_` | 报告3 vector 归属 |

## 6. 与其他泄露的关系

| 报告 | 家族 | 对应既有文档 | 修复 |
|------|------|-------------|------|
| 1(40B) | rxQueue 级联 Level 2b | `UBSOCKET-UMQ-RXQUEUE-FASTHEAP-LEAK-ANALYSIS.ch.md` | 随 `delete rxQueue` |
| 2a/2b(16B × 2) | EpollMapper 级联 Level 2c | `UBSOCKET-EPOLLMAPPER-LEAK-ANALYSIS.ch.md` | 随 `ubsocket_uninit` 清 map |
| 3(8B) | Connector 级联更深层 | `UBSOCKET-UMQ-CONNECTOR-OPS-VECTOR-LEAK-ANALYSIS.ch.md` | 随 `delete connector_` |

四条均是多份既有文档 anticipated 的更深层级联报告,随各自家族伞形修复一并消除。ubs-comm 无新增修复点。

## 参考

- `UBSOCKET-UMQ-RXQUEUE-FASTHEAP-LEAK-ANALYSIS.ch.md` — FastHeap InitHeap 2056B(§1 级联,本报告是其对象本体 40B)
- `UBSOCKET-UMQ-RXQUEUE-LEAK-ANALYSIS.ch.md` — rxQueue 48B(§5 级联层级表,§8 修复)
- `UBSOCKET-EPOLLMAPPER-LEAK-ANALYSIS.ch.md` — EpollMapper bucket 104B(§1 级联,本报告是其 hash node)
- `UBSOCKET-EPOLLMAPPER-OBJECT-LEAK-ANALYSIS.ch.md` — EpollMapper 对象 72B(§5 级联层级)
- `UBSOCKET-UMQ-CONNECTOR-OPS-VECTOR-LEAK-ANALYSIS.ch.md` — back_routes_ 160B(§2 表 anticipated peer_all_socket_ids_)
- `UBSOCKET-UMQ-CONNECTOR-LEAK-ANALYSIS.ch.md` — Connector 16B(§7 修复)
- 其他 ubs-comm/ub_test/brpc 泄露文档
- 源码:`src/ubsocket/csrc/core/umq/umq_buffer_receive_queue.cpp:47-52`、`src/ubsocket/csrc/common/ubsocket_fast_heap.h`、`src/ubsocket/csrc/core/ubsocket_event_epoll.h:83`、`src/ubsocket/csrc/core/umq/umq_socket_connector.cpp:463`、`umq_socket_connector.h:82`
