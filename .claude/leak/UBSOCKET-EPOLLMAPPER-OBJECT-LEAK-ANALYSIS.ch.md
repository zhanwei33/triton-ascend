# UBSocket EpollMapper 对象本体泄露分析(EpollMapper 泄露之父)

> **现象**:两条 ASan 报告,同 72B/1、同分配点 `CreateSocketEpollMapper`(`ubsocket_event_epoll.cpp:46`)= `new EpollMapper`,brpc 调用者不同:
> ```
> 报告1(运行时建链):
>     #1 CreateSocketEpollMapper ubsocket_event_epoll.cpp:46
>     #2 AsyncEventPoll::EpollCtl :400 → ubsocket_epoll_ctl :52 → ubsocket_wrapper_epoll_ctl :67
>     #5 brpc::EventDispatcher::RegisterEvent event_dispatcher_epoll.cpp:138
>     #7 brpc::Socket::Connect socket.cpp:1378
>
> 报告2(退出路径):
>     #1-#4 同上(CreateSocketEpollMapper → EpollCtl → ubsocket_epoll_ctl → ubsocket_wrapper_epoll_ctl)
>     #5 brpc::EventDispatcher::Stop event_dispatcher_epoll.cpp:106
>     #6 StopAndJoinGlobalDispatchers :50 → exit(libc)
> ```

## 1. 与 EpollMapper bucket 数组(104B)的父子关系

`UBSOCKET-EPOLLMAPPER-LEAK-ANALYSIS.ch.md`(104B indirect)记录了 `EpollMapper::epoll_set_`(`unordered_set<int>`)的 bucket 数组。本报告是**该 EpollMapper 对象本体**(72B),即 104B bucket 数组的**父对象**:

| 层级 | 对象 | 分配点 | 大小 | ASan 报告 |
|------|------|--------|------|-----------|
| **1** | **`EpollMapper` 对象本体** | **`ubsocket_event_epoll.cpp:46` `new EpollMapper`** | **72B** | **本报告(2× indirect)** |
| 2 | `EpollMapper::epoll_set_` bucket 数组 | `ubsocket_event_epoll.h:83` `unordered_set::insert` | 104B | 已文档(indirect) |
| 2b | `EpollMapper::mutex_`(bthread::Mutex) | `ubsocket_event_epoll.h:75` `LOCK_OPS.create` | 48B | 应有报告(indirect,~EpollMapper 空不 destroy) |

两报告各 72B = 两个不同 `EpollMapper` 对象(运行时建链 + 退出 Stop 各创建一个),其内部 bucket 数组(104B)即前文档记录的。两个 EpollMapper 均因全局 map 退出未 delete 值而泄露。

## 2. 泄露对象

- **对象**:`ock::ubs::EpollMapper` 对象本体(`ubsocket_event_epoll.h:71-108`)。
- **分配点**:`CreateSocketEpollMapper`(`ubsocket_event_epoll.cpp:38-54`):
  ```cpp
  bool CreateSocketEpollMapper(int socket_fd, EpollMapper *&mapper) {
      ...
      mapper = new (std::nothrow) EpollMapper(socket_fd);   // :46 ← 本报告 72B 分配
      ...
      g_socket_epoll_mappers[socket_fd] = mapper;            // :50 存入全局 map(裸指针)
      ...
  }
  ```
- **大小**:72 字节。`sizeof(EpollMapper)` 成员(`ubsocket_event_epoll.h:104-107`):
  - `const int fd_`(4B + 4B padding = 8B)
  - `u_mutex_t *mutex_`(8B 指针)
  - `std::unordered_set<int> epoll_set_`(~56B,libstdc++ unordered_set 内部:`_M_buckets` ptr + `_M_bucket_count` + `_M_element_count` + `_M_before_begin` + rehash policy + max_load_factor)
  - 合计 8 + 8 + 56 = 72B,吻合。
- **归属**:存于全局 `g_socket_epoll_mappers`(`ubsocket_event_epoll.cpp:25` `std::unordered_map<int, EpollMapper*>`)裸指针值。

## 3. 根因(同 EpollMapper bucket 数组泄露)

`UBSOCKET-EPOLLMAPPER-LEAK-ANALYSIS.ch.md` §3 已确认:**`g_socket_epoll_mappers` 全局 map 持 `EpollMapper*` 裸指针,退出不 delete 值**。

- `g_socket_epoll_mappers` 是全局静态 `std::unordered_map<int, EpollMapper*>`。
- 进程退出:全局静态析构 `~unordered_map` erases entries,但 `EpollMapper*` 值是**裸指针→不 delete**。
- `CleanSocketEpollMapper`(`:56-69`)是 per-socket 清理(erase + `delete mapper`),但退出未对全部 socket 调。
- 结果:`EpollMapper` 对象(72B)+ 其内部 `epoll_set_` bucket 数组(104B)+ `mutex_`(48B)全部失主泄露。

**两报告对应两条产生路径**:

| 报告 | 产生路径 | EpollMapper 来源 |
|------|---------|-----------------|
| 报告1(运行时) | `Socket::Connect`(`:1378`)→ `RegisterEvent`(`:138`)→ `ubsocket_epoll_ctl(ADD)` → `EpollCtl`(`:400`)→ `CreateSocketEpollMapper`(`:46`) | socket 建链时创建 EpollMapper 存入全局 map |
| 报告2(退出) | `exit` → `StopAndJoinGlobalDispatchers` → `EventDispatcher::Stop`(`:106`)→ `ubsocket_epoll_ctl` → `EpollCtl` → `CreateSocketEpollMapper` | 退出时 Stop 再创建(可能不同 socket_fd 的 EpollMapper) |

两条均因全局 map 退出未 delete 值导致 EpollMapper 对象泄露。

## 4. 为何 72B / 各 1 个

- `sizeof(EpollMapper)` = 72B(fd_ 8 + mutex_ 8 + epoll_set_ 56)。
- 各 1 个 = 两个不同 EpollMapper 对象(运行时 + 退出 Stop 各一)。
- 退出时刻 2 个 EpollMapper 残留在全局 map(或已被 erase 但未 delete)→ 2 × 72B 泄露。
- 每个 EpollMapper 内部还有 bucket 数组(104B)+ mutex_(48B)级联。

## 5. 次因:`~EpollMapper()` 空析构

`UBSOCKET-EPOLLMAPPER-LEAK-ANALYSIS.ch.md` §3 次因已指出:`~EpollMapper()`(`ubsocket_event_epoll.h:78`)是**空体**——不 `LockRegistry::LOCK_OPS.destroy(mutex_)`。即便 `delete mapper` 被调(经 `CleanSocketEpollMapper`),`mutex_`(48B)仍泄露(空析构不 destroy)。`Clear()`(`:102`)也空。本报告的 72B EpollMapper 对象若被 delete 会释放本体 + bucket 数组(经 `~unordered_set` 成员析构),但 mutex_ 仍需 `~EpollMapper` 补 destroy。

## 6. 触发条件

- brpc socket 建链或退出 Stop 调 `ubsocket_epoll_ctl` → `CreateSocketEpollMapper` 创建 EpollMapper 存入全局 map
- 进程退出未对每个 socket 调 `CleanSocketEpollMapper`(或全局 map 析构不 delete 值)→ EpollMapper 对象(72B)+ bucket 数组(104B)+ mutex_(48B)泄露
- 与 UB 配置无关,ubsocket 使能即存在

## 7. 修复方案

**与 `UBSOCKET-EPOLLMAPPER-LEAK-ANALYSIS.ch.md` §7 完全共用**:

### 方案 1【必须】`ubsocket_uninit` 清理全局 map

`ubsocket.cpp:208-246` 的 `ubsocket_uninit` 中补:

```cpp
void ubsocket_uninit() {
    ...
    // 新增:清理 g_socket_epoll_mappers(消 EpollMapper 对象 72B + bucket 数组 104B)
    {
        WriteLocker sLock(g_socket_epoll_lock);
        for (auto &kv : g_socket_epoll_mappers) {
            if (kv.second != nullptr) {
                delete kv.second;          // → ~EpollMapper(当前空) → ~unordered_set 释放 104B bucket + 不 destroy mutex(需方案2)
                kv.second = nullptr;
            }
        }
        g_socket_epoll_mappers.clear();
    }
    TxCqePoller::Instance().Stop();
    ArraySet<Socket>::GetInstance().ReleaseAll();
    ArraySet<EventPoll>::GetInstance().ReleaseAll();
    ...
}
```

`delete kv.second` → `~EpollMapper`(空)→ 成员自动析构:`~unordered_set` 释放 bucket 数组(104B 层级2)+ `~mutex_` 不释放(需方案2)。**一处 delete 覆盖 EpollMapper 对象(72B 层级1)+ bucket 数组(104B 层级2)**。

### 方案 2【必须】`~EpollMapper` 补 destroy mutex

`ubsocket_event_epoll.h:78`:

```cpp
~EpollMapper() {
    if (mutex_ != nullptr) {
        LockRegistry::LOCK_OPS.destroy(mutex_);   // ← 新增:释放 mutex(48B 层级2b)
        mutex_ = nullptr;
    }
    // epoll_set_ 由成员自动析构 ~unordered_set 释放 bucket(104B)
}
```

配合方案1:`delete kv.second` → `~EpollMapper`(补 destroy)→ destroy mutex_(48B)+ `~unordered_set` 释放 bucket(104B)+ EpollMapper 本体(72B)由 `operator delete` 释放。**方案1+2 一起覆盖 EpollMapper 全部三层(72B + 104B + 48B)**。

### 与其他修复的关系

本修复独立于 UmqSocket 析构链伞形(不同对象:EpollMapper 全局 map vs UmqSocket 子对象),与 `UBSOCKET-ASYNCEVENTPOLL-EPOLLEVENT-LEAK-ANALYSIS.ch.md`(`~AsyncEventPoll`)是同文件不同对象的清理缺失,建议合并修。

## 8. 验证

修复后 ASan 重跑:两条 72B(EpollMapper 对象)+ 104B(bucket 数组)+ 48B(mutex,若有)**同时消失**。可用小 `thread_num` 短测验证。

## 9. 关键代码位置索引

| 位置 | 作用 | 泄露相关性 |
|------|------|-----------|
| `ubsocket_event_epoll.cpp:46` | `new EpollMapper` in `CreateSocketEpollMapper` | **本报告 72B 分配点(层级1)** |
| `ubsocket_event_epoll.cpp:25` | `g_socket_epoll_mappers` 全局 map | 持 EpollMapper* 裸指针(根因) |
| `ubsocket_event_epoll.cpp:50` | `g_socket_epoll_mappers[socket_fd] = mapper` | 存入全局 map |
| `ubsocket_event_epoll.cpp:56-69` | `CleanSocketEpollMapper`(per-socket 清理) | 退出未对全部 socket 调 |
| `ubsocket_event_epoll.h:71-108` | `EpollMapper` 类(72B) | 泄露对象 |
| `ubsocket_event_epoll.h:75` | ctor `mutex_ = LOCK_OPS.create` | 层级2b mutex(48B) |
| `ubsocket_event_epoll.h:78` | `~EpollMapper() {}` 空 | 不 destroy mutex_ |
| `ubsocket_event_epoll.h:107` | `std::unordered_set<int> epoll_set_` | 层级2 bucket 数组(104B) |
| `ubsocket_event_epoll.cpp:400,408` | `EpollCtl` 调 `CreateSocketEpollMapper` + `mapper->Add` | frame #2 |
| `ubsocket.cpp:208-246` | `ubsocket_uninit` 不清全局 map | 根因(退出清理缺失) |

## 10. 与其他泄露的关系

| 维度 | 本泄露(EpollMapper 72B×2 层级1) | EpollMapper bucket 104B 层级2 | EpollMapper mutex 48B 层级2b | AsyncEventPoll EpollEvent 40B | UmqSocket 析构链家族 | TX Event | RX | RespClosure/done | bvar 家族 | RpcMeta |
|------|--------------------------------|------------------------------|------------------------------|-------------------------------|---------------------|----------|----|------------------|-----------|---------|
| 归属 | ubsocket core(EpollMapper 全局 map) | 同 | 同 | ubsocket core(AsyncEventPoll) | ubsocket core | ubsocket umq | ubsocket umq | ub_test | brpc | brpc/protobuf |
| 类别 | 全局 map 退出不 delete 值(EpollMapper 对象) | EpollMapper 级联子(bucket) | EpollMapper 级联子(mutex,~空不 destroy) | ~AsyncEventPoll 不清 socket_data_ | 析构不 delete | 退出未释放 | buffer 不回流 | 闭包/drain | bthread 抛弃 | 解析逃逸 |
| 与 EpollMapper bucket 关系 | **层级1 父对象** | 层级2 子 | 层级2b 子 | 独立 | 独立 | 独立 | 独立 | 独立 | 独立 | 独立 |
| ubs-comm 修复 | ✓(`ubsocket_uninit` 清 map,随 delete 覆盖层级1+2) | ✓(随 EpollMapper delete) | ✓(需 ~EpollMapper 补 destroy) | ✓(`~AsyncEventPoll`) | ✓(伞形) | ✓ | ✓ | ❌ | ❌ | ❌ |

本泄露是 `g_socket_epoll_mappers` 全局 map 退出清理缺失导致的 **EpollMapper 对象本体(72B)泄露**,是 `UBSOCKET-EPOLLMAPPER-LEAK-ANALYSIS.ch.md`(104B bucket 数组)的**父对象**(层级1)。随 `ubsocket_uninit` 清全局 map `delete` 每个 EpollMapper 覆盖层级1(72B)+层级2(104B bucket);`~EpollMapper` 补 destroy 覆盖层级2b(48B mutex)。属 ubs-comm 析构清理缺失批次。

## 参考

- `UBSOCKET-EPOLLMAPPER-LEAK-ANALYSIS.ch.md` — EpollMapper bucket 数组 104B(层级2,§3-5 根因/修复共用)
- `UBSOCKET-ASYNCEVENTPOLL-EPOLLEVENT-LEAK-ANALYSIS.ch.md` — `~AsyncEventPoll` 不清 socket_data_(同文件不同对象,合并修)
- `UBSOCKET-UMQ-ACCEPTOR-*-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-CONNECTOR-*-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-DATAOPS-*-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-RXQUEUE-*-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-SOCKET-MUTEX-LEAK-ANALYSIS.ch.md` — UmqSocket 析构链家族(不同对象)
- `UBSOCKET-UMQ-RX-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-TX-EVENT-LEAK-ANALYSIS.ch.md` — RX/TX Event 泄露(不同类)
- 其他 ub_test/brpc 泄露文档
- 源码:`src/ubsocket/csrc/core/ubsocket_event_epoll.h:71-108`、`ubsocket_event_epoll.cpp:25-69,400,408`、`ubsocket.cpp:208-246`
