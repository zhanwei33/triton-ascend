# UBSocket SPSCRingQueue vector 缓冲级联泄露分析(rxQueue 泄露之子)

> **现象**:ASan 报告(**Indirect leak**——间接泄露,挂在父对象 `UmqBufferReceiveQueue` 之下)
> ```
> Indirect leak of 131072 byte(s) in 1 object(s) allocated from:
>     #1 std::__new_allocator<umq_buf*>::allocate new_allocator.h:151
>     #2 allocator_traits::allocate :515 → _Vector_base::_M_allocate :383
>     #3 _Vector_base::_M_create_storage :401
>     #4 _Vector_base::_Vector_base(count, alloc) :337
>     #5 vector::vector(count, alloc) stl_vector.h:560
>     #6 SPSCRingQueue<umq_buf*>::SPSCRingQueue(capacity) ubsocket_spsc_ring_queue.h:24
>     #7 UmqBufferReceiveQueue::UmqBufferReceiveQueue() umq_buffer_receive_queue.cpp:36
>     #8 UmqSocket::CreateLocalUmq umq_socket.cpp:144
>     #9 UmqConnectorOps::DoUbConnect umq_socket_connector.cpp:570
>     ... → brpc::Socket::Connect socket.cpp:1343 → PerformanceTest::Init
> ```
>
> 本文确认其为 `UBSOCKET-UMQ-RXQUEUE-LEAK-ANALYSIS.ch.md`(UmqBufferReceiveQueue 48B)§5 anticipated 的**层级3 级联子对象**(`SPSCRingQueue::buffer_` vector 缓冲)。

## 1. 级联关系(rxQueue 泄露的第三层)

`UBSOCKET-UMQ-RXQUEUE-LEAK-ANALYSIS.ch.md` §5 已列出 rxQueue 泄露的级联层级,本报告即层级3:

| 层级 | 对象 | 分配点 | 大小 | ASan 报告 |
|------|------|--------|------|-----------|
| 1 | `UmqBufferReceiveQueue` 本体 | `umq_socket.cpp:144` `new UmqBufferReceiveQueue` | 48B | 已文档(direct) |
| 2 | `SPSCRingQueue<umq_buf_t*>` 本体 | `umq_buffer_receive_queue.cpp:34` `new SPSCRingQueue` | ~136B | 应有独立报告(本体) |
| 3 | **`SPSCRingQueue::buffer_`(vector 缓冲)** | **`ubsocket_spsc_ring_queue.h:24` `vector(count)` ctor** | **131072B(128KB)** | **本报告(indirect)** |
| 4 | `FastHeap`(o3 模式) | `umq_buffer_receive_queue.cpp:47` `new FastHeap` | ? | RM_CTP 模式应有报告 |

**Indirect leak** 标记说明 ASan 判定本 128KB 缓冲被某个 direct-leaked 对象(层级1 的 `UmqBufferReceiveQueue` 48B)间接持有,与级联分析一致。

## 2. 泄露对象

- **对象**:`std::vector<umq_buf*>` 的内部堆缓冲(`SPSCRingQueue<umq_buf_t*>::buffer_` 成员)。
- **分配点**:`SPSCRingQueue` 构造函数(`ubsocket_spsc_ring_queue.h:24`):
  ```cpp
  explicit SPSCRingQueue(uint64_t capacity) : buffer_(capacity), capacity_(capacity), mask_(capacity - 1UL) {
      ...
  }
  ```
  `buffer_(capacity)` 调 `std::vector<umq_buf*>(count, alloc)` ctor → `_M_create_storage` → `_M_allocate` → `new[]` 分配 `count × sizeof(umq_buf*)`。
- **大小**:131072 字节 = 16384 × 8(`sizeof(umq_buf*)==8` on 64-bit)。
- **capacity=16384 的来源**:`UmqBufferReceiveQueue` ctor(`umq_buffer_receive_queue.cpp:30-34`):
  ```cpp
  uint64_t queue_depth = GlobalSetting::UBS_RX_DEPTH;
  queue_depth = (queue_depth <= 1) ? 1 : 1ULL << (64 - __builtin_clzll(queue_depth - 1));  // 向上取整到 2 的幂
  receive_queue = new SPSCRingQueue<umq_buf_t *>(queue_depth);
  ```
  本环境 `UBS_RX_DEPTH` 落在 (8193, 16384] 区间 → 取整 16384 → ×8 = 131072B。

## 3. 调用栈解读

```
ub_test client PerformanceTest::Init → stub.Test → Channel::CallMethod → IssueRPC
  → brpc Socket::Write → ... → Connect (socket.cpp:1343)
    → ubsocket_wrapper_connect → ubsocket_connect → Connector::Connect
      → CreateSocketResources (umq_socket_connector.cpp:228)
        → DoUbConnect (:570): CreateLocalUmq
          → UmqSocket::CreateLocalUmq (umq_socket.cpp:144): new UmqBufferReceiveQueue
            → UmqBufferReceiveQueue ctor (umq_buffer_receive_queue.cpp:36): new SPSCRingQueue(queue_depth)
              → SPSCRingQueue ctor (ubsocket_spsc_ring_queue.h:24): vector buffer_(16384)  ← 泄露分配(128KB)
```

client 建链 `DoUbConnect` → `CreateLocalUmq` 创建 `UmqBufferReceiveQueue`(层级1,48B),其 ctor 内 `new SPSCRingQueue`(层级2)→ SPSCRingQueue ctor 内 `vector buffer_(16384)` 分配 128KB(层级3,本报告)。

## 4. 为何泄露(同 rxQueue 家族)

`UBSOCKET-UMQ-RXQUEUE-LEAK-ANALYSIS.ch.md` §4 已确认:`UmqSocket::rxQueue`(`UmqBufferReceiveQueue*`)全仓 **0 处 `delete rxQueue`**,`~UmqSocket`/`UnInitialize`/`~SocketBase`/`FlushRxQueue` 均不释放。

级联效应:
- `rxQueue` 不 delete → `~UmqBufferReceiveQueue` 从不执行
- `~UmqBufferReceiveQueue`(`umq_buffer_receive_queue.cpp:56-60`)本会 `ClearAllocations()`(`:118-132`)→ `delete receive_queue`(SPSCRingQueue)→ `~SPSCRingQueue` → `~vector<umq_buf*>` → 释放 `buffer_`(本 128KB)
- 但 `~UmqBufferReceiveQueue` 从不调 → 整条级联链不执行 → 层级1(48B)/层级2(SPSCRingQueue 本体)/层级3(本 128KB vector 缓冲)/层级4(FastHeap)全部泄露

**根因与 rxQueue 泄露完全相同**:rxQueue 无 delete → 全部级联子对象失主。ASan 标层级1 为 direct leak,层级2/3/4 为 indirect leak(被层级1 间接持有)。

## 5. 为何 131072B / 1 个

- `UBS_RX_DEPTH` 取整 16384 → `vector buffer_(16384)` → `16384 × 8 = 131072B`。
- 1 个 = 单个 `UmqBufferReceiveQueue`(单连接)的 SPSCRingQueue 缓冲。本次仅 1 个 UmqSocket 被销毁(同 rxQueue/Acceptor 家族的"1 个")→ 1 个 128KB 缓冲泄露。
- 这是 ubs-comm 侧**单笔最大的泄露对象**(128KB),远超前述析构链家族其他子对象(288B/96B/72B/48B 等)。但因 `UBS_RX_DEPTH` 固定,每次仅泄露固定 128KB(不随时间增长),与 RX 运行时 5GB 泄露(持续增长)性质不同。

## 6. 触发条件

与 rxQueue 泄露完全一致:任一 UmqSocket 被销毁(socket close 或 `ubsocket_uninit` 的 `ReleaseAll`)即泄露 rxQueue + 其全部级联子对象(含本 128KB SPSCRingQueue 缓冲)。与 UB 配置无关。

## 7. 修复方案

**与 `UBSOCKET-UMQ-RXQUEUE-LEAK-ANALYSIS.ch.md` §8 完全共用**——`~UmqSocket` 或 `UnInitialize` 补 `delete rxQueue`:

```cpp
// umq_socket.cpp UnInitialize 末尾 或 ~UmqSocket
void UmqSocket::UnInitialize() noexcept {
    ...
    UnbindAndFlushRemoteUmq(this);
    DestroyLocalUmq();
    delete rxQueue;          // ← 新增:触发级联释放
    rxQueue = nullptr;
}
```

`delete rxQueue` → `~UmqBufferReceiveQueue` → `ClearAllocations()` → `delete receive_queue`(SPSCRingQueue)→ `~SPSCRingQueue` → `~vector` → 释放 `buffer_`(本 128KB)。**一处 delete 即覆盖层级1/2/3/4 全部级联泄露**。

### 与伞形析构链 PR 的关系

本修复是伞形析构链 PR(`UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md` 等)的 `rxQueue` 子项,与 `~SocketBase` delete acceptor_/connector_、`~DataTx`/`~DataRx` delete tx_ops_/rx_ops_、`~UmqSocket` destroy mutex_ 一并合并。`~UmqSocket` 同时处理 `delete rxQueue` + `destroy mutex_`。

## 8. 验证

修复后 ASan 重跑:本 128KB indirect leak + rxQueue 48B direct leak + SPSCRingQueue 本体 + FastHeap(若 o3)**同时消失**。可用小 `thread_num` 短测 + `UBS_RX_DEPTH` 调小(如 8 → vector buffer_ 8B)验证容量按比例缩减。

## 9. 关键代码位置索引

| 位置 | 作用 | 泄露相关性 |
|------|------|-----------|
| `ubsocket_spsc_ring_queue.h:24` | `SPSCRingQueue` ctor `buffer_(capacity)` | **本报告 128KB 分配点** |
| `ubsocket_spsc_ring_queue.h:126` | `std::vector<T> buffer_` 成员 | vector 缓冲归属 |
| `umq_buffer_receive_queue.cpp:30-34` | `UmqBufferReceiveQueue` ctor 算 queue_depth + `new SPSCRingQueue` | 层级1→2 触发 |
| `umq_buffer_receive_queue.cpp:36` | `new SPSCRingQueue(queue_depth)` 调用 | frame #7 |
| `umq_buffer_receive_queue.cpp:56-60,118-132` | `~UmqBufferReceiveQueue` + `ClearAllocations`(清内部,从不被调) | 级联释放入口(失效) |
| `umq_socket.cpp:144` | `new UmqBufferReceiveQueue` | 层级1 分配(rxQueue 文档) |
| `umq_socket.h:214` | `UmqBufferReceiveQueue *rxQueue` 裸指针 | 无 RAII(根因) |
| `umq_socket.h:51-54` / `umq_socket.cpp:35-50` | `~UmqSocket`/`UnInitialize` 不 delete rxQueue | 根因 |

## 10. 与其他泄露的关系

| 维度 | 本泄露(SPSCRingQueue buffer 128KB) | rxQueue 48B | Acceptor 288B / Connector 16B / UmqTxOps 72B / UmqRxOps 96B / mutex 48B | TX Event | RX | RespClosure/done | bvar 家族 | RpcMeta | AsyncEventPoll |
|------|------------------------------------|-------------|--------------------------------------------------------|----------|----|------------------|-----------|---------|----------------|
| 归属 | ubsocket core(umq) | ubsocket core | ubsocket core | ubsocket umq | ubsocket umq | ub_test | brpc | brpc/protobuf | ubsocket core |
| 类别 | rxQueue 级联子(vector 缓冲) | 析构不 delete rxQueue | 析构不 delete | 退出未释放 | buffer 不回流 | 闉包/drain | bthread 抛弃 | 解析逃逸 | 析构不清 map |
| 与 rxQueue 关系 | **层级3 级联子** | 自身(层级1) | 同析构链家族同次销毁 | 独立 | 独立 | 独立 | 独立 | 独立 | 同类不同对象 |
| direct/indirect | indirect(挂 rxQueue 之下) | direct | direct | — | — | — | — | — | — |
| ubs-comm 修复 | ✓(随 rxQueue delete 级联消) | ✓(伞形) | ✓(伞形) | ✓ | ✓ | ❌ | ❌ | ❌ | ✓ |

本泄露是 rxQueue 泄露的**级联子对象(层级3)**,与 rxQueue 同根,随 `~UmqSocket` `delete rxQueue` 一并消除。属 ubs-comm 析构链伞形修复范围。

## 参考

- `UBSOCKET-UMQ-RXQUEUE-LEAK-ANALYSIS.ch.md` — rxQueue 48B direct 泄露(§5 已 anticipated 本层级3 级联报告,§8 修复方案共用)
- `UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-CONNECTOR-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-DATAOPS-*-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-SOCKET-MUTEX-LEAK-ANALYSIS.ch.md` — 析构链家族其他子对象(同次销毁)
- `UBSOCKET-ASYNCEVENTPOLL-EPOLLEVENT-LEAK-ANALYSIS.ch.md` — AsyncEventPoll 析构清理(同类不同对象)
- `UBSOCKET-UMQ-RX-LEAK-ANALYSIS.ch.md` — RX 运行时泄露(不同类,持续增长)
- `UBSOCKET-UMQ-TX-EVENT-LEAK-ANALYSIS.ch.md` — TX Event 泄露(不同类)
- 其他 ub_test/brpc 泄露文档
- 源码:`src/ubsocket/csrc/common/ubsocket_spsc_ring_queue.h:24,126`、`src/ubsocket/csrc/core/umq/umq_buffer_receive_queue.cpp:30-60,118-132`、`umq_socket.cpp:35-50,140-148`、`umq_socket.h:51-54,214`
