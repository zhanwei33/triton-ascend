# UBSocket AsyncEventPoll EpollEvent 析构泄露分析

> **现象**:两条 ASan 报告,同 40B/1、同分配点,brpc 调用者不同:
> ```
> 报告1(退出路径):
>     #1 AsyncEventPoll::AddRawSocketEvent ubsocket_event_epoll.cpp:626
>     #2 EpollCtlAdd :582 → EpollCtl :406 → ubsocket_epoll_ctl :52 → ubsocket_wrapper_epoll_ctl :67
>     #6 brpc::EventDispatcher::Stop event_dispatcher_epoll.cpp:106
>     #7 StopAndJoinGlobalDispatchers :50 → exit(libc)
>
> 报告2(运行时建链路径):
>     #1 AsyncEventPoll::AddRawSocketEvent ubsocket_event_epoll.cpp:626
>     #2-#5 同上(EpollCtlAdd → EpollCtl → ubsocket_epoll_ctl → ubsocket_wrapper_epoll_ctl)
>     #6 brpc::EventDispatcher::AddConsumer event_dispatcher_epoll.cpp:172
>     #7 IOEvent<Socket>::AddConsumer → Socket::ResetFileDescriptor :643
>     #8-#17 CheckConnectedAndKeepWrite → ... → EventDispatcher::Run :246(事件循环)
> ```

## 1. 泄露对象

- **对象**:`ock::ubs::EpollEvent`(`ubsocket_event_epoll.h:35`),类型 `EPOLL_EVENT_RAW_SOCKET`。
- **分配点**:`AsyncEventPoll::AddRawSocketEvent`(`ubsocket_event_epoll.cpp:626`):
  ```cpp
  auto event_data = new (std::nothrow) EpollEvent(EPOLL_EVENT_RAW_SOCKET, fd, *event);
  ```
- **大小**:40 字节(`EpollEvent` = `EpollEventType` + `int socket` + `epoll_event` + `EpollEvent* next`)。
- **归属**:存入 `AsyncEventPoll::socket_data_`(`ubsocket_event_epoll.h:519` `std::unordered_map<int, EpollEvent*>`)via `InsertSocketEventData`(`:641`)。

## 2. 正常清理路径(延迟释放)

brpc 经 `ubsocket_wrapper_epoll_ctl`(EPOLL_CTL_DEL)→ `ubsocket_epoll_ctl` → `AsyncEventPoll::EpollCtl`(`:406`)→ `EpollCtlDel`(`:751`)→ `DelRawSocketEvent`(`:693`):

```cpp
int AsyncEventPoll::DelRawSocketEvent(int fd) {
    if (!RemoveSocketEventData(fd)) { return 0; }   // ← 推入 removed_head_ 延迟链
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    return 0;
}
```

`RemoveSocketEventData`(`ubsocket_event_epoll.h:478-492`):

```cpp
ALWAYS_INLINE bool RemoveSocketEventData(int fd) noexcept {
    Locker sLock(mutex_);
    auto pos = socket_data_.find(fd);
    if (pos == socket_data_.end()) { return false; }
    auto removed = pos->second;
    socket_data_.erase(pos);              // 仅 erase map 项
    if (removed != nullptr) {
        removed->next = removed_head_;     // 推入延迟释放链表(:520 "待删除的event data列表")
        removed_head_ = removed;           // 不立即 delete
    }
    return true;
}
```

`ReleaseRemovedEventsData`(`:555-566`)在 `epoll_wait` 循环中统一释放:

```cpp
void AsyncEventPoll::ReleaseRemovedEventsData() {
    Locker sLock(ctl_mutex_);
    auto removed_head = removed_head_;
    removed_head_ = nullptr;
    while (removed_head != nullptr) {
        auto next = removed_head->next;
        delete removed_head;              // ← 真正 delete
        removed_head = next;
    }
}
```

**正常路径下 EpollEvent 经 DEL→removed_head_→epoll_wait 释放,无泄露。**

## 3. 泄露根因:`~AsyncEventPoll` 不清 socket_data_ / removed_head_

`~AsyncEventPoll`(`ubsocket_event_epoll.cpp:348-358`):

```cpp
AsyncEventPoll::~AsyncEventPoll() noexcept {
    UBS_VLOG_INFO("async_epoll destructure invoked for fd: %d\n", epoll_fd_);
    if (epoll_fd_ < 0 || sock_readable_fd_ < 0) { return; }
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, sock_readable_fd_, nullptr);
    close(sock_readable_fd_);
    sock_readable_fd_ = -1;
    // ❌ 不遍历 socket_data_ delete EpollEvent*
    // ❌ 不调 ReleaseRemovedEventsData() 释放 removed_head_
}
```

`AsyncEventPoll` 在 `ubsocket_uninit`(`ubsocket.cpp:234`)经 `ArraySet<EventPoll>::ReleaseAll()` → `DecreaseRef` → ref→0 → `delete` → `~AsyncEventPoll`。析构时:

- `socket_data_` 中**仍注册的 EpollEvent\***(DEL 未调或未及调)→ map 析构 erase 指针但**不 delete 对象** → 泄露
- `removed_head_` 链表(已 DEL 但 epoll_wait 未及 `ReleaseRemovedEventsData`)→ **整条链泄露**

两条 ASan 报告对应两条产生路径:

| 报告 | 产生路径 | 泄露时 EpollEvent 所在 |
|------|---------|----------------------|
| 报告1(退出路径) | `EventDispatcher::Stop`(`:106`)退出时 ADD 一个 raw socket event(wakeup/abort fd),进程随即退出,DEL 未调 | `socket_data_`(仍注册)→ ~AsyncEventPoll 不清 |
| 报告2(运行时路径) | `Socket::ResetFileDescriptor`(`:643`)建链时 ADD,连接生命周期内 DEL 推入 `removed_head_`,但 epoll_wait 未及 `ReleaseRemovedEventsData` 进程即退出 | `removed_head_`(延迟链)→ ~AsyncEventPoll 不清 |

两条均为 `~AsyncEventPoll` 析构时未释放残留 EpollEvent* 所致。

## 4. 触发条件

- `ubsocket_uninit` 跑 `ArraySet<EventPoll>::ReleaseAll()` 销毁 EventPoll(正常退出路径)
- 该 EventPoll 的 `socket_data_` 非空(有未 DEL 的注册)或 `removed_head_` 非空(有 DEL 但未释放的延迟链)
- 与 UB 配置无关,ubsocket 使能即存在

## 5. 修复方案

`~AsyncEventPoll` 补清理:

```cpp
AsyncEventPoll::~AsyncEventPoll() noexcept {
    UBS_VLOG_INFO("async_epoll destructure invoked for fd: %d\n", epoll_fd_);

    // 1. 释放延迟链(已 DEL 但 epoll_wait 未释放)
    ReleaseRemovedEventsData();

    // 2. 释放仍注册在 socket_data_ 的 EpollEvent*
    {
        Locker sLock(mutex_);
        for (auto &kv : socket_data_) {
            if (kv.second != nullptr) { delete kv.second; }
        }
        socket_data_.clear();
    }

    // 3. 原有关 sock_readable_fd_ 逻辑
    if (epoll_fd_ < 0 || sock_readable_fd_ < 0) { return; }
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, sock_readable_fd_, nullptr);
    close(sock_readable_fd_);
    sock_readable_fd_ = -1;
}
```

要点:
- 先 `ReleaseRemovedEventsData()`(内部加 `ctl_mutex_` 锁,释放 removed_head_)
- 再遍历 `socket_data_` delete(加 `mutex_` 锁,与 `RemoveSocketEventData`/`InsertSocketEventData` 互斥)
- 顺序:epoll_fd_ 关闭可在最后(关 fd 后所有 epoll 注册自动失效,但 EpollEvent* 堆对象仍需手动 delete)

### 与析构链家族的关系

本泄露与 `UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md` 系列(UmqSocket 子对象析构链)是**同类不同对象**:都是"析构函数清理缺失"——前者 `~AsyncEventPoll` 不清 socket_data_/removed_head_,后者 `~SocketBase`/`~DataTx`/`~DataRx`/`~UmqSocket` 不 delete 子对象。可纳入"析构链清理"伞形修复批次,但修复点不同(本处在 `ubsocket_event_epoll.cpp` ~AsyncEventPoll,彼处在 `ubsocket_socket.h/cpp`、`umq_socket.cpp`)。

## 6. 验证

修复后 ASan 重跑:两条 40B/1 报告应同时消失。可构造短测(单连接建拆 + 立即退出)验证 `socket_data_` 与 `removed_head_` 残留路径均覆盖。

## 7. 关键代码位置索引

| 位置 | 作用 | 泄露相关性 |
|------|------|-----------|
| `ubsocket_event_epoll.cpp:626` | `new EpollEvent` in `AddRawSocketEvent` | **泄露分配点** |
| `ubsocket_event_epoll.h:519` | `socket_data_` map | ~AsyncEventPoll 不清 |
| `ubsocket_event_epoll.h:520` | `removed_head_` 延迟链 | ~AsyncEventPoll 不清 |
| `ubsocket_event_epoll.h:478-492` | `RemoveSocketEventData` 推入 removed_head_ | 正常延迟释放(若 epoll_wait 跑) |
| `ubsocket_event_epoll.cpp:555-566` | `ReleaseRemovedEventsData` delete 链 | 正常释放(由 epoll_wait 调) |
| `ubsocket_event_epoll.cpp:693-706` | `DelRawSocketEvent` | DEL 路径(推 removed_head_) |
| `ubsocket_event_epoll.cpp:348-358` | `~AsyncEventPoll` | **不清 socket_data_/removed_head_** |
| `ubsocket.cpp:234` | `ArraySet<EventPoll>::ReleaseAll` | 触发 ~AsyncEventPoll |

## 8. 与其他泄露的关系

| 维度 | 本泄露(EpollEvent 40B×2) | UmqSocket 析构链家族(288/96/72/48/48B) | TX Event | RX | RespClosure/done | bvar 家族 | RpcMeta |
|------|------------------------|----------------------------------------|----------|----|------------------|-----------|---------|
| 归属 | ubsocket core(EventPoll) | ubsocket core(Socket) | ubsocket umq | ubsocket umq | ub_test | brpc | brpc/protobuf |
| 类别 | 析构不清 socket_data_/removed_head_ | 析构不 delete 子对象 | 退出未释放 | buffer 不回流 | 闭包/drain | bthread 抛弃 | 解析逃逸 |
| 修复点 | `~AsyncEventPoll` | `~SocketBase`/`~DataTx`/`~DataRx`/`~UmqSocket` | uninit/Clean/RebuildTp | AddQbuf/Enqueue | client.cpp | brpc atexit | 无 |
| ubs-comm 修复 | ✓ | ✓ | ✓ | ✓ | ❌ | ❌ | ❌ |

本泄露属 ubsocket core 层(EventPoll),与 UmqSocket 析构链家族是"析构清理缺失"同类,但修复点独立。

## 参考

- `UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-DATAOPS-*-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-RXQUEUE-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-SOCKET-MUTEX-LEAK-ANALYSIS.ch.md` — UmqSocket 析构链家族(同类不同对象)
- `UBSOCKET-UMQ-TX-EVENT-LEAK-ANALYSIS.ch.md` — TX Event 启动期泄露(不同类)
- `UBSOCKET-UMQ-RX-LEAK-ANALYSIS.ch.md` — RX 运行时泄露(不同类)
- 其他 ub_test/brpc 泄露文档
- 源码:`src/ubsocket/csrc/core/ubsocket_event_epoll.cpp:348-358,555-566,622-649,693-706,751-778`、`ubsocket_event_epoll.h:35-42,478-492,519-520`
