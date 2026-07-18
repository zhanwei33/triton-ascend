# UBSocket SPSCRingQueue 对象级联泄露分析(rxQueue 泄露之子二/层级2)

> **现象**:ASan 报告(**Indirect leak**——间接泄露,挂在父对象 `UmqBufferReceiveQueue` 之下)
> ```
> Indirect leak of 320 byte(s) in 1 object(s) allocated from:
>     #1 UmqBufferReceiveQueue::UmqBufferReceiveQueue() umq_buffer_receive_queue.cpp:36
>     #2 UmqSocket::CreateLocalUmq umq_socket.cpp:144
>     ... → brpc::Socket::Connect socket.cpp:1343 → PerformanceTest::Init
> ```
>
> 本文确认其为 `UBSOCKET-UMQ-RXQUEUE-LEAK-ANALYSIS.ch.md`(UmqBufferReceiveQueue 48B direct)§5 表 anticipated 的**层级2 级联子对象**(`SPSCRingQueue` 对象本体)。前述 `UBSOCKET-UMQ-RXQUEUE-SPSC-BUFFER-LEAK-ANALYSIS.ch.md`(128KB)是层级3(vector 缓冲),本报告是层级2(SPSCRingQueue 对象本身)。

## 1. rxQueue 级联层级(已观测 4/4)

`UBSOCKET-UMQ-RXQUEUE-LEAK-ANALYSIS.ch.md` §5 列出 rxQueue 泄露的 4 层级联,至此全部观测到:

| 层级 | 对象 | 分配点 | 大小 | ASan 报告 |
|------|------|--------|------|-----------|
| 1 | `UmqBufferReceiveQueue` 本体 | `umq_socket.cpp:144` `new UmqBufferReceiveQueue` | 48B | 已文档(direct) |
| **2** | **`SPSCRingQueue<umq_buf_t*>` 对象本体** | **`umq_buffer_receive_queue.cpp:33` `new SPSCRingQueue`** | **320B** | **本报告(indirect)** |
| 3 | `SPSCRingQueue::buffer_`(vector 缓冲) | `ubsocket_spsc_ring_queue.h:24` `vector(count)` ctor | 131072B(128KB) | 已文档(indirect) |
| 4 | `FastHeap::InitHeap` 缓冲(o3) | `ubsocket_fast_heap.h:156` `InitHeap` | 2056B | 已文档(indirect) |

**Indirect leak** 标记说明 ASan 判定本 320B 被层级1 `UmqBufferReceiveQueue` 48B direct 间接持有(经 `receive_queue` 指针),与级联分析一致。

## 2. 泄露对象

- **对象**:`ock::ubs::SPSCRingQueue<umq_buf_t*>` 对象本体(`ubsocket_spsc_ring_queue.h:22`)。
- **分配点**:`UmqBufferReceiveQueue` ctor(`umq_buffer_receive_queue.cpp:33`):
  ```cpp
  receive_queue = new (std::nothrow) SPSCRingQueue<umq_buf_t *>(queue_depth);
  ```
  ASan frame #1 报 `:36`(版本漂移,本仓库当前 :33)。
- **大小**:320 字节。`sizeof(SPSCRingQueue<umq_buf_t*>)` 成员(`ubsocket_spsc_ring_queue.h:126-132`):
  ```cpp
  std::vector<T> buffer_;                    // 24B(libstdc++ vector = 3 指针)
  alignas(64) uint64_t write_index_{0};       // alignas(64) → 64 对齐
  alignas(64) uint64_t read_index_{0};        // 64 对齐
  alignas(64) uint64_t commit_write_{0};       // 64 对齐
  alignas(64) uint64_t commit_read_{0};       // 64 对齐
  const uint64_t capacity_;                    // 8B
  const uint64_t mask_;                        // 8B
  ```
  布局:vector(24B,offset 0-23)→ write_index_ `alignas(64)`(offset 64,8B)→ read_index_(offset 128)→ commit_write_(offset 192)→ commit_read_(offset 256)→ capacity_(offset 264)→ mask_(offset 272)。内容 280B,但 `alignof(SPSCRingQueue)=64`(因 4 个 `alignas(64)` 成员)→ `sizeof` 取整到 64 的倍数 = **320B**。与报告吻合。
- **归属**:`UmqBufferReceiveQueue::receive_queue`(`umq_buffer_receive_queue.h:71` `SPSCRingQueue<umq_buf_t*>* receive_queue` 裸指针),经 `UmqBufferReceiveQueue` 归属 `UmqSocket::rxQueue`。

## 3. 与层级3(vector 缓冲 128KB)的区别

| 维度 | 层级2(本报告) | 层级3(已文档) |
|------|--------------|---------------|
| 对象 | `SPSCRingQueue` 对象本体 | `SPSCRingQueue::buffer_` vector 内部缓冲 |
| 分配点 | `umq_buffer_receive_queue.cpp:33` `new SPSCRingQueue` | `ubsocket_spsc_ring_queue.h:24` `vector(count)` ctor(在 SPSCRingQueue ctor 内) |
| 大小 | 320B(对象 shell) | 131072B(128KB,capacity×8) |
| ASan 归属 | indirect(挂 UmqBufferReceiveQueue) | indirect(挂 UmqBufferReceiveQueue,经 SPSCRingQueue→buffer_) |

两者是 SPSCRingQueue 的**对象本体 vs 内部 vector 缓冲**,均为层级1 `UmqBufferReceiveQueue` 的级联子对象,随 `delete rxQueue` 一并消除。

## 4. 为何泄露(同 rxQueue 家族,级联)

`UBSOCKET-UMQ-RXQUEUE-LEAK-ANALYSIS.ch.md` §4 已确认:`UmqSocket::rxQueue`(`UmqBufferReceiveQueue*`)全仓 **0 处 `delete rxQueue`**。

级联效应:
- `rxQueue` 不 delete → `~UmqBufferReceiveQueue`(`umq_buffer_receive_queue.cpp:56-60`)从不执行
- `~UmqBufferReceiveQueue` 本会 `ClearAllocations()`(`:118-132`):
  ```cpp
  void UmqBufferReceiveQueue::ClearAllocations() {
      FlushReceiveQueueInternal();
      if (use_o3_) { FlushOooQueueInternal(); }
      if (receive_queue) { delete receive_queue; receive_queue = nullptr; }   // ← 层级2(SPSCRingQueue 本体) + 层级3(vector)
      if (out_of_order_queue) { delete out_of_order_queue; out_of_order_queue = nullptr; }  // 层级4(FastHeap)
  }
  ```
  `delete receive_queue` → `~SPSCRingQueue` → `~vector<umq_buf*>` → 释放 `buffer_`(128KB 层级3)+ SPSCRingQueue 对象本体(320B 层级2)。
- 但 `~UmqBufferReceiveQueue` 从不调 → `ClearAllocations` 不执行 → 层级2(本 320B)/层级3(128KB)/层级4(2056B)全部不释放。

**根因与 rxQueue/SPSC buffer/FastHeap 泄露完全相同**:rxQueue 无 delete → 全部级联子对象失主。本 320B 是层级2 indirect。

## 5. 为何 320B / 1 个

- `sizeof(SPSCRingQueue<umq_buf_t*>)` = 320B(280B 内容按 `alignof=64` 取整)。
- 1 个 = 单 `UmqBufferReceiveQueue` 的 `receive_queue`。本次仅 1 个 UmqSocket 被销毁 → 1 个 320B SPSCRingQueue 对象泄露。
- 与层级3(128KB vector 缓冲)是**同一 SPSCRingQueue 的对象本体 vs 内部缓冲**——两者数量一致(均 1 个)。

## 6. 触发条件

与 rxQueue 家族完全一致:任一 UmqSocket 被销毁即泄露 rxQueue + 全部级联(含本 320B SPSCRingQueue 对象 + 128KB vector 缓冲 + 2056B FastHeap)。与 UB 配置无关。

## 7. 修复方案

**与 `UBSOCKET-UMQ-RXQUEUE-LEAK-ANALYSIS.ch.md` §8、`UBSOCKET-UMQ-RXQUEUE-SPSC-BUFFER-LEAK-ANALYSIS.ch.md` §7、`UBSOCKET-UMQ-RXQUEUE-FASTHEAP-LEAK-ANALYSIS.ch.md` §7 完全共用**——`~UmqSocket`/`UnInitialize` 补 `delete rxQueue`:

```cpp
void UmqSocket::UnInitialize() noexcept {
    ...
    UnbindAndFlushRemoteUmq(this);
    DestroyLocalUmq();
    delete rxQueue;          // ← 触发级联:~UmqBufferReceiveQueue → ClearAllocations
    rxQueue = nullptr;       //   → delete receive_queue(SPSCRingQueue)→ ~SPSCRingQueue → 释放 320B 本体 + 128KB buffer_
                             //   → delete out_of_order_queue(FastHeap)→ ~FastHeap → 释放 2056B
}
```

`delete rxQueue` → `~UmqBufferReceiveQueue` → `ClearAllocations()` → `delete receive_queue` → `~SPSCRingQueue` → `~vector` 释放层级3(128KB)+ SPSCRingQueue 本体(320B 层级2)+ `delete out_of_order_queue` → 层级4(2056B)。**一处 delete 覆盖层级1/2/3/4 全部级联**。

### 与伞形析构链 PR 的关系

本修复是伞形析构链 PR 的 `rxQueue` 子项(与 SPSC buffer 128KB、FastHeap 2056B 同),`~UmqSocket` 同时处理 `delete rxQueue` + `destroy mutex_`,与 `~SocketBase` delete acceptor_/connector_、`~DataTx`/`~DataRx` delete tx_ops_/rx_ops_、`~Acceptor` destroy mutex 一并合并。

## 8. 验证

修复后 ASan 重跑:本 320B indirect(层级2)+ 128KB indirect(层级3)+ 2056B indirect(层级4)+ rxQueue 48B direct(层级1)**同时消失**。可用小 `thread_num` 短测验证。

## 9. 关键代码位置索引

| 位置 | 作用 | 泄露相关性 |
|------|------|-----------|
| `umq_buffer_receive_queue.cpp:33`(用户 :36) | `new SPSCRingQueue<umq_buf_t*>(queue_depth)` | **本报告 320B 分配点(层级2)** |
| `ubsocket_spsc_ring_queue.h:22-132` | `SPSCRingQueue` 类(320B,4×alignas(64)+vector+2const) | 泄露对象 |
| `umq_buffer_receive_queue.h:71` | `SPSCRingQueue<umq_buf_t*>* receive_queue` 裸指针 | 无 RAII |
| `umq_buffer_receive_queue.cpp:56-60,118-132` | `~UmqBufferReceiveQueue`+`ClearAllocations`(清内部,从不被调) | 级联释放入口(失效) |
| `umq_socket.cpp:144` | `new UmqBufferReceiveQueue` | 层级1 分配 |
| `umq_socket.h:214` | `UmqBufferReceiveQueue *rxQueue` 裸指针 | 根因(无 delete) |

## 10. 与其他泄露的关系

| 维度 | 本泄露(SPSCRingQueue 320B 层级2) | SPSC buffer 128KB 层级3 | FastHeap 2056B 层级4 | rxQueue 48B 层级1 | 析构链其他(Acceptor/Connector/DataOps/mutex) | TX Event | RX | RespClosure/done | bvar 家族 | RpcMeta | AsyncEventPoll |
|------|--------------------------------|------------------------|----------------------|------------------|---------------------------------------------|----------|----|------------------|-----------|---------|----------------|
| 归属 | ubsocket core(umq) | 同 | 同 | 同 | ubsocket core | ubsocket umq | ubsocket umq | ub_test | brpc | brpc/protobuf | ubsocket core |
| 类别 | rxQueue 级联子(SPSCRingQueue 本体) | rxQueue 级联子(vector 缓冲) | rxQueue 级联子(FastHeap 内部) | 析构不 delete rxQueue | 析构不 delete | 退出未释放 | buffer 不回流 | 闭包/drain | bthread 抛弃 | 解析逃逸 | 析构不清 map |
| 与 rxQueue 关系 | **层级2** | 层级3 | 层级4 | 自身(层级1) | 独立 | 独立 | 独立 | 独立 | 独立 | 独立 | 同类不同对象 |
| direct/indirect | indirect | indirect | indirect | direct | direct | — | — | — | — | — | — |
| ubs-comm 修复 | ✓(随 rxQueue delete 级联消) | ✓(随 rxQueue) | ✓(随 rxQueue) | ✓(伞形) | ✓(伞形) | ✓ | ✓ | ❌ | ❌ | ❌ | ✓ |

本泄露是 rxQueue 泄露的**层级2 级联子对象**(SPSCRingQueue 对象本体),与层级3(128KB vector 缓冲)、层级4(2056B FastHeap)、层级1(48B rxQueue)同根,随 `~UmqSocket` `delete rxQueue` 一并消除。至此 rxQueue 级联 4 层全部观测。属 ubs-comm 析构链伞形修复范围。

## 参考

- `UBSOCKET-UMQ-RXQUEUE-LEAK-ANALYSIS.ch.md` — rxQueue 48B direct 泄露(§5 表已 anticipated 层级2,§8 修复方案共用)
- `UBSOCKET-UMQ-RXQUEUE-SPSC-BUFFER-LEAK-ANALYSIS.ch.md` — SPSC buffer 128KB indirect(层级3,同 SPSCRingQueue 的内部 vector)
- `UBSOCKET-UMQ-RXQUEUE-FASTHEAP-LEAK-ANALYSIS.ch.md` — FastHeap 2056B indirect(层级4)
- `UBSOCKET-UMQ-ACCEPTOR-*-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-CONNECTOR-*-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-DATAOPS-*-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-SOCKET-MUTEX-LEAK-ANALYSIS.ch.md` — 析构链家族其他子对象/级联
- `UBSOCKET-ASYNCEVENTPOLL-EPOLLEVENT-LEAK-ANALYSIS.ch.md` — AsyncEventPoll 析构清理(同类不同对象)
- `UBSOCKET-UMQ-RX-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-TX-EVENT-LEAK-ANALYSIS.ch.md` — RX/TX Event 泄露(不同类)
- 其他 ub_test/brpc 泄露文档
- 源码:`src/ubsocket/csrc/core/umq/umq_buffer_receive_queue.cpp:30-60,118-132`、`umq_buffer_receive_queue.h:71`、`src/ubsocket/csrc/common/ubsocket_spsc_ring_queue.h:22-132`、`umq_socket.cpp:35-50,140-148`、`umq_socket.h:51-54,214`
