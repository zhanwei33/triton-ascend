# UBSocket bthread::Mutex 泄露汇总分析(四条 anticipated mutex 报告)

> **现象**:四条 ASan 报告,同 48B/1、同分配点 `brpc_external_lock_create`(`ubsocket_initializer.cpp:407`)= `new bthread::Mutex()`,归属四个不同父对象的 `mutex_` 成员。
>
> 本文汇总这四条(均是多份既有文档 anticipated 的 "mutex 应有报告"),不再逐个建独立文档。

## 1. 四条 mutex 泄露映射

| # | 父对象 | mutex 成员 | 创建点 | 析构缺失原因 | ASan frame #2 | 对应文档 anticipated |
|---|--------|-----------|--------|-------------|---------------|---------------------|
| 1 | `EpollMapper`(运行时) | `EpollMapper::mutex_` | `ubsocket_event_epoll.h:75` | `~EpollMapper(){}` 空 + 全局 map 不 delete | `EpollMapper::EpollMapper(int) :75` | EPOLLMAPPER 系列 §5 |
| 2 | `Acceptor` | `ubSocket_async_accept_info.lock` | `ubsocket_socket_acceptor.h:62` | `~Acceptor` 不 destroy + `acceptor_` 不 delete | `Acceptor::Acceptor` | ACCEPTOR §6 |
| 3 | `UbsocketWakeupEvent`(Acceptor 子) | `ready_event_mutex_` | `ubsocket_wakeup_event.cpp:18` | `~Acceptor` 不执行→`~UbsocketWakeupEvent` 不执行(虽有 destroy 代码 :25) | `UbsocketWakeupEvent::UbsocketWakeupEvent() :18` | WAKEUP-QUEUE §1 表 |
| 4 | `EpollMapper`(退出 Stop) | `EpollMapper::mutex_` | `ubsocket_event_epoll.h:75` | 同 #1(不同 EpollMapper,退出 Stop 创建) | `EpollMapper::EpollMapper(int) :75` | EPOLLMAPPER-OBJECT §5 |

## 2. 泄露对象

- **对象**:`bthread::Mutex`(brpc 注入的锁实现,经 `brpc_external_lock_create` `new` 出)。
- **分配点**:`brpc_external_lock_create`(`ubsocket_initializer.cpp:363`/用户构建 407):`auto* mutex = new(std::nothrow) bthread::Mutex();` → 经 `LockRegistry::LOCK_OPS.create(LT_EXCLUSIVE)` 返回 `u_mutex_t*`。
- **大小**:48 字节(`sizeof(bthread::Mutex)` on aarch64)。
- **归属**:四个不同父对象的 `u_mutex_t* mutex_` 裸指针成员。

## 3. 四个父对象的 mutex 析构缺失分析

### 报告1/4:EpollMapper::mutex_(`ubsocket_event_epoll.h:75`)

```cpp
explicit EpollMapper(int fd) : fd_(fd) {
    mutex_ = LockRegistry::LOCK_OPS.create(LT_EXCLUSIVE);   // :75 创建 mutex(48B)
}
~EpollMapper() {}                                           // :78 ❌ 空!不 destroy mutex_
```

**双重缺失**:
1. `~EpollMapper()` 空——即便 `delete mapper` 被调,也不 `LockRegistry::LOCK_OPS.destroy(mutex_)` → mutex(48B)泄露
2. `g_socket_epoll_mappers` 全局 map 退出不 delete EpollMapper* → `~EpollMapper` 从不执行 → mutex 泄露

报告1 = 运行时 `RegisterEvent`(`:138`)创建的 EpollMapper;报告4 = 退出 `EventDispatcher::Stop`(`:106`)创建的 EpollMapper。两个不同 EpollMapper,各一 mutex。

### 报告2:Acceptor::ubSocket_async_accept_info.lock(`ubsocket_socket_acceptor.h:62`)

```cpp
Acceptor(const SocketPtr &sock, AcceptorOps *acceptorOps) : raw_fd_(...), acceptor_ops_(...) {
    ubSocket_async_accept_info.lock = LockRegistry::LOCK_OPS.create(LT_EXCLUSIVE);  // :62 创建 mutex(48B)
}
~Acceptor() { /* 只 trace,不 destroy lock */ }   // ubsocket_socket_acceptor.cpp:300-305
```

**缺失**:`acceptor_` 全仓 0 处 `delete` → `~Acceptor` 从不执行 → `ubSocket_async_accept_info.lock` 不 destroy → mutex(48B)泄露。即便 `~Acceptor` 执行,其函数体也**不** `destroy(lock)`(只做 trace)。

### 报告3:UbsocketWakeupEvent::ready_event_mutex_(`ubsocket_wakeup_event.cpp:18`)

```cpp
UbsocketWakeupEvent::UbsocketWakeupEvent() : epollFd_(-1), readyEventFd_(-1), ready_event_mutex_(nullptr) {
    ready_event_mutex_ = LockRegistry::LOCK_OPS.create(LT_EXCLUSIVE);  // :18 创建 mutex(48B)
}
~UbsocketWakeupEvent() {
    if (ready_event_mutex_ != nullptr) {
        LockRegistry::LOCK_OPS.destroy(ready_event_mutex_);   // :25 ✓ 有 destroy 代码!
        ready_event_mutex_ = nullptr;
    }
    ...
}
```

**缺失**:`UbsocketWakeupEvent` 是 `Acceptor::wakeup_event_` 值成员。`~Acceptor` 不执行 → `~UbsocketWakeupEvent` 不执行 → `:25` 的 destroy 代码**从不被调** → mutex(48B)泄露。注意:与报告1/2/4 不同,`~UbsocketWakeupEvent` **有** destroy 代码(`:25`),但因父析构不执行而不触发。

## 4. 四条 mutex 的根因与修复归属

| 报告 | 根因 | 修复方案 | 修复文档 |
|------|------|---------|---------|
| 1/4(EpollMapper mutex) | `~EpollMapper` 空 + 全局 map 不 delete | 方案1:`ubsocket_uninit` 清 map `delete` EpollMapper;方案2:`~EpollMapper` 补 destroy | `UBSOCKET-EPOLLMAPPER-LEAK-ANALYSIS.ch.md` §7 |
| 2(Acceptor async_accept lock) | `acceptor_` 不 delete + `~Acceptor` 不 destroy | `~SocketBase` 补 `delete acceptor_` + `~Acceptor` 补 `destroy(lock)` | `UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md` §7 |
| 3(WakeupEvent ready_event_mutex) | `acceptor_` 不 delete → `~UbsocketWakeupEvent` 不执行(有 destroy 代码但不触发) | `~SocketBase` 补 `delete acceptor_` → `~Acceptor` → `~UbsocketWakeupEvent` → `:25` destroy 执行 | `UBSOCKET-UMQ-ACCEPTOR-WAKEUP-QUEUE-LEAK-ANALYSIS.ch.md` §7 |

**报告1/4** 需两步修(`ubsocket_uninit` 清 map + `~EpollMapper` 补 destroy);**报告2** 需 `~SocketBase` delete acceptor_ + `~Acceptor` 补 destroy;**报告3** 只需 `~SocketBase` delete acceptor_(`~UbsocketWakeupEvent` 的 destroy 代码已存在,只需触发)。

## 5. 为何 48B / 各 1 个

- `sizeof(bthread::Mutex)` = 48B on aarch64(brpc 注入的 bthread 互斥锁)。
- 各 1 个 = 四个不同父对象(EpollMapper×2 + Acceptor + UbsocketWakeupEvent)各持一个 mutex。

## 6. 触发条件

- 报告1/4:brpc socket 建链/退出 Stop → `CreateSocketEpollMapper` → `new EpollMapper` → `mutex_` create;退出不 delete + `~EpollMapper` 空 →泄露
- 报告2/3:`SocketBase::Create` → `new Acceptor` → Acceptor ctor 创建 `ubSocket_async_accept_info.lock` + `wakeup_event_` ctor 创建 `ready_event_mutex_`;`acceptor_` 不 delete → 不析构 → 两 mutex 泄露
- 与 UB 配置无关

## 7. 关键代码位置索引

| 位置 | 作用 | 泄露相关性 |
|------|------|-----------|
| `ubsocket_initializer.cpp:363`(用户 407) | `brpc_external_lock_create` `new bthread::Mutex` | **四条共同分配点** |
| `ubsocket_event_epoll.h:75` | `EpollMapper` ctor create mutex | 报告1/4 |
| `ubsocket_event_epoll.h:78` | `~EpollMapper(){}` 空 | 不 destroy(报告1/4 次因) |
| `ubsocket_socket_acceptor.h:62` | `Acceptor` ctor create `ubSocket_async_accept_info.lock` | 报告2 |
| `ubsocket_socket_acceptor.cpp:300-305` | `~Acceptor` 只 trace | 不 destroy lock(报告2) |
| `ubsocket_wakeup_event.cpp:18` | `UbsocketWakeupEvent` ctor create `ready_event_mutex_` | 报告3 |
| `ubsocket_wakeup_event.cpp:21-26` | `~UbsocketWakeupEvent` 有 destroy(`:25`)但不被调 | 报告3 |
| `ubsocket_event_epoll.cpp:25` | `g_socket_epoll_mappers` 全局 map | 报告1/4 根因(不 delete 值) |
| `ubsocket_socket.h:44-52,94` | `~SocketBase` 不 delete `acceptor_` | 报告2/3 根因 |

## 8. 与其他泄露的关系

| 报告 | 归属 | 对应既有文档 anticipated | 修复点 |
|------|------|-------------------------|--------|
| 1/4(EpollMapper mutex) | ubsocket core(EpollMapper) | `UBSOCKET-EPOLLMAPPER-*-LEAK-ANALYSIS.ch.md` §5 | `ubsocket_uninit` 清 map + `~EpollMapper` 补 destroy |
| 2(Acceptor async_accept lock) | ubsocket core(Acceptor) | `UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md` §6 | `~SocketBase` delete acceptor_ + `~Acceptor` 补 destroy |
| 3(WakeupEvent ready_event_mutex) | ubsocket core(Acceptor/wakeup) | `UBSOCKET-UMQ-ACCEPTOR-WAKEUP-QUEUE-LEAK-ANALYSIS.ch.md` §1 表 | `~SocketBase` delete acceptor_(触发 `~UbsocketWakeupEvent` :25 destroy) |

四条 mutex 泄露是多份既有文档 anticipated 的 "mutex 应有报告",分属 EpollMapper(2×)+ Acceptor(1×)+ WakeupEvent(1×)三个父对象。修复分别归属 EpollMapper 修复(§3 方案1+2)和伞形析构链修复(§3 `delete acceptor_` + `~Acceptor` 补 destroy)。

## 参考

- `UBSOCKET-EPOLLMAPPER-LEAK-ANALYSIS.ch.md` — EpollMapper bucket 104B(§5 anticipated mutex;§7 方案2 `~EpollMapper` 补 destroy)
- `UBSOCKET-EPOLLMAPPER-OBJECT-LEAK-ANALYSIS.ch.md` — EpollMapper 对象 72B(§5 anticipated mutex)
- `UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md` — Acceptor 288B(§6 anticipated Acceptor 内 mutex;§7 方案2 `~Acceptor` 补 destroy)
- `UBSOCKET-UMQ-ACCEPTOR-WAKEUP-QUEUE-LEAK-ANALYSIS.ch.md` — wakeup queue 512B(§1 表 anticipated ready_event_mutex_)
- `UBSOCKET-UMQ-SOCKET-MUTEX-LEAK-ANALYSIS.ch.md` — UmqSocket::mutex_ 48B(另一个 mutex 泄露,UmqSocket 析构链家族)
- 其他 ubs-comm/ub_test/brpc 泄露文档
- 源码:`src/brpc/ubsocket_initializer.cpp:357-379`、`src/ubsocket/csrc/core/ubsocket_event_epoll.h:71-108`、`src/ubsocket/csrc/core/ubsocket_socket_acceptor.h:60-63,109-115`、`src/ubsocket/csrc/core/ubsocket_wakeup_event.cpp:16-26`、`src/ubsocket/csrc/core/ubsocket_socket_acceptor.cpp:300-305`
