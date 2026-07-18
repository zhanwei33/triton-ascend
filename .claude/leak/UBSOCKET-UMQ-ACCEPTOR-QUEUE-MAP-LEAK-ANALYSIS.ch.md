# UBSocket Acceptor queue deque MAP 数组级联泄露分析(Acceptor 泄露之深层)

> **现象**:两条 ASan 报告,同 64B/1、同分配模式 `std::deque::_M_allocate_map`(deque MAP 指针数组),归属 Acceptor 的两个 queue:
> ```
> 报告1(wakeup_event_ ready_event_queue_ 的 deque MAP):
>     #1 std::__new_allocator<int*>::allocate
>     #2 _Deque_base<int>::_M_allocate_map stl_deque.h:597
>     #3 _M_initialize_map stl_deque.h:460
>     #4-#6 _Deque_base::_Deque_base → deque::deque → queue<int,deque>::queue
>     #7 UbsocketWakeupEvent::UbsocketWakeupEvent() ubsocket_wakeup_event.cpp:16
>     #8 Acceptor::Acceptor
>     #9 SocketBase::Create ubsocket_socket.cpp:55
>
> 报告2(ubSocket_async_accept_info ready_queue 的 deque MAP):
>     #1 std::__new_allocator<tuple<int,sockaddr,uint>*>::allocate
>     #2 _Deque_base<tuple>::_M_allocate_map stl_deque.h:597
>     #3 _M_initialize_map stl_deque.h:646
>     #4-#6 _Deque_base::_Deque_base → deque::deque → queue<tuple,deque>::queue
>     #7 Acceptor::AsyncAcceptInfo::AsyncAcceptInfo()
>     #8 Acceptor::Acceptor
>     #9 SocketBase::Create ubsocket_socket.cpp:55
> ```

## 1. 与既有 queue data node 报告的 deque 结构关系

libstdc++ `std::deque<T>` 有**两套**堆分配:MAP 数组(指针数组,指向各 data node)+ data node(实际元素 chunk)。此前已记录 data node,本报告补 MAP 数组:

| queue | MAP 数组(本报告) | data node(已文档) | deque 归属 |
|-------|-------------------|-------------------|-----------|
| `wakeup_event_.ready_event_queue_`(`queue<int>`) | **64B**(8×`int*`,报告1) | 512B(128×int,已文档) | `UbsocketWakeupEvent` → `Acceptor::wakeup_event_` |
| `ubSocket_async_accept_info.ready_queue`(`queue<tuple<int,sockaddr,socklen_t>>`) | **64B**(8×`tuple*`,报告2) | 504B(21×24B tuple,已文档) | `Acceptor::ubSocket_async_accept_info` |

同一 deque 的 MAP + data node 均 leak,因 `~Acceptor` 从不执行。此前文档(`UBSOCKET-UMQ-ACCEPTOR-WAKEUP-QUEUE-LEAK-ANALYSIS.ch.md` 512B、`UBSOCKET-UMQ-ACCEPTOR-ASYNCACCEPT-QUEUE-LEAK-ANALYSIS.ch.md` 504B)只记录了 data node,本报告补 MAP 数组(64B)。

## 2. 泄露对象

- **对象**:`std::deque<T>::_M_map`(deque 内部 MAP 指针数组,`T**` array,指向各 data node chunk)。
- **分配点**:libstdc++ `_Deque_base::_M_initialize_map`(`stl_deque.h:460/646`)→ `_M_allocate_map`(`:597`)→ `new[]` 分配 `_M_map_size` 个 `T*` 指针。
- **大小**:64 字节。libstdc++ deque 默认 `_M_map_size = 8`(8 个 `T*` 指针槽)→ `8 × 8B = 64B`。两个 queue 均如此(int queue 与 tuple queue 的 MAP 都是 8 指针 = 64B,因 MAP 存的是指针非元素)。
- **归属**:
  - 报告1:`UbsocketWakeupEvent::ready_event_queue_`(`std::queue<int, std::deque<int>>`,经 `Acceptor::wakeup_event_`)
  - 报告2:`Acceptor::ubSocket_async_accept_info.ready_queue`(`std::queue<tuple<int,sockaddr,socklen_t>, std::deque<...>>`,经 `ubSocket_async_accept_info`)

## 3. deque 内部结构(两套堆分配)

```
std::deque<T>(libstdc++)
├── _M_map(T** 数组,MAP) ← 本报告 64B(8×8B 指针槽)
│   ├── [0] → data node 0(T array)
│   ├── [1] → data node 1
│   └── ...
└── data node(T array,元素 chunk) ← 前文档 512B(int)/504B(tuple)
```

默认构造的空 deque 经 `_M_initialize_map(0)` 仍分配 MAP(8 指针)+ 1 个 data node(最小 chunk)。故空 queue 占 64B(MAP)+ 512B/504B(data node)两块堆。

## 4. 为何泄露(同 Acceptor 家族,深层级联)

`UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md` §5 已确认:`SocketBase::acceptor_` 全仓 **0 处 `delete`**,`~Acceptor` 从不执行。

级联效应:
- `acceptor_` 不 delete → `~Acceptor` 从不执行
- `~Acceptor` 不执行 → 成员析构不触发:
  - `wakeup_event_`(`UbsocketWakeupEvent` 值成员)不析构 → `ready_event_queue_`(`queue<int>`)不析构 → `~deque<int>` 不执行 → **MAP 数组(64B 报告1)+ data node(512B)不释放**
  - `ubSocket_async_accept_info`(`AsyncAcceptInfo` 值成员)不析构 → `ready_queue`(`queue<tuple>`)不析构 → `~deque<tuple>` 不执行 → **MAP 数组(64B 报告2)+ data node(504B)不释放**

**根因与 Acceptor(288B)/wakeup queue(512B)/async queue(504B)/wakeup mutex(48B,应有报告)/async mutex(48B,Acceptor 文档 §6)泄露完全相同**:`acceptor_` 无 delete → Acceptor 全部子对象(含两个 queue 的 deque MAP+data node + 两 mutex + wakeup_event_ + acceptor_ops_)级联失主。本 64B × 2 是两个 deque MAP 数组 indirect。

## 5. 为何 64B / 各 1 个

- libstdc++ deque 默认 `_M_map_size=8` → `8 × sizeof(T*)=8 × 8 = 64B`。MAP 存指针(无论 T 是 int 还是 tuple,指针都是 8B),故两 queue MAP 均为 64B。
- 各 1 个 = 单 Acceptor 的两个 queue 各一。本次仅 1 个 UmqSocket 被销毁 → 1 + 1 = 2 × 64B。

## 6. Acceptor 级联子对象汇总(至此观测完整)

| 子对象 | 成员 | 大小 | 文档 |
|--------|------|------|------|
| `Acceptor` 本体 | `SocketBase::acceptor_` | 288B(direct) | ACCEPTOR |
| `acceptor_ops_` → UmqAcceptorOps | `Ref<AcceptorOps>` | 408B(indirect) | ACCEPTOR-OPS |
| `ubSocket_async_accept_info.lock` | mutex | 48B(应有报告) | ACCEPTOR §6 |
| `ubSocket_async_accept_info.ready_queue` deque MAP | `_M_map` | **64B(本报告2)** | 本文档 |
| `ubSocket_async_accept_info.ready_queue` deque data node | data chunk | 504B | ASYNCACCEPT-QUEUE |
| `wakeup_event_.ready_event_queue_` deque MAP | `_M_map` | **64B(本报告1)** | 本文档 |
| `wakeup_event_.ready_event_queue_` deque data node | data chunk | 512B | WAKEUP-QUEUE |
| `wakeup_event_.ready_event_mutex_` | mutex | 48B(应有报告) | WAKEUP-QUEUE §3 |
| `wakeup_event_.readyEventFd_` | eventfd | fd 泄露(ASan 不跟踪) | — |

## 7. 触发条件

与 Acceptor 家族完全一致:任一 UmqSocket 被销毁即泄露 Acceptor + 全部级联(含本 64B × 2 deque MAP + 512B/504B data node + 各 mutex)。与 UB 配置无关。

## 8. 修复方案

**与 `UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md` §7、`UBSOCKET-UMQ-ACCEPTOR-WAKEUP-QUEUE-LEAK-ANALYSIS.ch.md` §7、`UBSOCKET-UMQ-ACCEPTOR-ASYNCACCEPT-QUEUE-LEAK-ANALYSIS.ch.md` §7 等共用伞形修复**——`~SocketBase` 补 `delete acceptor_`:

```cpp
~SocketBase() override {
    delete acceptor_;           // ← 触发级联:~Acceptor → 成员析构
    acceptor_ = nullptr;        //   → ~UbsocketWakeupEvent → ~queue<int> → ~deque → 释放 64B MAP + 512B data node
                                //   → ~AsyncAcceptInfo → ~queue<tuple> → ~deque → 释放 64B MAP + 504B data node
                                //   + ~Ref<AcceptorOps> → delete UmqAcceptorOps(408B)
    delete connector_;          //   + ~Connector → ~Ref → delete UmqConnectorOps(480B + 深层 vector/string)
    connector_ = nullptr;
    // ... tx_/rx_/rxQueue/mutex_ 见各自文档
    if (GlobalSetting::UBS_TRACE_ENABLED) { ... }
}
```

`delete acceptor_` → `~Acceptor` → 值成员自动析构 → `~UbsocketWakeupEvent` → `~queue<int>` → `~deque<int>` 释放 MAP(64B)+ data node(512B)+ `~AsyncAcceptInfo` → `~queue<tuple>` → `~deque<tuple>` 释放 MAP(64B)+ data node(504B)。**一处 delete 覆盖 Acceptor 全部级联**(本 64B × 2 + 512B + 504B + 408B + 288B + mutex 等)。

`~Acceptor` 还需补 `LockRegistry::LOCK_OPS.destroy(ubSocket_async_accept_info.lock)`(Acceptor 文档 §6 方案2)+ `~UbsocketWakeupEvent` 已有 `destroy(ready_event_mutex_)`(但需 `~Acceptor` 触发 `~UbsocketWakeupEvent`)。

### 与伞形析构链 PR 的关系

本修复是伞形析构链 PR 的 `acceptor_` 子项,`delete acceptor_` 经 `~Acceptor` → `~UbsocketWakeupEvent`/`~AsyncAcceptInfo` 默认析构自动释放全部 queue deque(MAP + data node)。无需额外代码修 queue,只要 `delete acceptor_` 触发整条链。

## 9. 验证

修复后 ASan 重跑:本 64B × 2(deque MAP)+ 512B + 504B(data node)+ 408B(UmqAcceptorOps)+ 288B(Acceptor)+ mutex 报告**同时消失**。可用小 `thread_num` 短测验证。

## 10. 关键代码位置索引

| 位置 | 作用 | 泄露相关性 |
|------|------|-----------|
| `ubsocket_wakeup_event.cpp:16` | `UbsocketWakeupEvent` ctor,`ready_event_queue_` 默认构造 | 报告1 deque MAP 分配触发 |
| `ubsocket_wakeup_event.h` | `UbsocketWakeupEvent` 类,`ready_event_queue_`(`queue<int>`)成员 | deque 归属 |
| `ubsocket_socket_acceptor.h:110` | `std::queue<tuple<int,sockaddr,socklen_t>> ready_queue` | 报告2 deque 归属 |
| `ubsocket_socket_acceptor.h:109-113` | `AsyncAcceptInfo`(含 ready_queue) | Acceptor 子成员 |
| `ubsocket_socket_acceptor.h:60-63,115` | `Acceptor` ctor(默认构造 `ubSocket_async_accept_info`)+ `wakeup_event_` | 两 queue 触发 |
| `ubsocket_socket_acceptor.cpp:300-305` | `~Acceptor`(只 trace,从不被调) | 级联释放入口(失效) |
| `ubsocket_socket.cpp:55` | `new Acceptor` | 层级1 direct |
| `ubsocket_socket.h:44-52,94` | `~SocketBase` 不 delete `acceptor_` | 根因 |
| `stl_deque.h:597,460,646` | libstdc++ `_M_allocate_map`/`_M_initialize_map` | deque MAP 分配 |

## 11. 与其他泄露的关系

| 维度 | 本泄露(deque MAP 64B×2) | data node 512B/504B | Acceptor 288B + 级联(408B 等) | Connector 系列(16B+480B+160B+80B) | rxQueue 级联(48B+320B+128KB+2056B) | UmqTxOps/RxOps 72/96B | mutex 48B | EpollMapper 72B/104B | AsyncEventPoll 40B | TX Event | RX | RespClosure/done | bvar 家族 | RpcMeta |
|------|------------------------|---------------------|------------------------------|------------------------------------|--------------------------------------|----------------------|-----------|----------------------|---------------------|----------|----|------------------|-----------|---------|
| 归属 | ubsocket core(Acceptor 深层) | 同 | ubsocket core | ubsocket core | ubsocket core(umq) | ubsocket core | ubsocket core | ubsocket core | ubsocket core | ubsocket umq | ubsocket umq | ub_test | brpc | brpc/protobuf |
| 类别 | Acceptor 级联子(deque MAP) | Acceptor 级联子(deque data node) | 析构不 delete + 级联 | 析构不 delete + 级联 | 析构不 delete rxQueue + 级联 | 析构不 delete | 析构不 destroy | 全局 map 不清 | 析构不清 map | 退出未释放 | buffer 不回流 | 闭包/drain | bthread 抛弃 | 解析逃逸 |
| 与 Acceptor 关系 | **级联子(deque MAP,同一 queue 的另一半)** | 级联子(deque data node) | 自身 | 独立 | 独立 | 独立 | 独立 | 独立 | 同类不同对象 | 独立 | 独立 | 独立 | 独立 | 独立 |
| ubs-comm 修复 | ✓(随 delete acceptor_ 级联消) | ✓(随 acceptor_) | ✓(伞形) | ✓(伞形) | ✓(伞形) | ✓(伞形) | ✓(伞形) | ✓(独立) | ✓(独立) | ✓ | ✓ | ❌ | ❌ | ❌ |

本泄露是 Acceptor 泄露的**深层级联子对象**(两个 queue 的 deque MAP 数组,64B × 2),与 data node(512B/504B)、Acceptor(288B)、UmqAcceptorOps(408B)等同根,随 `~SocketBase` `delete acceptor_` 一并消除(经 `~Acceptor` 默认析构 → `~queue` → `~deque` 释放 MAP + data node)。属 ubs-comm 析构链伞形修复范围。

## 参考

- `UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md` — Acceptor 288B direct(§1 表已 anticipated 级联)
- `UBSOCKET-UMQ-ACCEPTOR-WAKEUP-QUEUE-LEAK-ANALYSIS.ch.md` — wakeup queue data node 512B(同一 deque 的 data node,本报告是其 MAP)
- `UBSOCKET-UMQ-ACCEPTOR-ASYNCACCEPT-QUEUE-LEAK-ANALYSIS.ch.md` — async queue data node 504B(同上)
- `UBSOCKET-UMQ-ACCEPTOR-OPS-LEAK-ANALYSIS.ch.md` — UmqAcceptorOps 408B(同级联)
- `UBSOCKET-UMQ-CONNECTOR-*-LEAK-ANALYSIS.ch.md` — Connector 系列(同析构链家族)
- `UBSOCKET-UMQ-DATAOPS-*-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-RXQUEUE-*-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-SOCKET-MUTEX-LEAK-ANALYSIS.ch.md` — 析构链家族其他子对象/级联
- `UBSOCKET-ASYNCEVENTPOLL-EPOLLEVENT-LEAK-ANALYSIS.ch.md` / `UBSOCKET-EPOLLMAPPER-*-LEAK-ANALYSIS.ch.md` — 析构清理缺失(同类不同对象)
- `UBSOCKET-UMQ-RX-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-TX-EVENT-LEAK-ANALYSIS.ch.md` — RX/TX Event 泄露(不同类)
- 其他 ub_test/brpc 泄露文档
- 源码:`src/ubsocket/csrc/core/ubsocket_wakeup_event.cpp:16`、`ubsocket_wakeup_event.h`、`src/ubsocket/csrc/core/ubsocket_socket_acceptor.h:60-63,109-115`、`ubsocket_socket_acceptor.cpp:300-305`、`ubsocket_socket.cpp:55`、`ubsocket_socket.h:44-52,94`
