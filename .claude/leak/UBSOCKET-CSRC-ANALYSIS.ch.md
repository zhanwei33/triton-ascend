# UBSocket CSRC 详细代码分析

> 本文档对 `src/ubsocket/csrc/` 目录进行逐模块深度分析，覆盖架构、数据流、设计模式、已知问题。

## 目录

- [1. 总览](#1-总览)
- [2. 构建结构](#2-构建结构)
- [3. 入口文件](#3-入口文件)
- [4. common/ 通用基础设施](#4-common-通用基础设施)
- [5. core/ 通用抽象层(不含umq/urma)](#5-core-通用抽象层)
- [6. core/umq/ UMQ具体实现](#6-coreumq-umq具体实现)
- [7. core/urma/ URMA传输层](#7-coreurma-urma传输层)
- [8. under_api/ 动态加载层](#8-under_api-动态加载层)
- [9. iobuf/ 零拷贝内存管理](#9-iobuf-零拷贝内存管理)
- [10. profiling/ 性能追踪](#10-profiling-性能追踪)
- [11. cli/ CLI诊断工具](#11-cli-cli诊断工具)
- [12. 数据流全景](#12-数据流全景)
- [13. 设计模式汇总](#13-设计模式汇总)
- [14. 已知问题清单](#14-已知问题清单)

---

## 1. 总览

### 目录结构

```
src/ubsocket/csrc/
├── ubsocket.cpp              ← Init/Uninit入口(312行)
├── ubsocket_sock.cpp         ← 27个POSIX socket API(300行)
├── ubsocket_epoll.cpp        ← 5个epoll API(84行)
├── ubsocket_cntl.cpp         ← 空壳(12行,实现已移入sock.cpp)
├── ubsocket_struct_helper.h  ← u_init_options_t序列化(32行)
├── CMakeLists.txt            ← 6个OBJECT库→ubsocket_static/shared
├── common/                   ← 通用基础设施(25文件,~1500行)
├── core/                     ← 通用抽象层+umq/urma子目录(1143行通用+824行umq_socket等)
│   ├── ubsocket_socket.h/.cpp          ← SocketBase工厂(388行)
│   ├── ubsocket_data_tx.h/.cpp         ← DataTx通用写路径(226行)
│   ├── ubsocket_data_rx.h/.cpp         ← DataRx通用读路径(287行)
│   ├── ubsocket_core_types.h/.cpp      ← 核心类型枚举(207行)
│   ├── ubsocket_event_epoll.h/.cpp     ← EventPoll+EpollRunner(1143行)
│   ├── ubsocket_buf_converter.h        ← scatter-gather遍历抽象(109行)
│   ├── ubsocket_socket_acceptor.h/.cpp ← Acceptor+AcceptorOps虚接口(407行)
│   ├── ubsocket_socket_connector.h/.cpp← Connector+ConnectorOps虚接口(159行)
│   ├── ubsocket_socket_helper.h/.cpp   ← TCP工具类(531行)
│   ├── ubsocket_wakeup_event.h/.cpp    ← 异步Accept唤醒(218行)
│   ├── ubsocket_tx_cqe_poller.h/.cpp   ← TX CQE后台Poller(293行)
│   ├── umq/                   ← UMQ具体实现(33文件,~5000行)
│   └── urma/                  ← URMA空壳+包装类(10文件,~1400行)
├── under_api/                ← dlopen/dlsym封装(11文件,~4573行)
│   ├── dl_api.h/.cpp         ← DlApi编排器
│   ├── dl_libc_api.h/.cpp    ← LibcApi(54个函数指针)
│   ├── dl_umq_api.h/.cpp     ← UmqApi(57个函数指针,dlopen/adapter双模式)
│   ├── umq_api.h             ← UMQ类型聚合头文件
│   └── urma/                  ← UrmaApi(99个函数指针)
├── iobuf/                    ← 零拷贝内存(3文件,~800行)
├── profiling/                ← 性能追踪(26文件,~4000行)
│   ├── ubsocket_prof.h/.cpp  ← ProfilingTPId+C API入口
│   ├── impl/                 ← Tracer/Tracepoint/Combiner/Dumper(快速+扩展模式)
│   ├── trace/                ← SplitTrace三域双缓冲追踪
│   ├── statistics/           ← StatsMgr+Listener(UDS服务器)+PrintStatsMgr
│   └── probe/                ← ProbeManager主动探测
├── cli/                      ← CLI诊断工具(6文件,~1500行)
```

### 代码规模

| 子模块 | .cpp | .h | 总行数(约) |
|--------|------|-----|-----------|
| 入口(ubsocket.cpp等) | 4 | 1 | ~740 |
| common/ | 4 | 21 | ~1500 |
| core/通用 | 10 | 10 | ~3760 |
| core/umq/ | 14 | 19 | ~5000 |
| core/urma/ | 5 | 5 | ~1400 |
| under_api/ | 5 | 6 | ~4573 |
| iobuf/ | 1 | 2 | ~800 |
| profiling/ | 9 | 17 | ~4000 |
| cli/ | 3 | 3 | ~1500 |
| **总计** | **51** | **73** | **~18600** |

---

## 2. 构建结构

`csrc/CMakeLists.txt`(109行)定义6个OBJECT库，合并为`ubsocket_static`和`ubsocket_shared`(OUTPUT_NAME均为`ubsocket`):

| OBJECT库 | 来源 | 条件编译 |
|----------|------|----------|
| `common_objects` | `common/*.cpp` | 无 |
| `entry_objects` | `csrc/*.cpp` | 无 |
| `core_objects` | `core/*.cpp` + `core/umq/*.cpp` | `BUILD_URMA_DLOPEN_BACKEND`时加入`core/urma/*.cpp` |
| `under_api_objects` | `under_api/**/*.cpp` | 无 |
| `iobuf_objects` | `iobuf/*.cpp` | 无 |
| `profiling_objects` | `profiling/**/*.cpp` | 无 |

链接依赖:
- `BUILD_UMQ_ADAPTER_BACKEND`模式: 链接`umq`, `umq_buf`, `umq_ipc`(直接链接)
- `BUILD_UMQ_DLOPEN_BACKEND`模式: 仅链接`pthread`(UMQ通过dlopen加载)
- CLI: `UBSOCKET_BUILD_CLI`条件编译

---

## 3. 入口文件

### 3.1 ubsocket.cpp — Init/Uninit入口(312行)

**7个导出API**:

| API | 功能 |
|-----|------|
| `ubsocket_init_options` | 设置默认初始化参数 |
| `ubsocket_init` | 10步初始化(锁→DL→全局设置→UMQ→UMQ Backend→探针→统计→追踪→零拷贝→信号) |
| `ubsocket_uninit` | 反初始化(**已知不完整**:不重置`UBS_INITED`,不释放`g_zcopy_allocator`,不销毁`ArraySet`) |
| `ubsocket_version` | 返回版本字符串+打印完整版本到stdout |
| `ubsocket_set_logger` | 注册外部日志函数 |
| `ubsocket_set_log_level` | 设置日志级别 |
| `ubsocket_iobuf_allocate/deallocate` | 零拷贝内存分配/释放 |

**初始化流程**(`ubsocket_init`):
1. 参数校验+互斥锁保护
2. `GlobalSetting::VerifySetting()` — 校验配置,设置`UBS_NATIVE_TCP_MODE`
3. `DlApi::Load(LOAD_LIBC | LOAD_UMQ)` — 加载libc和UMQ函数指针
4. `LockRegistry::RegisterDefaultOps()` — 注册pthread默认锁操作+同步到UMQ
5. `UmqBackend::Init()` — UMQ子系统初始化(设备发现+主UMQ创建+Share-JFR预热)
6. `Profiling::Init()` — 性能追踪初始化
7. `GlobalStatsMgr`初始化 — CLI统计服务器
8. `ProbeManager::Start()` — 主动探测
9. `ZeroCopyPrepare()` — brpc零拷贝拦截
10. `ubsocket_handle_signal(SIGUSR2)` — 注册诊断信号

**关键陷阱**:
- 步骤编号混乱(注释中step3和step5各出现两次)
- `ubsocket_uninit`不完整:无法安全重新初始化
- `ZeroCopyPrepare`失败时设置`UBS_NATIVE_TCP_MODE=true`作为降级
- `UmqLogger`级别映射用`level % sizeof(OTHER_LEVEL)`(即`level % 3`) — 对高level值可能产生意外类别

### 3.2 ubsocket_sock.cpp — POSIX Socket API(300行)

**25个导出函数**,经`UB_API_WRAP`宏展开为`ubsocket_socket/close/connect/readv/writev`等。

**通用模式**:每个函数首先检查`UBS_NATIVE_TCP_MODE`,为true则透传`LibcApi`;否则进入UB逻辑。

**已知严重问题**:

| 问题 | 位置 | 严重度 |
|------|------|--------|
| `close()`潜在无限递归 | 行64 | **严重** — 非TCP模式调用`close(fd)`而非`LibcApi::close(fd)`,若符号替换为`ubsocket_close`则无限递归 |
| 13个桩函数返回0违反POSIX语义 | 行88等 | **严重** — `send/recv/read/write`等返回0而非-1+ENOSYS,上层可能误判为"连接关闭"或"0字节成功" |
| `fcntl/fcntl64/ioctl/sendmsg`无TCP守卫 | 行259-272 | **严重** — 即使TCP模式也返回0,设置`O_NONBLOCK`等静默失败 |
| `epoll_create1`返回fd=0 | ubsocket_epoll.cpp | **严重** — 返回0(stdin),应用使用fd=0作为epoll会灾难性错误 |

**socket()流程**: `domain==AF_SMC`时创建UB socket(eventfd+SocketBase::Create),其他domain透传libc。

**listen()降级**: `setsockopt(TCP_UB_SOCKET_HANDSHAKE)`失败时静默降级为TFO模式。

### 3.3 ubsocket_epoll.cpp — Epoll API(84行)

**5个导出函数**: `epoll_create/create1/ctl/wait/pwait`。

- `epoll_create`: 创建真实内核epoll fd,包装为`AsyncEventPoll`存入`ArraySet<EventPoll>`
- `epoll_create1`和`epoll_pwait`: **桩函数**,非TCP模式返回0

### 3.4 ubsocket_cntl.cpp — 空壳(12行)

仅含2个include,`fcntl/ioctl`实现实际在`ubsocket_sock.cpp`中。

---

## 4. common/ 通用基础设施

### 4.1 核心定义头文件

| 文件 | 行数 | 关键内容 |
|------|------|----------|
| `ubsocket_defines.h` | 210 | `Result=int32_t`, `UBS_API`, `ALWAYS_INLINE`, `RPC_ADPT_FD_MAX=8192`, `IO_SIZE_MB`, 各种阈值常量 |
| `ubsocket_errno.h` | 119 | `InnerCode`错误码枚举, bit30=可重试, bit29=可降级, `IsOk/IsRetryable/IsDegradable`查询函数 |
| `ubsocket_common_includes.h` | 41 | 聚合头文件(拉入defines/errno/functions/global_setting/lock/logger/obj_statistics/ref/scope_exit/set) |

### 4.2 GlobalSetting — 全局静态配置(265+320行)

35+静态成员,全部从环境变量加载:

| 关键设置 | 默认值 | 说明 |
|----------|--------|------|
| `UBS_NATIVE_TCP_MODE` | false | TCP透传模式 |
| `UBS_ENABLE_SHARE_JFR` | **true** | Share-JFR模式(关键陷阱) |
| `UBS_AUTO_FALLBACK_TCP` | true | 自动降级TCP |
| `UBS_EPOLL_ASYNC_THREAD_COUNT` | 1 | 异步epoll线程数 |
| `UBS_HAND_SHAKE_MODE` | UB_SOCK_OPT | 握手模式(TFO/sockopt) |

`LoadEnv()`读取30+环境变量,通过`Validator`校验。`VerifySetting()`设置TCP模式标志。

### 4.3 LeakySingleton — 泄漏式单例(68行)

```cpp
template<T> class LeakySingleton {
    static atomic<T*> m_instance;
    static once_flag m_flag;
    static T& Instance(); // call_once + new T,永不删除
};
```

解决静态析构顺序问题(后台线程在shutdown时仍访问单例)。代价:Valgrind报泄漏。

### 4.4 Ref<T> — 引用计数智能指针(214行)

| 类/宏 | 功能 |
|-------|------|
| `Referable` | 基类,`atomic<int16_t> ref_count_`,IncreaseRef/DecreaseRef |
| `Ref<T>` | 智能指针,构造+1,析构-1,=0时delete |
| `DECLARE_REF_COUNT_VARIABLE` | 在非Referable派生类中声明ref_count_ |
| `DEFINE_REF_OPERATION_FUNC` | 定义IncreaseRef/DecreaseRef方法 |
| `MakeRef<C,Args>` | `new (std::nothrow) C(args)` → Ref构造 |
| `RefConvert<Src,Des>` | `dynamic_cast<Des*>(src.Get())` → Ref构造 |

`ref_count_`使用`int16_t`(~32767并发引用),`DecreaseRef`用`fetch_sub(1,acq_rel)`,返回值==0时delete。

**已知问题**: `Ref<T>` move赋值使用`std::__exchange`(libc++内部符号,不可移植); `MakeRef`传参by value(C++11无完美转发)。

### 4.5 LockRegistry — 可插拔锁操作(155+286行)

| 类 | 功能 |
|----|------|
| `LockRegistry` | 存储`LOCK_OPS/RW_LOCK_OPS/SEM_OPS`静态ops结构体,可被外部注册覆盖(brpc的butex) |
| `Locker` | RAII scoped mutex,使用`LOCK_OPS.lock/unlock` |
| `ReadLocker/WriteLocker` | RAII scoped rwlock,使用`RW_LOCK_OPS.lock_read/lock_write/unlock_rw` |

注册时同步到UMQ: `umq_external_mutex_lock_ops_register()`。

默认ops使用pthread_mutex/pthread_rwlock/sem_t,通过`reinterpret_cast`转换opaque `u_mutex_t/u_rw_lock_t/u_semaphore_t`(void*)。

### 4.6 ExecutorService — 线程池(95+143行)

- Meyer's单例(`GetExecutorService()`)
- `UbsocketRingBuffer<Runnable>`作为任务队列(mutex保护)
- 工作线程用`condition_variable`等待,STOP任务逐线程发送
- `pthread_setname_np`命名工作线程
- Start()忙等线程启动(`sleep_for(1us)`循环)

### 4.7 SPSCRingQueue — 无锁SPSC环形队列(125行)

```cpp
template<T> class SPSCRingQueue {
    vector<T> buffer_;
    alignas(64) uint64_t write_index_/read_index_; // 非原子,仅单线程访问
    atomic<uint64_t> commit_write_/commit_read_;    // 两阶段提交
    const uint64_t capacity_/mask_;                 // 容量必须2^n
};
```

- 生产者:写`write_index_`→commit `commit_write_`(release)
- 消费者:读`read_index_`→commit `commit_read_`(release)
- `alignas(64)`防止false sharing
- `Size()`用`memory_order_acquire`读取两端

### 4.8 ArraySet<T> — fd索引原子数组(142行)

```cpp
template<T> class ArraySet {
    static constexpr uint32_t FD_CAPACITY_HARD_LIMIT = 65536;
    uint32_t capacity_;            // min(rlim_cur, 65536)
    unique_ptr<atomic<T*>[]> set_obj_;
};
```

- `GetItem(idx)` → `Ref<T>`(原子load+IncreaseRef)
- `OverrideItem(idx, new_item)` → atomic exchange + IncreaseRef新/DecreaseRef旧
- `RemoveItem(idx)` → exchange nullptr + DecreaseRef旧
- `ArraySet<Socket>`存储所有活跃socket; `ArraySet<EventPoll>`存储所有epoll

**`RPC_ADPT_FD_MAX=8192`与`FD_CAPACITY_HARD_LIMIT=65536`语义不同**:前者是编译时常量用于ProbeManager固定数组,后者是运行时上限。

### 4.9 其他基础设施

| 文件 | 功能 |
|------|------|
| `ubsocket_logger.h` | 双路径日志(外部回调/默认stdout), `UBS_LOG/UBS_SLOG_ERR`等宏, `UBS_ASSERT`不abort |
| `ubsocket_setting_validator.h` | 规则验证注册表(Int64Rule/FloatRule/StrEnumRule/StrNotEmptyRule) |
| `ubsocket_functions.h` | 工具箱(FloatEqual/StrTrim/BoolFromStr/Error2Str/SecureRandUInt32) |
| `ubsocket_scope_exit.h` | ScopeGuard(EBO优化,可deactivate) |
| `ubsocket_obj_statistics.h` | 对象计数器(编译时开关`OBJ_COUNTING_ENABLED`,SIGUSR2触发dump) |
| `ubsocket_signal_handler.h/.cpp` | SIGUSR2→ObjectStatistics::DumpStr() |
| `ubsocket_version.h` | 32位打包版本号(6:12:14),`NegotiateVersion/ValidateNegotiatedVersion`纯函数 |
| `ubsocket_ring_buffer.h` | Mutex保护的环形缓冲(用于线程池任务队列) |
| `ubsocket_qbuf_queue.h` | 动态扩缩容队列(2x扩/50%缩,25%/100%滞后带) |
| `ubsocket_fast_heap.h` | 页对齐二叉小堆(posix_memalign) |
| `ubsocket_profiling.h` | Profiling外观类(Init/Uninit/Combine/Reset) |

---

## 5. core/ 通用抽象层

### 5.1 核心类型(ubsocket_core_types.h, 207行)

| 枚举 | 类型 | 值 |
|------|------|-----|
| `SocketState` | uint8_t | INIT→RAW_ESTABLISHED→ESTABLISHED→SHUTDOWN→CLOSE |
| `SocketType` | enum class uint8_t | TCP, UMQ, SHM |
| `SocketCreateType` | uint8_t | UNKNOWN, LISTEN, CONNECT, ACCEPT |
| `EpollRunnerType` | enum class uint8_t | SHARE_JFR_RX_RUNNER, TRANSPORT_POOL_TX_RUNNER, TRANSPORT_POOL_EVENT_RUNNER |

**Socket基类**: 虚接口`GetTxFd/IsBindRemote/AddTxEvent/DelTxEvent/ShouldRegisterTxEvent/ProcessEpollEvent`,成员`raw_socket_/event_fd_/state_/type_/split_trace_/ref_count_`。

**辅助结构**: `ConnInfo`(peer_ip/peer_fd/type_fd/create_time), `AsyncAcceptInfo`(ready_queue+asyncTaskNum+lock)。

### 5.2 SocketBase工厂(ubsocket_socket.h/.cpp, 388行)

**工厂+策略模式**: `SocketBase::Create(fd, SOCK_TYPE_UMQ, outSocket)` 是唯一允许引用`umq/`头文件的通用层代码。

```
Create(fd, SOCK_TYPE_UMQ) → MakeRef<UmqSocket>(fd) → Initialize()
  → CreateAcceptorOps(SOCK_TYPE_UMQ) → new UmqAcceptorOps
  → CreateConnectorOps(SOCK_TYPE_UMQ) → new UmqConnectorOps
  → Acceptor(sock, acceptorOps) + Connector(sock, connectorOps)
```

`GenerateSocketCommOps()`创建`DataTxOps/DataRxOps`:
```
CreateTxOps(SOCK_TYPE_UMQ) → new UmqTxOps
CreateRxOps(SOCK_TYPE_UMQ) → new UmqRxOps
```

### 5.3 DataTx — 通用写路径(ubsocket_data_tx.h/.cpp, 226行)

**DataTxOps虚接口**:
| 方法 | 功能 |
|------|------|
| `BuildIovConverter` | 创建scatter-gather遍历器 |
| `BuildBufferConverter` | 创建连续缓冲遍历器 |
| `AllocTxBuf` | 分配TX协议缓冲 |
| `PostSend` | 提交发送请求 |
| `PollTx` | 轮询TX完成 |
| `FlushTx` | 刷新未发送数据 |
| `Writable` | 检查可写性 |
| `WakeUpTx` | 唤醒流控等待 |

**DataTx::WriteV核心流程**:
1. `SOCK_STAT_RAW_ESTABLISHED` → 透传`LibcApi::writev()`
2. 检查`Writable(sock)`,不可写返回0+EAGAIN
3. `PollTx(sock)` — 清理TX完成
4. `BuildIovConverter(iov, iovcnt)` — 创建遍历器
5. **数据分段循环**: 用`IndexMove()`将用户数据切分为`IOBufSize()`大小的块,累积`buf_cnt`和`batch`计数
6. `AllocTxBuf(0, buf_cnt)` — 分配协议缓冲
7. `PostSend(sock, txBuf, batch, converterPtr)` — 提交到传输层
8. 返回`tx_total_len`

### 5.4 DataRx — 通用读路径(ubsocket_data_rx.h/.cpp, 287行)

**DataRxOps虚接口**:
| 方法 | 功能 |
|------|------|
| `PollRx` | 轮询RX完成,填充`block_cache_` |
| `RearmRxInterrupt` | 重置RX中断 |
| `FlushRx` | 刷新RX数据 |

**DataRxOps::RxDataSet共享算法**(模板方法模式):
1. `PtrFloorToBoundary(buf)` → 获取Block*
2. `block_cache_.CutAndInsertAfter(size, block)` — 从缓存剪切数据到用户block链
3. 0字节时:检查epoll_event_num_→CAS→设置poll_=true+errno=EINTR(重试信号);`SOCK_STAT_CLOSE`→返回0;`flow_control_failed_`→EIO;否则EAGAIN
4. 有数据时:设`out_first_block->size = out_first_block->cap`(标记block已满,防止brpc回收)

**OutputErrorMagicNumber**: 协议协商错误数据缓冲→输出完后转为`RAW_ESTABLISHED`(TCP降级)。

### 5.5 EventPoll + EpollRunner(ubsocket_event_epoll.h/.cpp, 1143行)

**最复杂的文件**。核心组件:

| 类 | 角色 |
|----|------|
| `EpollRunnerOps` | 虚接口:ProcessOneEvent/AddEventToRunner |
| `EpollRunnerBase` | 抽象基:Start/Stop/AddEpollEvent/DelEpollEvent/ProcessOneEvent |
| `EpollRunner<T>` | LeakySingleton模板+后台poller线程,CRTP-like |
| `EpollRunnerFactory` | 静态工厂,按`EpollRunnerType`分发到3个`EpollRunner<T>::Instance()` |
| `EventPoll` | 抽象基:EpollCtl/EpollWait/WakeUpEpollFd |
| `AsyncEventPoll` | 具体实现,拦截内核epoll+SPSC队列 |

**AsyncEventPoll关键设计**:
- 包装真实内核epoll_fd(不替换内核epoll)
- `readable_sockets_event_queue_`(SPSCRingQueue,容量65536) — UB socket可读事件通过此队列注入
- `sock_readable_fd_`(eventfd) — "socket有可读数据"通知机制
- `socket_data_`(unordered_map<int, EpollEvent*>) — fd到事件的映射
- `removed_head_` — 延迟删除链表(避免epoll_wait期间删除事件)
- 自定义`aligned new/delete` — cache-line对齐(SPSCRingQueue要求)

**EpollCtl(ADD)流程**:
1. 创建`EpollMapper` for fd
2. `AddRawSocketEvent` — 注册到真实内核epoll
3. `AddSockReadableEvent` — 惰性创建eventfd通知
4. 如`ShouldRegisterTxEvent()`: `AddProtoTxEvent` — 注册TX中断fd

**EpollWait流程**:
1. 先从SPSC队列弹出可读事件
2. 调用真实`epoll_wait`
3. `ArrangeWakeUpEvents`分发:RAW_SOCKET→透传;UB_SOCKET_IN→SPSC队列;UB_SOCKET_OUT→ProcessEpollEvent;wakeup_callback→异步accept

### 5.6 Acceptor + Connector(407+159行)

**AcceptorOps虚接口**:
| 方法 | 阶段 | 功能 |
|------|------|------|
| `PrepareConnect` | Phase 0 | TCP连接建立 |
| `Negotiate` | Phase 1 | 版本协商 |
| `CreateSocketResources` | Phase 2 | 创建UMQ资源 |
| `DestroySocketResources` | Phase 3 | 失败清理 |
| `ValidateProtocol` | — | 检测UB连接 |

**Acceptor::Accept双模式**:
- **异步模式**: `LibcApi::accept()`→提交`ProcessUBConnection`到线程池→返回-1+EAGAIN(下次从`TryPopAsyncReadyFd`取完成fd)
- **同步模式**: `LibcApi::accept()`→`ProcessUBConnection`内联→返回fd

**DoAccept流程**: eventfd → SocketBase::Create → Negotiate → CreateSocketResources → OverrideItem注册 → StatsMgr更新

**Connector::Connect**: 同样的Phase 0-3流程,客户端侧。失败时:可降级→TCP透传;不可降级→fatal error。

### 5.7 SocketConnHelper — TCP工具类(531行)

纯静态工具类:
- `IsBlocking/SetBlocking/SetNonBlocking` — fcntl操作
- `SendSocketData/RecvSocketData` — 带超时的TCP收发(poll+EAGAIN重试)
- `FlushSocketMsg` — 清空fd残留数据
- `IsUbsConnection` — 检测UB握手(TFO:TCP_INFO.tcpi_options;sockopt:TCP_UB_SOCKET_HANDSHAKE)
- `SendLengthPrefixed/RecvLengthPrefixed` — [4B len][body]线格式,跨版本兼容
- `GetCurrentProcessSocketId` — NUMA socket ID(sched_getcpu+sysfs)
- `ExtractIpFromSockAddr/ExtractPortFromSockAddr` — IPv4/IPv6地址提取

### 5.8 TxCqePoller — TX CQE后台Poller(293行)

- LeakySingleton,后台线程每100ms轮询所有注册socket的TX CQE
- `timerfd`提供周期性,`epoll+eventfd`提供响应式shutdown
- **SplitTrace.SuppressTrace()=true** — poller线程不写SplitTrace(防止trace爆炸,陷阱#7)
- `AddSocket/DelSocket`: mutex保护的vector操作(线性搜索去重)

---

## 6. core/umq/ UMQ具体实现

### 6.1 UmqSocket(824行)

**继承**: `SocketBase`, `UmqSocketSeq`(=`UmqSocketBoundedSequence<24,uint32_t,(1<<24)-2>`)

**关键成员**:
| 成员 | 类型 | 说明 |
|------|------|------|
| `umq_handle_` | uint64_t | 子UMQ handle(INVALID_HANDLE=未创建) |
| `share_umq_handle_` | uint64_t | 主UMQ handle(Share-JFR模式) |
| `rxQueue` | UmqBufferReceiveQueue* | Share-JFR模式接收队列 |
| `jetty_alloc_state` | JettyAllocState | IDLE/WAITING/READY状态机 |
| `trans_mode_` | ub_trans_mode | 协商传输模式(RM_TP默认) |
| `negotiated_version_` | uint32_t | 版本协商结果 |

**CreateLocalUmq核心流程**:
- 无Share-JFR: `UmqApi::umq_create()`直接创建
- 有Share-JFR: `GetOrCreateMainUmq()`先获取主UMQ,然后`umq_create(UMQ_CREATE_FLAG_SHARE_RQ | UMQ_CREATE_FLAG_SUB_UMQ)`创建子UMQ
- SINGLE类型: 设置TX中断+solicited模式
- POOL类型: 仅设置`UMQ_CREATE_FLAG_SHARE_RQ`

**虚方法覆写**: `Initialize()→UBS_OK`, `IsBindRemote()→umq_is_bind_remote_`, `AddTxEvent/DelTxEvent`→TX中断fd的epoll注册/注销, `ShouldRegisterTxEvent()→SINGLE类型才true`

### 6.2 UmqBackend(530行)

全静态生命周期类:
- `Init()` — 10步: 设置→配置→umq_init→AddUbDev→CPU亲和→UMQ perf→Share-JFR主UMQ→预热→`UMQ_INITED=true`
- `AddUbDev()` → `FindDevName()`(偏好"bonding_dev_0") + `FindDevEid()` + `umq_dev_add()`
- `CreateShareMainUmq()` — `UMQ_CREATE_FLAG_MAIN_UMQ`创建主UMQ
- `PrefillShareMainUmq()` — `UmqConnHelper::PrefillRx()`预填充RX缓冲
- `InitShareJfrMonitering()` — 启动SHARE_JFR_RX_RUNNER epoll

**UmqZeroCopyAllocator**: `allocate→umq_buf_alloc→buf->buf_data`; `deallocate→umq_data_to_head→umq_buf_free`

**已知问题**: `UMQ_INITED`是static bool,不能安全重置和重新初始化。

### 6.3 UmqErrnoConverter(449行) — **冻结,不可修改**

**7个UmqOperation枚举**: CONNECT, ACCEPT, WRITEV, READV, CREATE, BIND_INFO_GET, GET_STATE

**4条映射路径**:
| 路径 | 适用Operation | 机制 |
|------|---------------|------|
| Convert(统一表+override) | CONNECT/ACCEPT/WRITEV/READV | `kCommonErrnoMappings`(15条) + `ShouldOverrideWithSavedErrno` |
| ConvertBufStatus(方向区分) | CONNECT/ACCEPT→EIO全部; WRITEV→24条; READV→24条 | BufStatus→errno映射 |
| Convert(GET_STATE) | GET_STATE | ERR/MAX→EIO, else→0(无表查找) |
| ConvertHandleResult(有限透传) | CREATE→EINVAL/EPERM; BIND_INFO_GET→ENOMEM/EINVAL | handle result映射 |

### 6.4 UmqTxOps(617行)

**继承**: `DataTxOps`

**关键成员**: `local_umqh_`(子UMQ handle), `head_buf_/tail_buf_`(未信号WR链表), `unsolicited_bytes_/unsolicited_wr_num_/unsignaled_wr_num_`(计数器)

**PostSend核心流程**(最关键TX方法):
1. 遍历batch缓冲,应用converter `MemCopy()`
2. 设置`UMQ_OPC_SEND_IMM`opcode,分配序列号`FetchAddSeqNum()`
3. solicited_enable: 最后WR或阈值超过时设置
4. complete_enable: `unsignaled_wr_num_ >= TX_REPORT_THRESHOLD`时设置,保存`user_ctx`
5. `UmqApi::umq_post()`提交
6. 成功: `tx_queue_avail_num_ -= batch`
7. 部分失败(bad_qbuf): 恢复计数器+释放bad缓冲+HandleBadQBuf
8. EAGAIN/ENOBUFS/EMLINK: 入队`UmqTpWaitQueue`等待重试

**DpRearmTxInterrupt陷阱**: ret==0(成功)设置errno=EAGAIN返回-1,**不调用Convert**(仅失败路径走Convert)。

**数据流(写)**:
```
ubsocket_writev → SocketBase::WriteV → UmqTxOps::AllocTxBuf[umq_buf_alloc]
  → UmqTxOps::PostSend[converter memcpy + Block IncRef + umq_post]
  → UmqTxOps::PollTx[umq_poll CQE + Block DecRef + umq_buf_free]
```

### 6.5 UmqRxOps(470行)

**继承**: `DataRxOps`

**PollRx核心流程**:
- 无Share-JFR: 先ack RX epoll event
- `poll_`标志: 调用`GetQbuf()`轮询缓冲
- 每个缓冲: 检查opcode→处理probe→检查status
- 错误缓冲: FC_ERR/FC_MSG/FC_UPDATE/EMLINK
- 成功缓冲: `block_cache_.Insert(buf_data, data_size)`
- `poll_=false`如果poll_num==0(聚合优化)

**GetQbuf路径**:
- Share-JFR: `umqSock->GetAndPopQbuf()`(从UmqBufferReceiveQueue)
- 无Share-JFR: `UmqPollAndRefillRx()`(umq_poll + 缓冲补充)

**数据流(读)**:
```
ubsocket_readv → SocketBase::ReadV → UmqRxOps::PollRx[umq_poll或GetAndPopQbuf]
  → block_cache_.Insert → SocketBase::ReadV从block_cache_读取
```

### 6.6 UmqConnectorOps(1134行) — 最复杂的连接器

**PrepareConnect** — TCP连接建立:
- UB_SOCK_OPT模式: `setsockopt(TCP_UB_SOCKET_HANDSHAKE)`,失败降级TFO
- TFO模式: `BuildNegotiateReqBuffer()` + `sendto(MSG_FASTOPEN)`(negotiate数据嵌入SYN)
- 纯TCP: `LibcApi::connect()`

**Negotiate(ConnectNegotiate)**:
- 发送/接收4B版本 + length-prefixed body
- Major mismatch → 可降级到TCP
- 接收NegotiateRsp: peer_eid, peer_trans_mode, socket_ids
- `DoRoute()` → 路由选择(FULLMESH: GetConnEid; CLOS: GetCpuAffinityUmqRoute)

**CreateSocketResources** — 状态机循环:
- kSTART: CheckRouteDevAdd → DoUbConnect → send/recv ack → 判断retry/degrade/fail
- kRETRY: Unbind+Destroy → CheckOtherRoute → DoUbConnectRetry
- kDEGRADE: OverrideItem(nullptr)(降级TCP)
- kFAILED: 返回错误

**DoUbConnect**: CreateLocalUmq → GenerateSocketCommOps → umq_bind_info_get → send CpMsg → recv CpMsg → umq_bind → EnsurePrefilled → UpdateRxQueueAvailNum

### 6.7 UmqAcceptorOps(655行)

**镜像ConnectorOps**的服务端版本:
- `PrepareConnect()` → 返回0(服务端无需TCP连接)
- `Negotiate(AcceptNegotiate)` → ValidateVersion + recv NegotiateReq + send NegotiateRsp + recv NegotiateRoute
- `ValidateVersion`: Major mismatch时发送`UBS_PROTOCOL_VERSION`(非硬编码0!陷阱)
- **TFO残留数据陷阱**: Major mismatch时必须消费wire上`body_len`字节残留(否则后续brpc读到垃圾)

### 6.8 UmqBufConverter(72行)

| 类 | MemCopy行为 |
|----|------------|
| `UmqIovConverter` | **零拷贝**: 直接将umq_buf的buf_data指向iov内存(Block IncRef保活) |
| `UmqBufferConverter` | **拷贝**: `memcpy()`从用户缓冲到umq_buf数据区 |

### 6.9 EidRegistry + UmqEidTable + RouteListRegistry(336行)

| 类 | 单例模式 | 功能 |
|----|---------|------|
| `UmqEidTable` | Meyer's | 主UMQ状态管理(umq_eid→MainUmqState vector) |
| `MainUmqState` | — | enable_shared_from_this, EnsurePrefilled双检锁 |
| `EidRegistry` | LeakySingleton | 设备EID注册/注销(测试中需UnregisterEid清理) |
| `RouteListRegistry` | LeakySingleton | 路由列表缓存(peer EID→route_list) |

### 6.10 Share-JFR EpollRunner(376行)

**ProcessShareJfrEvent核心流程**(Share-JFR RX路径):
1. `ProcessMainUmqRearm()` — ack + rearm主UMQ RX中断
2. `umq_poll(main_umq)` — 获取所有子UMQ RX缓冲
3. 分离FC缓冲与IO缓冲
4. 分配新RX缓冲→`umq_buf_alloc()`→`umq_post()`回填主UMQ
5. `SiftSocketEventsWithUmqBuffers()` — 每个缓冲: `buf_pro->umq_ctx`(=raw_socket_)→查找socket→`AddQbuf()`→`NewRxEpollIn()`→`AsyncEventPoll::AddReadableEvent()`

### 6.11 其他umq文件

| 文件 | 功能 |
|------|------|
| `umq_transport_pool.h/.cpp` | 传输池管理(POOL模式:umq_transport_pool_resource_create/destroy) |
| `umq_setting.h/.cpp` | UMQ设置(UMQ_DEV_NAME/TRANS_MODE/TP_TYPE/BLOCK_SIZE等) |
| `umq_conn_helper.h/.cpp` | 连接辅助(GetDevEid/PrefillRx/GetRouteList/RegisterSharedJfrForRead) |
| `umq_tx_helper.h/.cpp` | TX CQE处理(PollUmqTx模板方法→PollUmqTxInternal→ProcessTxCqe) |
| `umq_buffer_receive_queue.h/.cpp` | 接收队列(FIFO或有序RM_CTP模式,Out-of-Order heap重排) |
| `umq_bounded_seq.h` | 循环序列号算术(24-bit,Mask/Normalize/CompareLessInCircularOrder) |
| `umq_qbuf_list.h` | umq_buf_t链表宏(QBUF_LIST_FIRST/INSERT_HEAD/REMOVE等) |
| `umq_tp_wait_queue.h/.cpp` | 流控等待队列(LeakySingleton+SPSCRingQueue, IDLE→WAITING→READY) |
| `umq_tp_tx_epoll_runner_ops` | TP TX epoll runner(CQE错误→shutdown+RebuildTp) |
| `umq_tp_event_epoll_runner_ops` | TP event runner(eventfd→WakeUp等待socket) |

---

## 7. core/urma/ URMA传输层

| 文件 | 功能 | 状态 |
|------|------|------|
| `urma_socket.h/.cpp` | UrmaSocket(继承SocketBase) | **空壳类**,无成员/方法覆写 |
| `urma_wrapper.h/.cpp` | UrmaDevice/Context/Jfc/Jfs/Jfr/Jetty包装类(804行) | **完整实现**,Referable+Ref<T> |
| `urma_backend.h/.cpp` | Urma::Init/UnInit(加载UrmaApi+urma_init) | 基础可用 |
| `urma_setting.h/.cpp` | UrmaSetting(UB_DEV_NAME/UB_DEV_EID) | **占位符**,Init()和LoadEnv()均为空 |
| `urma_socket_types.h` | TransportType(TPT_CTP/RTP)/TransportMode(TPM_RC/RM)枚举 | 定义完成 |

UrmaWrapper实现了完整的URMA对象层次(Device→Context→Jfc→Jfs→Jfr→Jetty),但UrmaSocket未实现任何Ops接口,无法通过工厂创建。

---

## 8. under_api/ 动态加载层

### 8.1 DlApi编排器(dl_api.h/.cpp, 140行)

**3个加载标志**: `LOAD_LIBC(1<<0)`, `LOAD_UMQ(1<<1)`, `LOAD_URMA(1<<2)`

- `Load(libraries)` — 按位掩码顺序加载(libc→UMQ→URMA),任一失败goto ERROR卸载全部
- `UnLoad(libraries)` — **忽略位掩码参数**,总是卸载全部(已知问题)
- `LoadSym(handle, name)` — `dlsym()`+失败时`dlclose(handle)`

**宏体系**: `DL_API_DECLARE`/`DL_API_DEFINE`/`DL_API_SET_NULL`/`DL_API_LOAD` — 消除50-100个符号的声明/定义/加载样板代码

### 8.2 LibcApi(dl_libc_api.h/.cpp, 530行)

**54个函数指针**: socket/close/connect/accept/bind/listen/readv/writev/epoll_create/epoll_ctl等

- `dlopen("libc.so.6", RTLD_NOW)` — 加载后立即`dlclose`(libc始终驻留)
- variadic处理: `open/fcntl/ioctl`提取1个variadic参数(va_list),仅支持单参数
- `Load()` — 54个`DL_API_LOAD`全部加载(完整)

### 8.3 UmqApi(dl_umq_api.h/.cpp, 959行)

**双模式编译**:

| 模式 | 编译标志 | 实现方式 |
|------|----------|----------|
| dlopen | `UMQ_DLOPEN_BACKEND_ENABLED` | 57个`_ptr`函数指针,`dlopen("libumq.so",RTLD_NOW|RTLD_NODELETE|RTLD_GLOBAL)` |
| adapter | `UMQ_ADAPTER_BACKEND_ENABLED`(else) | 无`_ptr`,直接调用`::umq_xxx()`全局C函数 |

**已知严重问题**:
| 问题 | 位置 | 严重度 |
|------|------|--------|
| `umq_transport_pool_resource_create`无限递归 | dl_umq_api.h:335 | **严重** — dlopen分支调用自身而非`_ptr` |
| 5+个UMQ API未DL_API_LOAD | dl_umq_api.cpp | **严重** — stats_perf/trace/transport_pool/interrupt_fd_list的_ptr保持nullptr→segfault |
| umq_post_api/umq_poll_api签名不匹配 | dl_umq_api.h:64-65 | **高** — using类型用`umq_io_direction_t`,wrapper用`umq_io_option_t*` |

### 8.4 UrmaApi(dl_urma_api.h/.cpp, 1433行)

**99个函数指针**,覆盖URMA完整API(Device/Context/Jfc/Jfs/Jfr/Jetty/Seg/DataTransfer/Polling/Log/Network/TP)。

- `dlopen("liburma.so", RTLD_NOW|RTLD_NODELETE|RTLD_GLOBAL)`
- 99个`DL_API_LOAD`全部加载(完整,无UmqApi的缺失问题)
- 始终dlopen模式(无双模式分支)

**urma_types.h**(1442行): URMA类型系统(EID/Device/Context/Jfc/Jfs/Jfr/Jetty/CR/Seg/NetAddr等)。
**urma_opcode.h**(268行): URMA操作码(WRITE/READ/CAS/SEND等)和状态码。

---

## 9. iobuf/ 零拷贝内存管理

### 9.1 Block + BlockRef + BlockCache(ubsocket_iobuf.h, 233行)

**Block结构体**: brpc兼容iobuf block
- `atomic<int16_t> nshared` — 引用计数
- `char *data` / `uint32_t size/cap/flags` — 数据区(flags=`IOBUF_BLOCK_FLAGS_UB`)
- `IncRef()` / `DecRef()` — fetch_add/fetch_sub + acquire-release fence, nshared→0时`blockmem_deallocate_zero_copy(this)`
- `SetNext/GetNext` — 链表操作

**BlockRef结构体**: Block的部分引用(offset + length)

**BlockCache类**: Block链表缓存
- `Insert(data_in, data_size)` — placement new在`data_in - sizeof(Block)`位置构造Block头
- `CutAndInsertAfter(cut_size, block)` — 从缓存剪切请求字节到用户block链
- `partial_block_`(BlockRef) — 跟踪部分消耗的block(IncRef保活,nshared=2)

### 9.2 ZcopyAdapter(ubsocket_zcopy_adapter.h/.cpp, 563行)

**核心机制**: 拦截brpc的`blockmem_allocate/deallocate`函数指针,替换为`blockmem_allocate/deallocate_zero_copy`,通过`g_zcopy_allocator`路由到UMQ零拷贝分配器。

**DynSymScanner**: ELF符号扫描器
- 读取`/proc/self/maps` + `/proc/self/exe`
- 解析ELF `.symtab/.strtab`节头定位brpc的mangled符号
- 过滤ASAN/GCOV变体
- ET_DYN(PIE): base_addr_偏移; ET_EXEC: 直接st_value

**UbsZcopyAdapter**: 三阶段拦截
1. `dlsym(RTLD_DEFAULT)`尝试默认mangled名
2. 回退`DynSymScanner` ELF扫描
3. `RecordAndSetBrpcAllocator()`替换函数指针

---

## 10. profiling/ 性能追踪

### 10.1 ProfilingTPId + C API入口(ubsocket_prof.h/.cpp, 299行)

**38个tracepoint ID**: `CORE_CONNECT=0`到`UBSOCKET_PROF_COUNT`(终止符)

**双模式profiling**:
- **fast模式**: Tracer(thread_local TraceGroup, 每线程独立统计)
- **ext模式**: TracerExt(per-CPU TraceGroupExt agents, reservoir采样+p50/p90/p95/p99/p999百分位)

**C API**: `ubsocket_prof_init/record/combind/reset`, `PROF_START/PROF_END`宏

### 10.2 Tracepoint + TracepointExt

| 类 | 统计 | 百分位 |
|----|------|--------|
| `Tracepoint` | success/failure计数+total/min/max time | 仅pp90(未实现) |
| `TracepointExt` | 同上+reservoir[1024]采样 | p50/p90/p95/p99/p999(线性插值计算) |

`TracepointExt.RecordExt`使用reservoir采样: total<RESERVOIR_SIZE直接插入; 满时概率替换(`fast_rand_ext()`线程局部LCG)。

### 10.3 SplitTrace — 三域双缓冲追踪(ubsocket_trace.h/.cpp, 773行)

**SplitTrace类**: 3个域(write/read/epoll),每个域2个TraceBuffer,atomic active_idx切换。

**SplitTraceInfo结构体**: raw_socket/peer_socket/rpc_id/seq_no/data_size/offset/type/poll_num/tid/start_timestamp/end_timestamp

**关键方法**:
- `AddWriteTrace/AddReadTrace/AddEpollTrace` — 多个重载(简单/详细/带时间)
- `UpdateWriteFirstTrace` — **前向扫描回填**:从found位置向前扫描,对`seq_no==0`条目回填seq_no/data_size/offset,遇到已填条目停止(修复"硬编码相邻位"陷阱)
- `TrySwap/TrySwapEpoll` — freeze+swap双缓冲
- `SuppressTrace()` — thread_local开关,poller线程设为true跳过trace(陷阱#7)

**TracePrintThread**: 后台线程定期`DrainAllSockets`(遍历ArraySet<Socket>→Flush每个socket的SplitTrace)。

### 10.4 ProbeManager — 主动探测(577行)

**ProbeTimeInfo**(packed): 8个时间戳字段(client_send/client_recv/umq_client_send/umq_client_recv/server_recv/server_rsp/umq_server_recv/umq_server_rsp)

**机制**: 注册`UmqPerfCallback`→UMQ处理probe缓冲(user_data==PROBE_USER_DATA_ID)时更新时间戳→client计算RTT=clientDelta-serverDelta

**PeriodicProbe**: `sem_timedwait`调度,超时发client probe;sem_post唤醒处理server response。

### 10.5 Statistics + CLI Server(statistics.h, 1000行)

**Listener类**: Unix域套接字服务器(abstract UDS `ubscli-{pid}`)
- epoll+eventfd事件循环
- 9种CLI命令: STAT/TOPO/DELAY/FC/QBUF_POOL/UMQ_INFO/IO/UMQ/PROBE

**StatsMgr**: 8个原子计数器(CONN_COUNT/ACTIVE_CONN_COUNT/RxPacket/TxPacket/RxByte/TxByte/Error/Lost) + Recorder数组

**PrintStatsMgr**: 后台线程定期JSON输出到`/tmp/ubsocket/log/ubsocket_kpi.json`

---

## 11. cli/ CLI诊断工具

| 文件 | 功能 |
|------|------|
| `cli_main.cpp` | `ubstat`二进制入口,解析args→CLIClient→TerminalDisplay |
| `cli_client.h/.cpp` | UDS客户端,连接→send header→recv header+payload |
| `cli_args_parser.h/.cpp` | getopt_long参数解析(pid/command/watch/srceid/dsteid/type/enable/value) |
| `cli_terminal_display.h/.cpp` | ANSI彩色终端输出(9种数据类型的表格渲染) |

---

## 12. 数据流全景

### 写路径(POSIX writev → UMQ)
```
ubsocket_writev(fd, iov, iovcnt)
  [UBS_NATIVE_TCP_MODE?] → LibcApi::writev()
  [否] → ArraySet<Socket>::GetItem(fd) → SocketPtr
    → SocketBase::WriteV(sock, iov, iovcnt)
      [RAW_ESTABLISHED?] → LibcApi::writev()
      [否] → DataTx::WriteV()
        → Writable(sock) [JettyAllocState检查]
        → PollTx(sock) [清理TX CQE]
        → BuildIovConverter(iov) → UmqIovConverter(零拷贝)
        → 数据分段循环(IndexMove + AllocTxBuf + PostSend)
          → UmqTxOps::AllocTxBuf [umq_buf_alloc]
          → UmqTxOps::PostSend [MemCopy + Block IncRef + umq_post]
            [EAGAIN?] → UmqTpWaitQueue::Enqueue
          → 返回tx_total_len
```

### 读路径(非Share-JFR)
```
ubsocket_readv(fd, iov, iovcnt)
  → SocketBase::ReadV(sock, iov, iovcnt)
    → DataRx::ReadV()
      → OutputErrorMagicNumber [协议协商错误缓冲]
      → UmqRxOps::PollRx(sock)
        → GetAndAckEvent [umq_get_cq_event + umq_ack_interrupt]
        → GetQbuf → UmqPollAndRefillRx [umq_poll + 缓冲补充]
        → block_cache_.Insert(buf_data, data_size)
      → RxDataSet(iov[0].iov_base, max_buf_size)
        → PtrFloorToBoundary → Block*
        → block_cache_.CutAndInsertAfter → 数据复制到用户缓冲
      → 返回rx_total_len
```

### 读路径(Share-JFR模式)
```
[后台线程] UmqShareJfrEpollRunnerOps::ProcessShareJfrEvent(main_umq)
  → ProcessMainUmqRearm [umq_get_cq_event + umq_ack_interrupt + umq_rearm_interrupt]
  → umq_poll(main_umq) [所有子UMQ RX缓冲在主JFR上]
  → 分离FC/IO缓冲 → 分配新RX缓冲回填
  → SiftSocketEventsWithUmqBuffers
    → buf_pro->umq_ctx(=raw_socket_) → 查找socket
    → UmqSocket::AddQbuf [UmqBufferReceiveQueue]
    → NewRxEpollIn + AsyncEventPoll::AddReadableEvent [唤醒用户epoll_wait]

[用户线程] epoll_wait → readv
  → UmqRxOps::PollRx → GetAndPopQbuf [从UmqBufferReceiveQueue出队]
  → block_cache_.Insert → RxDataSet → 返回数据
```

### 连接路径(客户端)
```
ubsocket_connect(fd, addr)
  → UmqConnectorOps::PrepareConnect [TCP: TFO/sockopt/plain]
  → UmqConnectorOps::Negotiate [ConnectNegotiate: 版本+路由协商]
  → UmqConnectorOps::CreateSocketResources
    [kSTART] → DoUbConnect
      → CreateLocalUmq [umq_create]
      → GenerateSocketCommOps [创建Tx/Rx Ops]
      → umq_bind_info_get → send CpMsg → recv CpMsg → umq_bind
      → EnsurePrefilled (Share-JFR) → UpdateRxQueueAvailNum
    [kRETRY] → Unbind+Destroy → CheckOtherRoute → DoUbConnectRetry
    [kDEGRADE] → TCP降级
```

### 连接路径(服务端/Accept)
```
ubsocket_accept(listen_fd)
  → Acceptor::Accept [同步/异步模式]
  → ValidateProtocol [8B magic检测UB连接]
  → UmqAcceptorOps::Negotiate [AcceptNegotiate: 版本+路由]
  → UmqAcceptorOps::CreateSocketResources
    → DoUbAccept [镜像DoUbConnect]
```

---

## 13. 设计模式汇总

| 模式 | 应用位置 | 说明 |
|------|----------|------|
| **工厂+策略** | SocketBase::Create, CreateXxxOps | 按SocketType分发到具体Ops实现 |
| **虚接口+具体实现** | DataTxOps/UmqTxOps等6组 | 通用层定义接口,umq/覆写 |
| **依赖方向隔离** | core/ubsocket_*禁止引用umq/头文件 | 仅ubsocket_socket.cpp工厂可引用 |
| **LeakySingleton** | EidRegistry, RouteListRegistry, EpollRunner<T>, TxCqePoller | 永不删除,避免shutdown顺序问题 |
| **Meyer's Singleton** | Logger, Validator, UmqEidTable, ExecutorService, ArraySet | local static,shutdown时析构 |
| **Intrusive Ref<T>** | Socket, SocketBase, EventPoll, 所有Ops类 | atomic int16_t, =0时delete |
| **可插拔Ops Registry** | LockRegistry, TraceRegistry | 外部注册覆盖(brpc的butex/rpc_id) |
| **SPSC Lock-Free Queue** | SPSCRingQueue | 两阶段提交,cache-line对齐 |
| **Atomic fd-indexed Array** | ArraySet<Socket/EventPoll> | atomic<T*>[]按fd直接索引 |
| **Template Method** | DataRxOps::RxDataSet | 通用算法调用protected virtual PtrFloorToBoundary |
| **双缓冲+三域** | SplitTrace | write/read/epoll各2个TraceBuffer,atomic切换 |
| **Reservoir采样** | TracepointExt | 1024采样+p50/p90/p95/p99/p999 |
| **Per-CPU Sharding** | TracerExt | sched_getcpu()→agent[]消除跨线程竞争 |
| **Function-Pointer Hooking** | UbsZcopyAdapter | 替换brpc blockmem_allocate/deallocate指针 |
| **ELF Symbol Scanning** | DynSymScanner | 解析.symtab/.strtab定位mangled符号 |
| **错误码位标志** | InnerCode | bit30=可重试, bit29=可降级,OR'd与基础错误 |
| **Scope Guard** | ScopeExit<F> | EBO优化,可deactivate |
| **动态扩缩容** | QbufQueue | 2x扩/50%缩,25%/100%滞后带 |
| **状态机** | ConnectorOps/AcceptorOps | kSTART→kRETRY→kDEGRADE/kFAILED |
| **dlopen/dlsym Indirection** | DlApi/LibcApi/UmqApi/UrmaApi | 静态类+函数指针,运行时加载 |

---

## 14. 已知问题清单

| # | 问题 | 位置 | 严重度 | 说明 |
|---|------|------|--------|------|
| 1 | `close()`潜在无限递归 | ubsocket_sock.cpp:64 | **严重** | 非TCP模式调用`close(fd)`而非`LibcApi::close(fd)` |
| 2 | 13个桩函数返回0违反POSIX | ubsocket_sock.cpp多处 | **严重** | send/recv等返回0而非-1+ENOSYS |
| 3 | fcntl/ioctl/sendmsg无TCP守卫 | ubsocket_sock.cpp:259-272 | **严重** | TCP模式也返回0 |
| 4 | epoll_create1返回fd=0 | ubsocket_epoll.cpp | **严重** | stdin被当作epoll fd |
| 5 | ubsocket_uninit不完整 | ubsocket.cpp | **中等** | 不重置UBS_INITED,不释放g_zcopy_allocator等 |
| 6 | umq_transport_pool_resource_create无限递归 | dl_umq_api.h:335 | **严重** | dlopen分支调用自身而非_ptr |
| 7 | 5+个UMQ API未DL_API_LOAD | dl_umq_api.cpp | **严重** | stats/trace/transport_pool_ptr保持nullptr |
| 8 | umq_post/poll_api签名不匹配 | dl_umq_api.h:64-65 | **高** | 类型别名与wrapper参数类型不一致 |
| 9 | DlApi::UnLoad忽略位掩码 | dl_api.cpp:69 | **低** | 总是卸载全部 |
| 10 | SocketTypeToStr对无效值越界 | ubsocket_core_types.cpp:32 | **低** | 应返回strings[COUNT]而非strings[value] |
| 11 | IovConverter/BufferConverter缺MemCopy覆写 | ubsocket_buf_converter.h | **中等** | 基类声明纯虚但子类未覆写 |
| 12 | SocketBase::Create行58死代码 | ubsocket_socket.cpp:58 | **低** | result未更新后的不可达检查 |
| 13 | UmqLogger级别映射level%3 | ubsocket.cpp | **低** | 对高level值产生意外类别 |
| 14 | UMQ_INITED不可安全重置 | umq_backend.cpp | **中等** | static bool无法重新初始化 |
| 15 | TxCqePoller使用RefDynamicCast | ubsocket_tx_cqe_poller.cpp | **低** | TODO改为static_cast |
| 16 | Ref<T> move赋值用std::__exchange | ubsocket_ref.h:129 | **中等** | libc++内部符号,不可移植 |
| 17 | MakeRef传参by value | ubsocket_ref.h:189 | **低** | C++11无完美转发 |
| 18 | QbufQueueT用C99 flexible array | ubsocket_qbuf_queue.h | **低** | 非标准C++,GCC扩展 |
| 19 | Validator::AddStrNotEmtpyRule拼写错误 | ubsocket_setting_validator.h | **低** | 应为NotEmpty |
| 20 | ExecutorService用Meyer's而非LeakySingleton | ubsocket_thread_pool.h | **中等** | 可能与LeakySingleton后台线程shutdown冲突 |
| 21 | Func::SecureRandUInt32非static | ubsocket_functions.h:73 | **低** | 需Func实例调用,与其他static方法不一致 |
| 22 | GlobalSetting::GetEnv(float)stod→int64_t→float | ubsocket_global_setting.h:167 | **中等** | 应直接static_cast<float>(stod) |
| 23 | ubsocket_ring_buffer.h重复typedef u_mutex_t | ubsocket_ring_buffer.h:17 | **低** | 与ubsocket_def.h重复定义 |
| 24 | ubsocket_cntl.cpp空壳 | ubsocket_cntl.cpp | **低** | fcntl/ioctl实现在sock.cpp,此文件冗余 |
| 25 | ubsocket_struct_helper.h缺少rpc_id_ops | ubsocket_struct_helper.h | **低** | u_init_options_t有8字段但只打印7个 |
