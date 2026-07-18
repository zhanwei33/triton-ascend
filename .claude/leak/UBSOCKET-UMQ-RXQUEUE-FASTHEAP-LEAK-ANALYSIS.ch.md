# UBSocket FastHeap(o3 队列)级联泄露分析(rxQueue 泄露之四)

> **现象**:ASan 报告(**Indirect leak**——间接泄露,挂在父对象 `UmqBufferReceiveQueue` 之下)
> ```
> Indirect leak of 2056 byte(s) in 1 object(s) allocated from:
>     #1 ock::ubs::FastHeap<umq_buf*, O3QueueComparator>::InitHeap ubsocket_fast_heap.h:156
>     #2 FastHeap::FastHeap(capacity, ...) ubsocket_fast_heap.h:35
>     #3 UmqBufferReceiveQueue::UmqBufferReceiveQueue() umq_buffer_receive_queue.cpp:52
>     #4 UmqSocket::CreateLocalUmq umq_socket.cpp:144
>     #5 UmqConnectorOps::DoUbConnect umq_socket_connector.cpp:570
>     ... → brpc::Socket::Connect socket.cpp:1343 → PerformanceTest::Init
> ```
>
> 本文确认其为 `UBSOCKET-UMQ-RXQUEUE-LEAK-ANALYSIS.ch.md`(UmqBufferReceiveQueue 48B)§5 anticipated 的**层级4 级联子对象**(`out_of_order_queue` FastHeap 内部 `InitHeap` 缓冲),并印证环境使用 **RM_CTP 模式**(`use_o3_=true`)。

## 1. 级联关系(rxQueue 泄露的第四层)

`UBSOCKET-UMQ-RXQUEUE-LEAK-ANALYSIS.ch.md` §5 已列出 rxQueue 泄露的级联层级,本报告即层级4,至此 4 层全部观测到:

| 层级 | 对象 | 分配点 | 大小 | ASan 报告 |
|------|------|--------|------|-----------|
| 1 | `UmqBufferReceiveQueue` 本体 | `umq_socket.cpp:144` `new UmqBufferReceiveQueue` | 48B | 已文档(direct) |
| 2 | `SPSCRingQueue<umq_buf_t*>` 本体 | `umq_buffer_receive_queue.cpp:34` `new SPSCRingQueue` | ~136B | 应有独立报告(本体) |
| 3 | `SPSCRingQueue::buffer_`(vector 缓冲) | `ubsocket_spsc_ring_queue.h:24` `vector(count)` ctor | 131072B(128KB) | 已文档(indirect) |
| 4 | **`FastHeap::InitHeap` 缓冲(o3 队列内部)** | **`ubsocket_fast_heap.h:156` `InitHeap`** | **2056B** | **本报告(indirect)** |

**Indirect leak** 标记说明 ASan 判定本 2056B 被层级1 的 `UmqBufferReceiveQueue` 48B 间接持有(经 `out_of_order_queue` 指针),与级联分析一致。

## 2. 泄露对象

- **对象**:`FastHeap<umq_buf_t*, O3QueueComparator>` 的内部堆数组(`InitHeap` `malloc` 出)。
- **分配点**:`FastHeap::FastHeap(capacity, ...)`(`ubsocket_fast_heap.h:35`)→ `InitHeap(capacity)`(`:156`):
  ```cpp
  // ubsocket_fast_heap.h:35
  FastHeap(unsigned long capacity, unsigned long ...) {
      ...
      InitHeap(capacity);   // :156 → malloc 内部堆数组
  }
  ```
- **大小**:2056 字节。FastHeap 内部堆数组(容量 × 元素大小 + 头部),具体容量来自 `o3_queue_depth`(下文)。
- **归属**:`UmqBufferReceiveQueue::out_of_order_queue`(`umq_buffer_receive_queue.h:72` `FastHeap<umq_buf_t*, O3QueueComparator>* out_of_order_queue`)。

## 3. 代码细节(印证 RM_CTP 模式)

`UmqBufferReceiveQueue` ctor(`umq_buffer_receive_queue.cpp:30-54`):

```cpp
UmqBufferReceiveQueue::UmqBufferReceiveQueue() {
    uint64_t queue_depth = GlobalSetting::UBS_RX_DEPTH;
    queue_depth = (queue_depth <= 1) ? 1 : 1ULL << (64 - __builtin_clzll(queue_depth - 1));
    receive_queue = new SPSCRingQueue<umq_buf_t *>(queue_depth);            // 层级2/3

    use_o3_ = (UmqSetting::UMQ_UB_TRANS_MODE == RM_CTP);                    // ← 印证 RM_CTP
    if (use_o3_) {
        uint32_t o3_queue_depth = GlobalSetting::UBS_RX_DEPTH;
        if (o3_queue_depth > queue_depth) { o3_queue_depth = queue_depth; }
        out_of_order_queue = new FastHeap<umq_buf_t*, O3QueueComparator>(   // 层级4 触发
            o3_queue_depth, o3_queue_depth);                                 // :47-48 → FastHeap ctor :35 → InitHeap :156
    }
}
```

- `use_o3_ = (UMQ_UB_TRANS_MODE == RM_CTP)`——本报告存在 FastHeap 分配,说明环境 `UBSOCKET_UB_TRANS_MODE` 为 `RM_CTP`(默认),`use_o3_=true`,启用 CTP 保序乱序重排。
- `o3_queue_depth = UBS_RX_DEPTH`(若超 queue_depth 则截断)→ `FastHeap(o3_queue_depth, o3_queue_depth)` → `InitHeap` 分配内部堆数组(2056B)。

## 4. 为何泄露(同 rxQueue 家族,级联)

`UBSOCKET-UMQ-RXQUEUE-LEAK-ANALYSIS.ch.md` §4 已确认:`UmqSocket::rxQueue` 全仓 **0 处 `delete rxQueue`**。

级联效应:
- `rxQueue` 不 delete → `~UmqBufferReceiveQueue` 从不执行
- `~UmqBufferReceiveQueue`(`umq_buffer_receive_queue.cpp:56-60`)本会 `ClearAllocations()`(`:118-132`):
  ```cpp
  void UmqBufferReceiveQueue::ClearAllocations() {
      FlushReceiveQueueInternal();
      if (use_o3_) { FlushOooQueueInternal(); }                          // free ooo 队列内 umq_buf
      if (receive_queue) { delete receive_queue; receive_queue = nullptr; }   // 层级2/3
      if (out_of_order_queue) { delete out_of_order_queue; out_of_order_queue = nullptr; }  // 层级4 ← 本报告
  }
  ```
  `delete out_of_order_queue` → `~FastHeap` → 释放 `InitHeap` 的 2056B 内部数组。
- 但 `~UmqBufferReceiveQueue` 从不调 → `ClearAllocations` 不执行 → 层级1/2/3/4 全部不释放。

**根因与 rxQueue/SPSC buffer 泄露完全相同**:rxQueue 无 delete → 全部级联子对象失主。本 2056B 是层级4 indirect。

## 5. 为何 2056B / 1 个

- `FastHeap::InitHeap(o3_queue_depth)` `malloc` 内部堆数组,大小 = `o3_queue_depth × sizeof(entry) + 头部`。2056B 对应 `UBS_RX_DEPTH` 取整后的 o3_queue_depth(具体公式取决于 FastHeap 内部布局,与 `UBS_RX_DEPTH` 取整值一致——同 SPSC buffer 的 16384 容量来源)。
- 1 个 = 单连接的 `UmqBufferReceiveQueue` 的 FastHeap。本次仅 1 个 UmqSocket 被销毁 → 1 个 2056B 泄露。
- 仅 RM_CTP 模式存在(`use_o3_=true`);非 CTP 模式无 FastHeap,无本报告。

## 6. 触发条件

- `UBSOCKET_UB_TRANS_MODE = RM_CTP`(默认,启用 o3 保序重排)→ `use_o3_=true` → FastHeap 创建
- 任一 UmqSocket 被销毁(socket close 或 `ubsocket_uninit` 的 `ReleaseAll`)即泄露 rxQueue + 全部级联(含本 FastHeap 2056B)
- 与 UB 配置无关(仅依赖 RM_CTP 模式)

## 7. 修复方案

**与 `UBSOCKET-UMQ-RXQUEUE-LEAK-ANALYSIS.ch.md` §8、`UBSOCKET-UMQ-RXQUEUE-SPSC-BUFFER-LEAK-ANALYSIS.ch.md` §7 完全共用**——`~UmqSocket`/`UnInitialize` 补 `delete rxQueue`:

```cpp
void UmqSocket::UnInitialize() noexcept {
    ...
    UnbindAndFlushRemoteUmq(this);
    DestroyLocalUmq();
    delete rxQueue;          // ← 触发级联释放(层级1→2/3/4)
    rxQueue = nullptr;
}
```

`delete rxQueue` → `~UmqBufferReceiveQueue` → `ClearAllocations()` → `delete out_of_order_queue`(FastHeap)→ `~FastHeap` → 释放 `InitHeap` 2056B。**一处 delete 覆盖层级1/2/3/4 全部级联**。

### 与伞形析构链 PR 的关系

本修复是伞形析构链 PR 的 `rxQueue` 子项(与 SPSC buffer 128KB 同),`~UmqSocket` 同时处理 `delete rxQueue` + `destroy mutex_`,与 `~SocketBase` delete acceptor_/connector_、`~DataTx`/`~DataRx` delete tx_ops_/rx_ops_、`~Acceptor` destroy mutex 一并合并。

## 8. 验证

修复后 ASan 重跑:本 2056B indirect + rxQueue 48B direct + SPSCRingQueue 本体 + SPSC buffer 128KB **同时消失**。可在非 CTP 模式(`UBSOCKET_UB_TRANS_MODE=RM_TP`)复测确认 FastHeap 报告不再出现(因 `use_o3_=false` 不创建)。

## 9. 关键代码位置索引

| 位置 | 作用 | 泄露相关性 |
|------|------|-----------|
| `ubsocket_fast_heap.h:156` | `FastHeap::InitHeap` `malloc` 内部堆数组 | **本报告 2056B 分配点** |
| `ubsocket_fast_heap.h:35` | `FastHeap` ctor 调 `InitHeap` | 调用链 |
| `umq_buffer_receive_queue.cpp:47-52` | `new FastHeap(o3_queue_depth, ...)` | 层级4 触发(仅 `use_o3_`) |
| `umq_buffer_receive_queue.cpp:40` | `use_o3_ = (UMQ_UB_TRANS_MODE == RM_CPT)` | 印证 RM_CTP 模式 |
| `umq_buffer_receive_queue.h:72` | `FastHeap<...>* out_of_order_queue` 裸指针 | 无 RAII |
| `umq_buffer_receive_queue.cpp:56-60,118-132` | `~UmqBufferReceiveQueue`+`ClearAllocations`(清内部,从不被调) | 级联释放入口(失效) |
| `umq_socket.cpp:144` | `new UmqBufferReceiveQueue` | 层级1 分配 |
| `umq_socket.h:214` | `UmqBufferReceiveQueue *rxQueue` 裸指针 | 根因(无 delete) |

## 10. 与其他泄露的关系

| 维度 | 本泄露(FastHeap 2056B) | SPSC buffer 128KB | rxQueue 48B | 析构链其他子对象 | TX Event | RX | RespClosure/done | bvar 家族 | RpcMeta | AsyncEventPoll |
|------|----------------------|-------------------|-------------|---------------|----------|----|------------------|-----------|---------|----------------|
| 归属 | ubsocket core(umq) | ubsocket core | ubsocket core | ubsocket core | ubsocket umq | ubsocket umq | ub_test | brpc | brpc/protobuf | ubsocket core |
| 类别 | rxQueue 级联子(o3 FastHeap) | rxQueue 级联子(SPSC buffer) | 析构不 delete rxQueue | 析构不 delete | 退出未释放 | buffer 不回流 | 闭包/drain | bthread 抛弃 | 解析逃逸 | 析构不清 map |
| 与 rxQueue 关系 | **层级4 级联子** | 层级3 级联子 | 自身(层级1) | 同析构链家族同次销毁 | 独立 | 独立 | 独立 | 独立 | 独立 | 同类不同对象 |
| direct/indirect | indirect | indirect | direct | direct | — | — | — | — | — | — |
| 模式依赖 | RM_CTP(`use_o3_=true`) | 无(总创建) | 无 | 无 | BONDING_BACKUP+RM+POOL | 无 | 无 | 无 | 无 | 无 |
| ubs-comm 修复 | ✓(随 rxQueue delete 级联消) | ✓(随 rxQueue) | ✓(伞形) | ✓(伞形) | ✓ | ✓ | ❌ | ❌ | ❌ | ✓ |

本泄露是 rxQueue 泄露的**级联子对象(层级4,仅 RM_CTP 模式)**,与 rxQueue/SPSC buffer 同根,随 `~UmqSocket` `delete rxQueue` 一并消除。至此 rxQueue 级联 4 层全部观测。属 ubs-comm 析构链伞形修复范围。

## 参考

- `UBSOCKET-UMQ-RXQUEUE-LEAK-ANALYSIS.ch.md` — rxQueue 48B direct 泄露(§5 已 anticipated 本层级4,§8 修复方案共用)
- `UBSOCKET-UMQ-RXQUEUE-SPSC-BUFFER-LEAK-ANALYSIS.ch.md` — SPSC buffer 128KB indirect(层级3,同 rxQueue 级联)
- `UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-CONNECTOR-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-DATAOPS-*-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-SOCKET-MUTEX-LEAK-ANALYSIS.ch.md` — 析构链家族其他子对象(同次销毁)
- `UBSOCKET-ASYNCEVENTPOLL-EPOLLEVENT-LEAK-ANALYSIS.ch.md` — AsyncEventPoll 析构清理(同类不同对象)
- `UBSOCKET-UMQ-RX-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-TX-EVENT-LEAK-ANALYSIS.ch.md` — RX/TX Event 泄露(不同类)
- 其他 ub_test/brpc 泄露文档
- 源码:`src/ubsocket/csrc/common/ubsocket_fast_heap.h:35,156`、`src/ubsocket/csrc/core/umq/umq_buffer_receive_queue.cpp:30-60,118-132`、`umq_buffer_receive_queue.h:72`、`umq_socket.cpp:35-50,140-148`、`umq_socket.h:51-54,214`
