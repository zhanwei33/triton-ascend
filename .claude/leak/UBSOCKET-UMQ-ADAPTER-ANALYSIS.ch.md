# ubsocket UMQ 适配层逐文件分析 (csrc/core/umq/)

> 本文对 `src/ubsocket/csrc/core/umq/` 目录下全部 37 个文件逐一分析。该目录是 ubsocket 通用抽象层 (`csrc/core/ubsocket_*.cpp`) 与底层 libumq 之间的**具体实现层**:把 POSIX 语义的 socket/connect/accept/writev/readv 转译为 UMQ 的 umq_create/umq_bind/umq_post/umq_poll 等调用。
>
> - 命名空间: `ock::ubs::umq`
> - 依赖方向: 通用层头文件 → 本目录具体实现 → `under_api/dl_umq_api.h`(libumq 动态加载)
> - 配套阅读: `UBSOCKET-BRPC-UB-TEST-FLOW.ch.md`(端到端流程)、`UBSOCKET-ARCHITECTURE.ch.md`(整体架构)

## 目录文件清单 (37 个)

| 类别 | 文件 | 行数 | 职责 |
|------|------|------|------|
| 配置/生命周期 | umq_setting.{cpp,h} | 355/100 | UMQ 配置静态类(env→成员) |
| 配置/生命周期 | umq_backend.{cpp,h} | 506/91 | UMQ 库 init/uninit + 零拷贝分配器 |
| 连接辅助 | umq_eid_table.h | 336 | EID→主UMQ映射 + RR索引 + 路由缓存(三个单例) |
| 连接辅助 | umq_conn_helper.{cpp,h} | 257/44 | 建链静态工具(选路/PrefillRx/共享JFR注册) |
| Socket对象 | umq_socket.{cpp,h} | 657/299 | UmqSocket 类 + 协商报文结构 |
| 建链(client) | umq_socket_connector.{cpp,h} | 1096/100 | UmqConnectorOps(client侧全流程) |
| 建链(server) | umq_socket_acceptor.{cpp,h} | 640/87 | UmqAcceptorOps(server侧全流程) |
| 发送 | umq_data_tx_ops.{cpp,h} | 742/93 | UmqTxOps(PollTx/AllocTxBuf/PostSend) |
| 发送 | umq_tx_helper.{cpp,h} | 326/86 | UmqTxHelper(umq_poll TX + CQE处理) |
| 接收 | umq_data_rx_ops.{cpp,h} | 434/61 | UmqRxOps(PollRx/GetQbuf/RxDataSet) |
| Buffer管理 | umq_buffer_receive_queue.{cpp,h} | 264/96 | 共享JFR RX队列(保序+乱序重排) |
| Buffer管理 | umq_buf_converter.h | 72 | IOBuf→umq_buf 切片转换器 |
| Buffer管理 | umq_qbuf_list.h | 79 | umq_buf 单链表宏(C风格) |
| Buffer管理 | umq_bounded_seq.h | 162 | 环形序列号算子(保序用) |
| EpollRunner | umq_share_jfr_epoll_runner_ops.{cpp,h} | 325/85 | 共享JFR RX runner(主UMQ poll) |
| EpollRunner | umq_tp_tx_epoll_runner_ops.{cpp,h} | 133/99 | Jetty池 TX runner |
| EpollRunner | umq_tp_event_epoll_runner_ops.{cpp,h} | 55/47 | Jetty池事件 runner(eventfd唤醒) |
| EpollRunner | umq_epoll_ops.h | 30 | EventPollOps 子类(注册TX事件) |
| 资源管理 | umq_tp_wait_queue.{cpp,h} | 103/99 | Jetty资源等待队列(MPSC) |
| 资源管理 | umq_transport_pool.{cpp,h} | 318/72 | Jetty传输池创建/重建/清理 |
| Errno | umq_errno_converter.{cpp,h} | 139/310 | UMQ错误码→Linux errno(**.h 冻结**) |

---

## 1. 配置与生命周期

### 1.1 umq_setting.h / umq_setting.cpp — UMQ 配置静态类

**职责**: 把 `UBSOCKET_*` 环境变量加载为进程级静态配置,供整个 umq 适配层读取。

**关键成员** (`umq_setting.h`):
- 设备/选路: `UMQ_DEV_NAME`/`UMQ_LOCAL_EID`/`UMQ_EID_INDEX`/`UMQ_IS_BONDING`/`UMQ_DEV_SCHEDULE_POLICY`(ROUND_ROBIN/CPU_AFFINITY/CPU_AFFINITY_PRIORITY)/`UMQ_ALL_SOCKET_IDS`/`UMQ_PROCESS_SOCKET_ID`
- 传输模式: `UMQ_UB_TRANS_MODE`(RC_TP/RM_TP/RM_CTP/RC_CTP) → 派生 `UMQ_UB_TP_MODE`/`UMQ_UB_TP_TYPE`
- 队列深度: `UMQ_FC_DEFAULT_CREDIT`/`UMQ_FC_MAX_CREDIT`/`UMQ_FC_MIN_CREDIT`(流控信用)
- 内存池: `UMQ_MEM_POOL_INIT_SIZE_MB`/`UMQ_MEM_POOL_MAX_SIZE_MB`/`UMQ_BUF_POOL_DEPTH`/`UMQ_TINY_POOL_*`
- Jetty: `UMQ_TP_TYPE`(SINGLE/POOL)/`UMQ_TP_POOL_SIZE`
- 保序: `UMQ_MAX_O3_GAP`/`UMQ_O3_TIMEOUT_MS`(CTP乱序熔断)
- 探针: `UMQ_PROBE_USER_DATA_ID = 0xFFFFFF`(2^24-1,保留作探测包标识)
- 序列号: `UMQ_SOCKET_SEQ_NUM_BIT_WIDTH = 24` → `UMQ_SOCKET_SEQ_NUM_MAX = 2^24-2`(预留一个值给探针)

**关键方法** (`umq_setting.cpp`):
- `AddRules()` (`:83`): 向 `Validator` 注册 int64/strEnum 校验规则(范围、枚举值)
- `LoadEnv()` (`:120`): `GlobalSetting::GetEnvAndValidate` 逐项读 env,关键转换 `UMQ_UB_TRANS_MODE` 字符串→三元组(`UMQ_UB_TRANS_MODE`/`UMQ_UB_TP_MODE`/`UMQ_UB_TP_TYPE`)
- `VerifySetting()` (`:229`): `UMQ_MEM_POOL_MAX_SIZE_MB >= INIT + 64` 校验
- `GetIOBufSize()` (`:268`): 按 `IO_BLOCK_TYPE`(4K/8K/.../64K)返回 `SIZE - IOBUF_DIFF`(block 头预留),CTP默认4K、其余8K
- `FloorMask()` (`:286`): 返回 block 地址对齐掩码(按 block 大小),供零拷贝 `PtrFloorToBoundary` 回溯 Block 头
- `DefaultBlockTypeCheck()` (`:304`): CTP 模式→4K,其余→8K

**陷阱**: `UmqSetting` 构造 `= delete`,全静态;`Init()` 是 private,只由 `friend UmqBackend` 调用。

### 1.2 umq_backend.h / umq_backend.cpp — UMQ 库生命周期 + 零拷贝分配器

**职责**: libumq 的 init/uninit、UB 设备添加、共享主 UMQ 预创建、零拷贝内存分配器。

**UmqBackend 静态类** (`umq_backend.h:23`):
- `UMQ_MUTEX`/`UMQ_INITED`: 互斥+幂等
- `Init()` (`umq_backend.cpp:32`): `UmqSetting::Init()` → 构造 `umq_init_cfg_t`(feature/buf_pool/flow_control/trans_info) → `UmqApi::umq_init` → `AddUbDev` → 设 `LINK_SELECTION_POLICY`(BONDING_BACKUP/BONDING_ROUTE/RAW_DEVICE) → 取 socket_id → perf → 若 BONDING_BACKUP 则预创建主UMQ+Prefill+InitShareJfrMonitering+WarmUp
- `AddUbDev()` (`:217`): 设备名空时 `FindDevName` 找 `bonding_dev_0`(或首个 bonding_dev_);`udma*` 前缀走 `FindDevEid`;按 dev_name 是否含 `bonding_dev` 决定 `UMQ_DEV_ASSIGN_MODE_DEV/EID`,`umq_dev_add`(EEXIST 视为成功)
- `CreateShareMainUmq()` (`:351`): BONDING_BACKUP 下 `GetRouteList(bonding→bonding)` 取所有 port,排序+去重作为 `used_ports`,加 `UMQ_CREATE_FLAG_MAIN_UMQ` 调 `umq_create`,注册进 `UmqEidTable`
- `InitShareJfrMonitering()` (`:467`): 启动 `SHARE_JFR_RX_RUNNER`,`umq_interrupt_fd_get` 取主UMQ RX中断fd,`AddEpollEvent` 注册进 runner
- `UnInit()` (`:196`): `umq_uninit` + 停 perf

**UmqZeroCopyAllocator** (`umq_backend.h:42`): 实现 `UbsZeroCopyAllocator` 接口
- `allocate`: 按 `ubs_iobuf_alloc_option_t.pool_type` 映射到 `umq_alloc_option_t.pool_type`(TINY/NORMAL/ESCAPE),调 `umq_buf_alloc(size, BRPC_ALLOC_DEFAULT_BUF_NUM, ...)`,返回 `buf->buf_data`(跳过 umq_buf_t 头)
- `deallocate`: `umq_data_to_head(ptr)` 反查 umq_buf_t,清 `qbuf_next`,`umq_buf_free`

> 此分配器是 brpc `UBIOBuf` 与 UMQ 内存池的桥梁——brpc IOBuf Block 的数据指针直接指向 UMQ DMA 内存,writev 时零拷贝。

---

## 2. 连接辅助

### 2.1 umq_eid_table.h — 三个单例注册表

**1. `UmqEidTable`(普通单例, `:98`)**: `eid → vector<MainUmqState>` 映射。一个 eid 可对应多种 trans_mode 的主 UMQ。
- `MainUmqState`(`:47`): 持 `m_umqh`/`m_ubTransMode`/`m_prefilled` + 互斥。`EnsurePrefilled<F>()`(`:74`) 是**双重检查锁**(atomic acquire + mutex),保证 RX 预填只执行一次——多 socket 共享同一主 UMQ 时关键
- `GetMainMutex()`: `CreateShareMainUmq` 用它做主 UMQ 创建的竞态保护(先查表→无则 create→再查表防并发→Add)

**2. `EidRegistry`(LeakySingleton, `:215`)**: `umq_dev_add` 去重 + RR 轮询索引
- `registered_eids_`: set,`CheckDevAdd` 用 `IsRegisteredEid` 避免重复 `umq_dev_add`
- `eid_index_map_`: `eid→uint32`,记录 RR 下次起始位置,`RegisterOrReplaceEidIndex`/`GetEidIndex`
- LeakySingleton: 进程级、不析构(避免退出顺序问题)

**3. `RouteListRegistry`(LeakySingleton, `:285`)**: `eid → umq_route_list_t` 路由缓存
- `GetRouteList` 查缓存命中则免调 `umq_get_route_list`(省一次内核往返)

**辅助算子**: `UmqEidHash`(取 raw 前8+后8字节异或)、`UmqEidEqual`(memcmp)。

### 2.2 umq_conn_helper.h / umq_conn_helper.cpp — 建链静态工具

`UmqConnHelper`(全静态,构造 delete):

- `GetDevEid(dev_name, eid_idx, *eid)` (`:21`): `umq_dev_info_get` 取设备信息,遍历 `eid_list` 匹配 `eid_index`
- `PrefillRx(umq_handle)` (`:48`): RX 预填主循环——`GetLeftPostRxNum` 算剩余槽位(`rqe_post_factor * rx_depth`),循环 `umq_buf_alloc`(带 `UMQ_ALLOC_FLAG_HEAD_ROOM_SIZE, sizeof(Block)` 头预留)+`umq_post(UMQ_IO_RX)`,每次最多 `UMQ_POST_BATCH_MAX`
- `NewBaseUmqCreateOptions` (`:113`): 构造 `umq_create_option_t` 基础模板(tx/rx depth、buf size、`UMQ_MODE_INTERRUPT`、priority、`GetTpInfo`),POOL 模式加 `UMQ_CREATE_FLAG_SHARE_TRANSPORT`
- `GetTpInfo` (`:145`): trans_mode→(tp_mode, tp_type) 映射(RC_TP→(RC,RTP)、RM_CTP→(RM,CTP) 等)
- `GetRouteList` (`:166`): 构造 `umq_route_key`(src/dst bonding_eid + tp_type),`umq_get_route_list`,空返回错误
- `RegisterSharedJfrForRead` (`:199`): 启动 SHARE_JFR_RX_RUNNER,`umq_interrupt_fd_get` 取主UMQ RX fd,构造 `RUNNER_EVENT_TYPE_SHARE_JFR` 事件,`AddEpollEvent` 注册(ExtContext 携 umq_handle)
- `GetTargetChipId` (`:239`): 在 socket_ids 中找 processSocketId 的位置,映射到 chip_id_list——CPU 亲和选路用

---

## 3. Socket 对象

### 3.1 umq_socket.h / umq_socket.cpp — UmqSocket 核心类

**`UmqSocket`** (`umq_socket.h:42`): 继承 `SocketBase` + `UmqSocketSeq`(序列号算子)。

**关键成员**:
- `umq_handle_`(子UMQ)、`share_umq_handle_`(主UMQ,共享JFR时非0)
- `trans_mode_`/`topo_type_`/`negotiated_version_`/`peer_version_`
- `rxQueue`(`UmqBufferReceiveQueue*`,共享JFR RX 缓存,非共享模式为null)
- `jetty_alloc_state`(IDLE/WAITING/READY,POOL 模式 Jetty 资源状态)
- `used_ports_`(`unique_ptr<umq_port_id_t[]>`,光组网异常CQE时标记失效端口)

**关键方法**:
- `CreateLocalUmq(conn_eid, used_ports, topo_type)` (`umq_socket.cpp:52`): 建 umq 的核心——构造 `umq_create_option_t`(NewBaseUmqCreateOptions + dev_info.assign_mode + used_ports[仅BONDING_BACKUP] + name="fd: %d" + umq_ctx=raw_socket_),调 `CreateSubUmq`,失败返回 `UBS_UMQ_CREATE|RETRYABLE|DEGRADABLE`。成功后 `RegisterFcTxEvent` + new `UmqBufferReceiveQueue` + SINGLE模式开启TX solicited + 保存used_ports
- `CreateSubUmq` (`:193`): 非共享JFR→直接 `umq_create`;共享JFR→`GetOrCreateMainUmq` 取/建主UMQ,加 `SHARE_RQ`(+POOL无 `SUB_UMQ`) flag,`umq_create` 建子UMQ,`share_umq_handle_=main_umq`
- `GetOrCreateMainUmq` (`:230`): 查 `UmqEidTable` 命中返回;未命中加 `MAIN_UMQ` flag `umq_create`,**双查防并发**——`GetMainMutex` 内再查,若他人已建则 `umq_destroy` 自己的用他人的
- `UpdateRxQueueAvailNum` (`:265`): `umq_state_get` 等待 `QUEUE_STATE_READY`,置 `rx_queue_avail_num_=UBS_RX_DEPTH`
- `UnbindAndFlushRemoteUmq` (`:285`): ack 残留中断→`umq_unbind`→`FlushTx`→共享JFR `FlushRxQueue` 否则 `FlushRx`
- `DestroyLocalUmq` (`:313`): `umq_destroy(umq_handle_)`,注释说明 `share_umq_handle_` 不在此删(作为 umq_handle_ 时会删)
- `CheckDevAdd(conn_eid)` (`:462`): `EidRegistry::IsRegisteredEid` 命中跳过;否则 `umq_dev_add`(EEXIST OK)+`RegisterEid`
- `AddQbuf`/`GetAndPopQbuf`/`FlushRxQueue`: 共享JFR下 rxQueue 的入/出/关
- `NewRxEpollIn`/`NewTxEpollIn` (`h:138/148`): 原子递增 `epoll_event_num_`,从0→1时置 `get_and_ack_event_=true`(触发中断ack路径)
- TX事件系列: `AddTxEvent`/`DelTxEvent`/`GetTxFd`/`ShouldRegisterTxEvent`(仅SINGLE)/`ProcessEpollEvent`
- CLI 数据 getter 系列: `GetSocketFlowControlData`/`GetSocketQbufPoolData`/`GetSocketUmqInfoData`/`GetSocketIoPacketData`/`GetSocketUmqPerfData`/`GetSocketCLIData`——调 umq_stats_* 系列填充 CLI 展示结构

**协商报文结构** (`umq_socket.h:225`): `CpMsg`(bind_info 交换)、`NegotiateReq`/`NegotiateRsp`/`NegotiateRoute`(一主三备 `BACK_ROUTE_MAX_NUM=3`)、`OtherRouteMessage`(重试)。`NEGOTIATE_REQ_WIRE_SIZE` = magic(8)+version(4)+body_len(4)+Req。`EID_FMT`/`EID_ARGS` 格式化宏。

### 3.2 umq_socket_connector.h / umq_socket_connector.cpp — Client 建链

**`UmqConnectorOps`** (`h:22`,继承 `ConnectorOps`): client 侧三段式建链。

**`UmqConnInfo`** (`h:36`): 扩展 `ConnInfo`,加 `peer_eid`/`peer_bonding_eid`/`bonding_eid`/`conn_eid`。

**三段式** (被 `Connector::Connect` 调用):
1. `PrepareConnect` (`:95`): 按 `UBS_HAND_SHAKE_MODE`:
   - `ConnectViaHandshakeOpt` (`:29`): `setsockopt(TCP_UB_SOCKET_HANDSHAKE)`,不支持降级TFO;`LibcApi::connect`
   - `ConnectViaTfo` (`:45`): `BuildNegotiateReqBuffer`→`setsockopt(TCP_FASTOPEN)`→`sendto(MSG_FASTOPEN)` SYN 携带协商报文;cookie 未命中则新建 tmp_fd 重试 + `dup3` 替换
   - 设 `TCP_NODELAY`/非阻塞;`EINPROGRESS/EALREADY` 修正为 OK
2. `Negotiate` (`:173`): `ConnectNegotiate` (`:379`)——收 negotiated_version(4B) + `ValidateNegotiated`(Major 不匹配降级TCP) + 收 `NegotiateRsp`(length-prefixed) + `DoRoute` + 发 `NegotiateRoute`
3. `CreateSocketResources` (`:183`): 状态机 `kSTART→DoUbConnect→收发ack/peer_ret→kOK/kRETRY/kDEGRADE/kFAILED`

**`DoUbConnect`** (`:558`): `CreateLocalUmq` + `GenerateSocketCommOps` + `umq_bind_info_get` + 发 CpMsg + 收对端 CpMsg + CLOS下检查 port cooldown + `umq_bind` + 共享JFR下 `EnsurePrefilled`(PrefillRx+RegisterSharedJfrForRead) + `UpdateRxQueueAvailNum`

**选路逻辑** (核心难点):
- `DoRoute` (`:503`): `GetDevRouteList`(缓存优先)→按 `topo_type`:`FULLMESH_1D` 走 `GetConnEid`;`CLOS` 走 `GetCpuAffinityUmqRoute`(分亲和组/不亲组)+`RRChooseMainRoute`(一主三备)
- `GetConnEid` (`:793`): 非RR→按 CPU socket_id 找同 chip_id 的 route;RR→`GetRoundRobinConnEid`
- `GetCpuAffinityUmqRoute` (`:881`): 本端 chip_id = `GetTargetChipId(UMQ_ALL_SOCKET_IDS, ...)`;对端 chip_id = `GetTargetChipId(peer_all_socket_ids_, peer_socket_id_)`;分出 affine(non_aff) 两组
- `RRChooseMainRoute` (`:956`): 从 startIndex 取主路,后续循环取最多3条备路,`RegisterOrReplaceEidIndex` 推进 RR 位置

**重试** `DoUbConnectRetry` (`:684`): `DestroyLocalUmq`→`CheckOtherRoute`/`CheckOtherRouteForClos`→发 `OtherRouteMessage`(kRETRY)→`DoUbConnect` 新路径→收发ack/peer_ret

### 3.3 umq_socket_acceptor.h / umq_socket_acceptor.cpp — Server 建链

**`UmqAcceptorOps`** (`h:22`,继承 `AcceptorOps`): server 侧。

**方法**:
- `ValidateProtocol` (`:414`): 读 8B magic 校验 `CONTROL_PLANE_PROTOCOL_NEGOTIATION`
- `ValidateVersion` (`:430`): 读 4B peer_version,`UBS_PROTOCOL_VERSION.Negotiate` 返回 `VersionCheckResult`
- `Negotiate` (`:30`): 调 `AcceptNegotiate`
- `AcceptNegotiate` (`:478`): Major不匹配→发 server 版本+读丢弃 body→降级;否则读 NegotiateReq + 发 negotiated_version + 发 NegotiateRsp(ret_code 校验 bonding 一致)+ 收 NegotiateRoute + **视角翻转**(client的src/dst互换为server视角)+ 适配一主三备 back_routes
- `CreateSocketResources` (`:62`): 状态机,与 client 对称,但收发 ack 顺序相反(server 先收 peer_ret 后发 ack_ret)
- `DoUbAccept` (`:187`): 与 `DoUbConnect` 对称
- `DoUbAcceptRetry` (`:312`): 收 `OtherRouteMessage`→`CheckDevAdd` 新 eid→`DoUbAccept`

---

## 4. 数据发送 (TX)

### 4.1 umq_data_tx_ops.h / umq_data_tx_ops.cpp — UmqTxOps

**`UmqTxOps`** (`h:24`,继承 `DataTxOps`): 持 `local_umqh_`、`head_buf_`/`tail_buf_`(unsignaled wr 缓存链表)、`unsolicited_*`/`unsignaled_wr_num_`(solicited/signaled 聚合)、`successful_post_count_`。

**核心方法**:

- `AllocTxBuf(size, count)` (`:26`): `umq_buf_alloc(size, count, INVALID_HANDLE, nullptr)`,失败 `DpRearmTxInterrupt`(重新武装TX中断触发下次重试)
- `PollTx(sock)` (`:394`): 三分支
  - `get_and_ack_event_`: `GetAndAckEvent`(`umq_get_cq_event`+`umq_ack_interrupt`+`umq_rearm_interrupt`)+`PollUmqTx(poll_to_empty=true)`
  - `tx_queue_avail_num_==0`: `PollUmqTx(false)`
  - else: `PollUmqTxOnce`(后台 TxCqePoller 已批量回收,这里轻量再 poll)
- `PollUmqTx`/`PollUmqTxOnce`/`DoUmqTxPoll`: 调 `UmqTxHelper::PollUmqTx`,`tx_queue_avail_num_` 增加
- `PostSend(sock, buf, batch, cvt)` (`:156`): **发送主流程**
  1. 遍历 batch,每个 wr:`MemCopy` 切片→`DataToBlock`→`block->IncRef()`(零拷贝引用)→设 `buf_pro->opcode=UMQ_OPC_SEND_IMM`→`imm.user_data=FetchAddSeqNum(1)`(序列号)→trace
  2. solicited 策略:最后一个 wr 或 `avail==1` 或聚合超阈值 `TX_REPORT_THRESHOLD`/`TX_UNSOLICITED_BYTES_MAX` → `solicited_enable=1`
  3. signaled 策略:每 `TX_REPORT_THRESHOLD` 个 wr 设 `complete_enable=1` 并缓存 head 到 `user_ctx`(供 CQE 回收)
  4. `umq_post(local_umqh_, tx_buf_list, UMQ_IO_TX, &bad_qbuf)`
  5. 成功:`avail-=batch`,`successful_post_count_+=batch`
  6. 失败按 bad_qbuf 处理:`EAGAIN`→`need_fc_awake_`;`EMLINK`(无jetty)→`UmqTpWaitQueue.Enqueue`;`ENOBUFS`→`PollUmqTx(true)`+Enqueue;其他→返回-1销链。部分成功走 `HandleBadQBuf`
- `HandleBadQBuf` (`:615`): 遍历 bad 链表计 wr_cnt,恢复计数器,`umq_buf_free`
- `FlushTx` (`:658`): 拆链时 `DoUmqTxPoll` 清空 + 释放 unsignaled wr 缓存(DecRef+free),超时保护
- `Writable` (`:528`): POOL且 `UmqTpWaitQueue.Empty()` 可写;IDLE 可写;WAITING 不可写;其他 Enqueue 返回false
- `DpRearmTxInterrupt` (`:580`): `umq_rearm_interrupt(false)` 成功→设 EAGAIN 返回-1(注意:成功路径不走 Convert)
- `ProcessTracePacket` (`:42`): brpc 包头 magic 解析,维护 `pack_size_list`/`pack_size` 供 SplitTrace 关联

### 4.2 umq_tx_helper.h / umq_tx_helper.cpp — UmqTxHelper

**`UmqTxHelper`** (全静态): TX CQE 回收的共享逻辑(被 `UmqTxOps::DoUmqTxPoll` 和 `UmqTpTxEpollRunnerOps` 共用)。

- `PollUmqTx<PollArgs, F>` (`h:55`): 模板,把 lambda 包成 `ICallback` 转发 `PollUmqTxInternal`
- `PollUmqTxInternal` (`:23`): `umq_poll` 取 TX CQE 批;遍历:status≠0→`HandleTxCqeError`+`error_cb.invoke`;探测包→`HandleProbePacket`;正常→`ProcessTxCqe`
- `ProcessTxCqe(start_qbuf, end_qbuf, sock)` (`:103`): 沿 `user_ctx` 链表 DecRef 每个 Block,`umq_buf_free(start_qbuf)`,返回 wr_cnt
- `HandleTxCqeError` (`:155`): 探测包直free;否则 `LogTxCqeErrorMsg`+`ProcessErrorTxCqe`(DecRef+free)
- `PollUmqTxForFcReturn` (`:296`): 流控返回轮询,POOL 模式静默错误;`EMLINK` 重新 Enqueue umq_handle

**设计要点**: `ICallback` 虚接口 + lambda 包装,避免模板膨胀;不同调用方(TxOps vs TpTxEpollRunner)用不同 error_cb(后者额外 `shutdown` socket + `RebuildTp`)。

---

## 5. 数据接收 (RX)

### 5.1 umq_data_rx_ops.h / umq_data_rx_ops.cpp — UmqRxOps

**`UmqRxOps`** (`h:27`,继承 `DataRxOps`): 持 `local_umqh_`。

**核心方法**:

- `PollRx(sock)` (`:22`): 
  - 非共享JFR: `GetAndAckEvent`(`umq_get_cq_event`+ack+`m_rearm`不在此)→置 `get_and_ack_event_=false`
  - `poll_` 为真: `GetQbuf`
  - 遍历 cqe: 探测包→ProbeManager+free;错误(`FC_ERR`/`FC_UPDATE`/`FC_MSG`/`FC_EMLINK`)→`HandleErrorRxCqe`+置CLOSE/Enqueue;正常→`block_cache_.Insert(buf_data, data_size)`
- `GetQbuf(sock, buf, max_num)` (`:131`): 共享JFR→`umqSock->GetAndPopQbuf`(从 rxQueue);非共享→`UmqPollAndRefillRx`
- `UmqPollAndRefillRx` (`:145`): `umq_poll(UMQ_IO_RX)`,`avail-=poll_num`;不足时(`UBS_RX_DEPTH-avail > TX_REFILL_THRESHOLD`)`umq_buf_alloc`+`umq_post` 补充 RX 预填(失败走 `HandleBadQBuf`)
- `RxDataSet(buf, size)` (在 `ubsocket_data_rx.cpp:130` 的 `DataRxOps` 基类): `DataToBlock`→`block_cache_.CutAndInsertAfter` 挂到 brpc IOBuf Block 链表;0 时检查 epoll 事件号/EINTR/`recv(MSG_PEEK)`探测EOF/EAGAIN
- `RearmRxInterrupt` (`:345`): POOL 模式直接 OK;否则 `umq_rearm_interrupt(false)`。注意共享JFR不在此 rearm(由 ShareJfr runner 统一)
- `HandleErrorRxCqe` (`:246`): 按 `buf->status` switch 打日志,统一 `shutdown(SHUT_RD)`+置CLOSE(异步关闭,等下次 EPOLLIN)
- `FlushRx` (`h:42`): 非共享模式拆链时清 RX
- `PollSubUmqRx`: 共享JFR 子UMQ 轮询辅助

---

## 6. Buffer 管理

### 6.1 umq_buffer_receive_queue.h / .cpp — 共享 JFR RX 队列(保序)

**`UmqBufferReceiveQueue`** (`h:26`): 共享JFR下,主UMQ poll 到的 cqe 按 `umq_ctx`(socket fd)分派到各 socket 的 rxQueue。**仅 RM_CTP 模式启用乱序重排**(电组网 RTP 不需要)。

**成员**:
- `receive_queue`(`SPSCRingQueue<umq_buf_t*>`): 有序就绪队列,brpc readv 从此 MultiPop
- `out_of_order_queue`(`FastHeap<O3QueueComparator>`): 乱序暂存最小堆,按 sn 排序
- `m_expect_sn`: 期望下一 sn
- `m_max_ooo_gap`/`m_ooo_timeout_ns`: 熔断阈值(超 gap 或超时则强制前移)

**关键方法**:
- `Enqueue(buf)` (`:78`): shutdown 时 free;非 o3 → `receive_queue->Push`;o3 → `EnqueueInOrder`
- `EnqueueInOrder` (`:134`): FC/probe 包直 push;`Normalize(sn)`;`Distance(expect,sn)>MAX_WINDOW` 丢弃;`CheckAndTriggerMeltdown`;`expect==sn`→`ProcessNormalInOrder` 否则入 ooo 队列
- `ProcessNormalInOrder` (`:199`): push 当前 + 循环 pop ooo 顶部 sn==expect 的连续包 + 跳过重复
- `CheckAndTriggerMeltdown` (`:223`): gap > max 或 ooo 非空且超时 → `FlushOooQueueToReceiveQueueInternal`(强制前移 expect_sn)
- `DequeueBatch`: `receive_queue->MultiPop`

### 6.2 umq_buf_converter.h — IOBuf→umq_buf 切片转换器

- `UmqIovConverter` (`:18`,继承 `IovConverter`): `MemCopy(len, buf)` 把 brpc iovec 段**零拷贝**指到 `umq_buf->buf_data`(直接用 iov_base+offset,不 memcpy),跳过 0 长度 iov
- `UmqBufferConverter` (`:55`,继承 `BufferConverter`): `memcpy` 拷贝(非零拷贝路径)

### 6.3 umq_qbuf_list.h — umq_buf 单链表宏(C 风格)

`umq_buf_list_t{umq_buf_t *first}`,宏:`QBUF_LIST_FIRST/EMPTY/NEXT/INIT/INSERT_HEAD/INSERT_AFTER/FOR_EACH/FOR_EACH_SAFE/REMOVE_HEAD/REMOVE_AFTER/REMOVE`。利用 `umq_buf_t::qbuf_next` 字段。

### 6.4 umq_bounded_seq.h — 环形序列号算子

- `UmqBoundedSeqTraits<Bits, IntType, MaxVal>` (`:22`): 模板,`MASK`/`MODULUS`/`MAX_WINDOW`=`(MODULUS-1)/2`。`Normalize`/`Distance`/`CompareLessInCircularOrder`(带符号位移判断)/`Add`(支持负数回退)/`Next`
- `UmqSocketBoundedSequence` (`:116`): 加 `std::atomic` 的 CAS 递增/递减(`FetchAddSeqNum`/`FetchSubSeqNum`),供 `UmqSocket` 继承使用
- 默认 `MaxVal=0` 用位掩码自然回绕;`UMQ_SOCKET_SEQ_NUM_MAX=2^24-2` 预留 `2^24-1` 给探针

---

## 7. EpollRunner (后台 poller 线程)

三个 runner 类型(均继承 `EpollRunnerOps`),由 `EpollRunnerFactory` 单例管理,后台线程 `epoll_wait` + `ProcessOneEvent`。

### 7.1 umq_share_jfr_epoll_runner_ops.h / .cpp — 共享 JFR RX

**`UmqShareJfrEpollRunnerOps`** (`h:34`): 处理主 UMQ 的 RX 中断,把数据分派到各子 socket。持 `jfr_main_umq_`(fd→主umq 映射) + `UmqPollTraceTime`(打点)。

- `ProcessOneEvent` (`:27`): `RUNNER_EVENT_TYPE_SHARE_JFR`→查 `jfr_main_umq_`→`ProcessShareJfrEvent`;`RUNNER_EVENT_TYPE_SUB_UMQ_RX`→对子UMQ `umq_poll`+`HandleSubUmqPollBuffers`
- `ProcessShareJfrEvent` (`:97`): `ProcessMainUmqRearm`(get_cq_event+ack+rearm)→循环 `umq_poll(main_umq)`(`UBS_SHARE_JFR_LOOP_POLL_ENABLED` 时循环 poll)→流控 buf 排除→`umq_buf_alloc`+`umq_post` 补充 RX→`SiftSocketEventsWithUmqBuffers` 按 `umq_ctx` 分派到各 socket 的 `AddQbuf`+`NewRxEpollIn`+`AddReadableEvent`+`SetReadableEventFd`(唤醒 brpc epoll)
- `SiftSocketEventsWithUmqBuffers` (`:202`): 按 `buf_pro->umq_ctx`(=socket fd)查 `ArraySet<Socket>`,trace,`AddQbuf`,去重收集 socket_ptrs
- `ProcessMainUmqRearm` (`:259`): `umq_get_cq_event`(带 `TAG_TIMESTAMP`)+`umq_rearm_interrupt`+ 累积到 `GET_PER_ACK` 才 `umq_ack_interrupt`(批量 ack 减少中断)
- `AddEventToRunner` (`:298`): `InsertJfrMainUmq`(去重 epoll_ctl ADD)+`umq_rearm_interrupt`

**thread_local `FlashDynamicBitSet`**: 按 epoll 线程缓存,避免重复分配。

### 7.2 umq_tp_tx_epoll_runner_ops.h / .cpp — Jetty 池 TX

**`UmqTpTxEpollRunnerOps`** (`h:21`): 处理 Jetty 池(POOL 模式)的 TX 中断。持 `socket_data_`(fd→TxEpollEvent 映射)。
- `TxEpollEvent{type, umq_handle, tp_idx}`: `RUNNER_EVENT_TYPE_TP_TX`/`RUNNER_EVENT_TYPE_FC_TX`
- `ProcessOneEvent` (`:24`): TP_TX→`umq_get_cq_event`(带 `TP_HANDLE_IDX`)+循环 `PollUmqTx`(error_cb: shutdown socket + `RebuildTp`)+`umq_rearm_interrupt`;FC_TX→`PollUmqTxForFcReturn`
- `AddEventToRunner` (`:84`): epoll_ctl ADD + InsertSocketEventData + TP_TX 开启 TX 中断
- `DelEpollEvent` (`:117`): epoll_ctl DEL + RemoveSocketEventData

### 7.3 umq_tp_event_epoll_runner_ops.h / .cpp — Jetty 池事件唤醒

**`UmqTpEventEpollRunnerOps`** (`h:21`): 处理 Jetty 池的 eventfd 唤醒。
- `ProcessOneEvent` (`:21`): `RUNNER_EVENT_TYPE_TP_EVENT`→`eventfd_read` 取 cnt→`UmqTpWaitQueue::Instance().WakeUp(cnt)` 唤醒 cnt 个等待者

### 7.4 umq_epoll_ops.h — EventPollOps 子类(30行,无.cpp)

`UmqEventPollOps`: 仅声明 `AddTxEvent(socket, epoll_fd)`,实现委派给 `UmqSocket::AddTxEvent`。

---

## 8. 资源管理

### 8.1 umq_tp_wait_queue.h / .cpp — Jetty 资源等待队列

**`UmqTpWaitQueue`** (`h:65`): LeakySingleton + `MPSCRingQueue<UmqTpWaitQueueElement>`。元素两态:`UMQ_SOCKET`(带 SocketPtr)/`UMQ_HANDLE`(带 umq_handle)。
- `Enqueue(sock)` (`:19`): IDLE 状态才入队,成功置 WAITING
- `Enqueue(umq_handle)` (`:86`): 流控返回等待
- `TryWakeupOne`/`WakeUp(n)` (`:38/60`): pop 元素,SOCKET→置 READY;UMQ_HANDLE→`PollUmqTxForFcReturn`。`WakeUp` 上限 `UMQ_TP_POOL_SIZE`
- 容量取 `UBS_RX_DEPTH` 向上取整为 2 的幂,兜底 1024

### 8.2 umq_transport_pool.h / .cpp — Jetty 传输池

**`UmqTransportPool`** (普通单例, `h:26`): POOL 模式下预创建 Jetty 传输资源(tp),`Umqh2TpIdxMap` = `umq_handle → {tp_idx → fd_vec}`。
- `WarmUp` (`:25`): 启动 TRANSPORT_POOL_TX_RUNNER + (RM+POOL) `CreatePool` + `AddPollTxEvent` + `AddTransportEpollEvent`
- `CreateOneTp` (`:112`): BONDING_BACKUP 下按 CPU 亲和/RR 选 port 序,`umq_transport_pool_resource_create` 返回 tp_idx,`umq_interrupt_fd_get` 取 TX fd 存表
- `RebuildTp` (`:221`): tp 异常时 `umq_transport_pool_resource_modify`(置err)+`destroy`+`CreateOneTp` 重建
- `Clean` (`:86`): 遍历 destroy 所有 tp
- `AddPollTxEvent` (`:263`): 每个 tp 的 fd 注册 `RUNNER_EVENT_TYPE_TP_TX` 到 TRANSPORT_POOL_TX_RUNNER
- `AddTransportEpollEvent` (`:290`): `umq_transport_pool_eventfd_get` 取池事件fd,注册 `RUNNER_EVENT_TYPE_TP_EVENT` 到 TRANSPORT_POOL_EVENT_RUNNER

---

## 9. Errno 映射

### 9.1 umq_errno_converter.h / .cpp — UMQ错误码→Linux errno

> **`umq_errno_converter.h` 是冻结文件(final),不可修改**(AGENTS.md 明确规定)。

**四条映射路径** (`.h:23` 注释详述):

1. **`Convert(op, umqRet, savedErrno)`** (统一表+override): CONNECT/ACCEPT/WRITEV/READV 统一查 `kCommonErrnoMappings`(15项)。`ShouldOverrideWithSavedErrno` 生效时用 savedErrno 覆盖(`UMQ_ERR_EPERM`+特定 errno,或 `UMQ_ERR_ENODEV`+EINVAL/EIO)。GET_STATE 特殊:非 `QUEUE_STATE_ERR/MAX` 返回0。
2. **`ConvertBufStatus(op, bufStatus, savedErrno)`** (方向区分表): op 决定表——CONNECT/ACCEPT→`kCommonConnectAcceptBufStatusMappings`(17项);WRITEV→`kWritevBufStatusMappings`(24项);READV→`kReadvBufStatusMappings`(24项)。语义差异(如 REM_OPERATION_ERR: WRITEV="Broken pipe", READV="Connection reset by peer")。
3. **`ConvertHandleResult(op, savedErrno)`** (有限透传): CREATE 仅透传 EINVAL/EPERM;BIND_INFO_GET 仅透传 ENOMEM/EINVAL;其余 EIO。
4. **`GetErrorDescription`/`GetBufStatusDescription`**: 日志描述。

**模板工具**: `FindErrno`/`FindDescription`/`FindBufStatusErrno`/`FindBufStatusDescription`(线性扫描 array),未命中回退 savedErrno(>0) 再 EIO。

**cpp 实现** (`:139`): 与头声明完全一致,`ShouldOverrideWithSavedErrno`(`:107`) 实现 EPERM/ENODEV 的 errno 白名单覆盖。

**使用约定**: 调用 UMQ API 后**立即** `int savedErrno = errno;` 再 `errno = Convert(...)`。生产代码遍布 `UBS_VLOG_ERR` 打印 mapped/original 双 errno。

---

## 关键设计要点总结

1. **共享 JFR 模型** (`UBS_ENABLE_SHARE_JFR=true` 默认): 多 socket 共享一个主 UMQ 的 JFR(RX 完成队列),子 UMQ 各有 TX 队列。`UmqEidTable`+`MainUmqState::EnsurePrefilled` 保证主UMQ 创建与 RX 预填只做一次。后台 SHARE_JFR_RX_RUNNER 线程统一 poll 主UMQ,按 `umq_ctx`(=socket fd)分派。

2. **Jetty 池化** (`UMQ_TP_TYPE=POOL`): POOL 模式下 Jetty 资源(传输路径)池化共享,`UmqTransportPool` 预创建多个 tp,`UmqTpWaitQueue` 管理资源等待。`umq_post` 返回 EMLINK(无jetty)/ENOBUFS 时入队等待,由 TRANSPORT_POOL_EVENT_RUNNER 的 eventfd 唤醒。

3. **零拷贝**: brpc `UBIOBuf` 通过 `UmqZeroCopyAllocator` 直接分配 UMQ DMA 内存,writev 时 `UmqIovConverter::MemCopy` 把 `umq_buf->buf_data` 直接指向 brpc IOBuf Block 数据(零 memcpy),`Block::IncRef` 保证生命周期。回收时 `DataToBlock` 反查 + `DecRef`。

4. **一主三备选路** (CLOS 光组网): `RRChooseMainRoute` 从统一路由池选 1 主 + 最多 3 备,`NegotiateRoute` 携带到对端;server 视角翻转。失败重试走 `CheckOtherRouteForClos` 从 `non_aff_route_list_` 容灾池重选。

5. **CTP 保序** (`RM_CTP`): `UmqBufferReceiveQueue` 的 o3 重排——`expect_sn` 顺序入队,乱序入 `FastHeap` 最小堆,gap 超阈值或超时强制前移(熔断防死等)。

6. **solicited/signaled 聚合**: TX 发送时多个 wr 聚合一次中断(solicited)和一次完成事件(signaled),减少中断风暴。`unsolicited_*`/`unsignaled_wr_num_` 计数器控制聚合阈值 `TX_REPORT_THRESHOLD`。

7. **异步关闭**: TX/RX CQE 错误不立即关 socket,而是 `shutdown(SHUT_RD)`+置 `SOCK_STAT_CLOSE`,等下次 EPOLLIN 让 brpc 读 EOF 自行关闭——避免在 writev/readv 中途销毁对象。

8. **errno 三段式**: 每个 UMQ API 调用后 `savedErrno=errno` → `errno=Convert(...)` → 日志双 errno。`umq_errno_converter.h` 冻结,改动需走特批流程。

## 参考

- `UBSOCKET-BRPC-UB-TEST-FLOW.ch.md` — brpc ub_test 端到端调用流程
- `UBSOCKET-ARCHITECTURE.ch.md` — ubsocket 整体架构与 mermaid 图
- `UBSOCKET-CSRC-ANALYSIS.ch.md` — csrc 全目录概览
- `UBSOCKET-IO.ch.md` — UBIOBuf 零拷贝内存模型
- 通用层: `csrc/core/ubsocket_socket.{h,cpp}`、`ubsocket_socket_connector.cpp`、`ubsocket_socket_acceptor.cpp`、`ubsocket_data_tx.cpp`、`ubsocket_data_rx.cpp`
