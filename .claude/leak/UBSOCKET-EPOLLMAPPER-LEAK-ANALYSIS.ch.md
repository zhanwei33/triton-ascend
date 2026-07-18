# UBSocket EpollMapper 全局 map 泄露分析(析构清理缺失家族)

> **现象**:两条 ASan 报告,同 104B/1、同分配点 `EpollMapper::Add`(`ubsocket_event_epoll.h:83`)`unordered_set::insert` bucket 数组,brpc 调用者不同:
> ```
> 报告1(运行时建链):
>     #1 std::__new_allocator<_Hash_node_base*>::allocate
>     #2 _Hashtable_alloc::_M_allocate_buckets
>     #3-#10 _Hashtable::_M_rehash → _M_insert_unique_node → insert → unordered_set::insert
>     #11 EpollMapper::Add ubsocket_event_epoll.h:83
>     #12 AsyncEventPoll::EpollCtl ubsocket_event_epoll.cpp:408
>     #13 ubsocket_epoll_ctl :52 → ubsocket_wrapper_epoll_ctl :67
>     #15 brpc::EventDispatcher::RegisterEvent event_dispatcher_epoll.cpp:138
>     #17 brpc::Socket::Connect socket.cpp:1378
>
> 报告2(退出路径):
>     #1-#12 同上(EpollMapper::Add → unordered_set::insert)
>     #15 brpc::EventDispatcher::Stop event_dispatcher_epoll.cpp:106
>     #16 StopAndJoinGlobalDispatchers :50 → exit(libc)
> ```

## 1. 泄露对象

- **对象**:`std::unordered_set<int>` 的 bucket 数组(`EpollMapper::epoll_set_` 的底层 `_Hash_node_base*[]`)。
- **分配点**:`EpollMapper::Add(int epoll_fd)`(`ubsocket_event_epoll.h:80-84`):
  ```cpp
  void Add(int epoll_fd) {
      Locker sLock(mutex_);
      epoll_set_.insert(epoll_fd);   // :83 ← insert 触发 _M_rehash → _M_allocate_buckets → new[]
  }
  ```
  libstdc++ `unordered_set<int>` 首次 insert 触发 rehash,分配 bucket 数组(`_Prime_rehash_policy` 取最小素数,104B = 13 buckets × 8B 指针)。
- **大小**:104 字节(13 × `sizeof(_Hash_node_base*)==8`)。
- **归属**:`EpollMapper::epoll_set_`(`ubsocket_event_epoll.h:107`),经 `EpollMapper` 存于全局 `g_socket_epoll_mappers`(`ubsocket_event_epoll.cpp:25`)。

## 2. EpollMapper 生命周期与全局 map

`EpollMapper`(`ubsocket_event_epoll.h:71-108`)是 socket_fd → epoll_fd 集合的映射,跟踪一个 socket 注册到哪些 epoll:

```cpp
class EpollMapper {
public:
    explicit EpollMapper(int fd) : fd_(fd) {
        mutex_ = LockRegistry::LOCK_OPS.create(LT_EXCLUSIVE);   // :75 创建 mutex
    }
    ~EpollMapper() {}                                            // :78 ❌ 空析构!不 destroy mutex_
    void Add(int epoll_fd) { Locker sLock(mutex_); epoll_set_.insert(epoll_fd); }
    void Del(int epoll_fd) { Locker sLock(mutex_); epoll_set_.erase(epoll_fd); }
    int QueryFirst() { ... }
    void Clear() {}                                              // :102 ❌ 空!不清 epoll_set_/mutex_
private:
    const int fd_;
    u_mutex_t *mutex_ = nullptr;                                 // :106 裸指针
    std::unordered_set<int> epoll_set_;                          // :107 bucket 数组(本报告 104B)
};
```

`g_socket_epoll_mappers`(`ubsocket_event_epoll.cpp:25`)是**全局** `std::unordered_map<int, EpollMapper*>`,持 `EpollMapper*` 裸指针:

```cpp
std::unordered_map<int, EpollMapper *> g_socket_epoll_mappers{};   // :25 全局
```

`EpollMapper` 在 `CreateSocketEpollMapper`(`:38-54`)中 `new`(`:46`)并存入全局 map(`:50`)。`CleanSocketEpollMapper`(`:56-69`)是预期的 per-socket 清理:`erase` + `Clear()`(空)+ `delete mapper`。

## 3. 根因:全局 map 退出未清 + ~EpollMapper 空

**主因(全局 map 退出未清)**:`g_socket_epoll_mappers` 是全局静态,进程退出时 `~unordered_map` 析构 erases entries,但 `EpollMapper*` 值是**裸指针→不 delete**。`CleanSocketEpollMapper` 只在 per-socket close 路径调(若该 socket 未干净 close 则不调)。退出时大量 `EpollMapper*` 残留在 map → 不 delete → `~EpollMapper` 不执行 → `~unordered_set` 不执行 → bucket 数组(104B)泄露。

**次因(`~EpollMapper` 空)**:即便 `delete mapper` 被调,`~EpollMapper()`(`:78`)是**空体**——不 `LockRegistry::LOCK_OPS.destroy(mutex_)`。故 `mutex_`(48B,`:75` create)无论是否 delete 都泄露(应有独立报告)。`Clear()`(`:102`)同样空,不清 `epoll_set_`/`mutex_`(但 `delete mapper` 会触发成员自动析构 `~unordered_set` 释放 bucket,故 `delete` 仍能消 104B,只是消不了 mutex_)。

**两报告对应两条产生路径**:

| 报告 | 产生路径 | EpollMapper 状态 |
|------|---------|-----------------|
| 报告1(运行时) | `Socket::Connect`(`:1378`)→ `RegisterEvent`(`:138`)→ `ubsocket_epoll_ctl(ADD)` → `EpollCtl`(`:408`)→ `EpollMapper::Add(epoll_fd)` insert | socket 建链时创建/插入 epoll_fd,bucket 数组 104B 分配 |
| 报告2(退出) | `exit` → `StopAndJoinGlobalDispatchers` → `EventDispatcher::Stop`(`:106`)→ `ubsocket_epoll_ctl` → `EpollCtl` → `EpollMapper::Add` insert | 退出时 Stop 再 insert(可能同 EpollMapper 再 rehash 或不同 socket 的 EpollMapper) |

两条均因全局 map 退出未清导致 EpollMapper + bucket 数组泄露。

## 4. 为何 104B / 各 1 个

- libstdc++ `unordered_set<int>` 首次 insert → `_Prime_rehash_policy` 取最小素数 bucket 数 = 13 → `13 × 8B = 104B`。
- 各 1 个 = 两个不同 EpollMapper(或同 EpollMapper 两次 rehash 的不同 bucket 数组,但 rehash 会 free 旧的,故应是两个不同 EpollMapper)。退出时刻 2 个 EpollMapper 残留在全局 map → 2 × 104B。

## 5. 触发条件

- brpc socket 建链(`ubsocket_epoll_ctl ADD`)→ `CreateSocketEpollMapper` + `EpollMapper::Add` → EpollMapper 存入全局 map
- 进程退出未对每个 socket 调 `CleanSocketEpollMapper`(或全局 map 析构不 delete 值)→ EpollMapper + bucket 数组泄露
- 与 UB 配置无关,ubsocket 使能即存在

## 6. 与 `~AsyncEventPoll` EpollEvent 泄露的区别

| 维度 | 本泄露(EpollMapper bucket 104B) | AsyncEventPoll EpollEvent 40B |
|------|--------------------------------|-------------------------------|
| 对象 | `EpollMapper::epoll_set_` bucket 数组 | `AsyncEventPoll::socket_data_`/`removed_head_` EpollEvent* |
| 持有者 | 全局 `g_socket_epoll_mappers` map | `ArraySet<EventPoll>`(LeakySingleton)中的 AsyncEventPoll |
| 根因 | 全局 map 退出不 delete 值 + `~EpollMapper` 空 | `~AsyncEventPoll` 不清 socket_data_/removed_head_ |
| 修复点 | `ubsocket_uninit` 清全局 map + `~EpollMapper` destroy mutex | `~AsyncEventPoll` 清 socket_data_/removed_head_ |
| 文件 | `ubsocket_event_epoll.cpp/h`(EpollMapper + 全局 map) | `ubsocket_event_epoll.cpp`(AsyncEventPoll) |

两者同在 `ubsocket_event_epoll.*`,同属"析构清理缺失"家族,但对象/持有者/修复点不同,需分别修。

## 7. 修复方案

### 方案 1【必须】`ubsocket_uninit` 清理全局 map

`ubsocket.cpp:208-246` 的 `ubsocket_uninit` 中,在 `ArraySet<EventPoll>::ReleaseAll()` 附近补全局 map 清理:

```cpp
void ubsocket_uninit() {
    ...
    // 新增:清理 g_socket_epoll_mappers
    {
        WriteLocker sLock(g_socket_epoll_lock);
        for (auto &kv : g_socket_epoll_mappers) {
            if (kv.second != nullptr) {
                kv.second->Clear();        // 当前空(见方案2补)
                delete kv.second;          // → ~EpollMapper(当前空) → ~unordered_set 释放 104B bucket + ~mutex 不释放(见方案2)
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

顺序:在 `ArraySet<EventPoll>::ReleaseAll` 之前清 EpollMapper(EpollMapper 引用 epoll_fd,而 EventPoll 持 epoll_fd;先清 EpollMapper 再释放 EventPoll)。

### 方案 2【必须】`~EpollMapper` 补 destroy mutex

`ubsocket_event_epoll.h:78`:

```cpp
~EpollMapper() {
    if (mutex_ != nullptr) {
        LockRegistry::LOCK_OPS.destroy(mutex_);   // ← 新增:释放 mutex(48B,应有独立报告)
        mutex_ = nullptr;
    }
    // epoll_set_ 由成员自动析构 ~unordered_set 释放 bucket(104B)
}
```

`Clear()`(`:102`)也可补 `epoll_set_.clear();` 但 `delete mapper` 时成员自动析构已释放,非必须。

### 与伞形析构链 PR 的关系

本修复独立于 UmqSocket 析构链伞形(不同对象:EpollMapper 全局 map vs UmqSocket 子对象),但同属"析构清理缺失"批次,可一并 PR。与 `UBSOCKET-ASYNCEVENTPOLL-EPOLLEVENT-LEAK-ANALYSIS.ch.md`(`~AsyncEventPoll` 清 socket_data_)是同文件不同对象的清理缺失,建议合并修。

## 8. 验证

修复后 ASan 重跑:两条 104B 报告同时消失,且 `~EpollMapper` 的 mutex(48B)独立报告(若有)也消失。可用小 `thread_num` 短测验证。

## 9. 关键代码位置索引

| 位置 | 作用 | 泄露相关性 |
|------|------|-----------|
| `ubsocket_event_epoll.h:83` | `EpollMapper::Add` `epoll_set_.insert` | **本报告 104B 分配点** |
| `ubsocket_event_epoll.h:107` | `std::unordered_set<int> epoll_set_` 成员 | bucket 数组归属 |
| `ubsocket_event_epoll.h:71-108` | `EpollMapper` 类 | 泄露对象 |
| `ubsocket_event_epoll.h:75` | ctor `mutex_ = LOCK_OPS.create` | 次因 mutex(48B,~EpollMapper 不 destroy) |
| `ubsocket_event_epoll.h:78` | `~EpollMapper() {}` **空** | **不 destroy mutex_** |
| `ubsocket_event_epoll.h:102` | `Clear() {}` **空** | 不清 epoll_set_/mutex_ |
| `ubsocket_event_epoll.cpp:25` | `g_socket_epoll_mappers` 全局 map | 持 EpollMapper* 裸指针(根因) |
| `ubsocket_event_epoll.cpp:46` | `new EpollMapper` in `CreateSocketEpollMapper` | 分配点 |
| `ubsocket_event_epoll.cpp:56-69` | `CleanSocketEpollMapper`(per-socket 清理) | 退出未对全部 socket 调 |
| `ubsocket_event_epoll.cpp:408` | `EpollCtl` 调 `mapper->Add(epoll_fd_)` | frame #12 |
| `ubsocket.cpp:208-246` | `ubsocket_uninit` 不清全局 map | 根因(退出清理缺失) |

## 10. 与其他泄露的关系

| 维度 | 本泄露(EpollMapper 104B×2) | AsyncEventPoll EpollEvent 40B | UmqSocket 析构链家族 | TX Event | RX | RespClosure/done | bvar 家族 | RpcMeta |
|------|--------------------------|----------------------------|---------------------|----------|----|------------------|-----------|---------|
| 归属 | ubsocket core(EpollMapper 全局 map) | ubsocket core(AsyncEventPoll) | ubsocket core | ubsocket umq | ubsocket umq | ub_test | brpc | brpc/protobuf |
| 类别 | 全局 map 退出不 delete + ~EpollMapper 空 | ~AsyncEventPoll 不清 socket_data_ | 析构不 delete | 退出未释放 | buffer 不回流 | 闭包/drain | bthread 抛弃 | 解析逃逸 |
| 与 AsyncEventPoll 关系 | 同文件不同对象(独立修) | 自身 | 独立 | 独立 | 独立 | 独立 | 独立 | 独立 |
| ubs-comm 修复 | ✓(`ubsocket_uninit` 清 map + `~EpollMapper`) | ✓(`~AsyncEventPoll`) | ✓(伞形) | ✓ | ✓ | ❌ | ❌ | ❌ |

本泄露是 `g_socket_epoll_mappers` 全局 map 退出清理缺失 + `~EpollMapper` 空析构所致,与 `~AsyncEventPoll` EpollEvent 泄露同文件不同对象,需分别修。属 ubs-comm 析构清理缺失批次。

## 参考

- `UBSOCKET-ASYNCEVENTPOLL-EPOLLEVENT-LEAK-ANALYSIS.ch.md` — `~AsyncEventPoll` 不清 socket_data_/removed_head_(同文件不同对象,合并修)
- `UBSOCKET-UMQ-ACCEPTOR-*-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-CONNECTOR-*-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-DATAOPS-*-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-RXQUEUE-*-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-SOCKET-MUTEX-LEAK-ANALYSIS.ch.md` — UmqSocket 析构链家族(不同对象)
- `UBSOCKET-UMQ-RX-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-TX-EVENT-LEAK-ANALYSIS.ch.md` — RX/TX Event 泄露(不同类)
- 其他 ub_test/brpc 泄露文档
- 源码:`src/ubsocket/csrc/core/ubsocket_event_epoll.h:71-108`、`ubsocket_event_epoll.cpp:25-69,408`、`ubsocket.cpp:208-246`
