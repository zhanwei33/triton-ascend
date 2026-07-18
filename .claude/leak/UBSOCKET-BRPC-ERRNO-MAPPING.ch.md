# bRPC错误码映射参考

> 来源: `bRPC错误码1.0.xlsx` (Sheet: 接口调用链, 列D-F)
> 代码实现: `src/ubsocket/csrc/core/umq/umq_errno_converter.h/.cpp`
> UMQ错误码定义: `src/hcom/umq/include/umq/umq_errno.h`

***

## 人工验收查验清单

验收时应逐项确认以下内容，确保代码实现与xlsx设计文档完全对齐：

1. **映射表完整性**：对照第七节7.1，确认 `kCommonErrnoMappings`(13项) 中每个UMQ错误码均有对应条目，无遗漏
2. **映射目标errno正确性**：逐行核对每个UMQ错误码映射到的Linux errno值是否与xlsx D列一致，重点关注以下易错项：
   - `UMQ_ERR_EPERM`(=UMQ\_FAIL=-1) → EIO（两个错误码值相同，代码中仅映射EPERM）
   - `UMQ_ERR_ETSEG_NON_IMPORTED` → EIO
   - `UMQ_ERR_EFLOWCTL` → EIO
3. **ShouldOverrideWithSavedErrno覆盖逻辑**：对照第七节7.3，确认当UMQ返回UMQ\_FAIL/UMQ\_ERR\_EPERM时，savedErrno为EINVAL/ENODEV/ENOMEM/ENOEXEC/EIO时能正确覆盖；UMQ\_ERR\_ENODEV时savedErrno为EINVAL/EIO时能正确覆盖
4. **ConvertHandleResult逻辑**：对照第七节7.4，确认CREATE场景仅透传EINVAL和EPERM，BIND\_INFO\_GET场景仅透传ENOMEM和EINVAL，其余一律返回EIO
5. **Buf Status映射完整性**：对照第七节7.5，确认三张Buf状态映射表条目数和映射值：
   - `kCommonConnectAcceptBufStatusMappings` 17项，非成功均映射EIO
   - `kWritevBufStatusMappings` 24项，非错误状态(FC\_UPDATE/MEMPOOL\_SUCCESS/IMPORT\_TSEG\_SUCCESS/FAKE\_BUF\_FC\_UPDATE)映射0
   - `kReadvBufStatusMappings` 24项，同上
6. **Writev/Readv语义差异**：确认同一Buf Status在writev和readv中描述不同（远端错误writev用"Broken pipe"，readv用"Connection reset by peer"），但映射errno值相同（均为EIO）
7. **FindErrno/FindBufStatusErrno回退逻辑**：确认映射表未命中时先回退savedErrno，再回退EIO；不应出现映射表未命中时丢失错误信息的情况
8. **UMQ\_FAIL与UMQ\_ERR\_EPERM同值处理**：确认代码注释说明UMQ\_FAIL(-1)和UMQ\_ERR\_EPERM(EPERM=1)绝对值相同，映射表中只保留UMQ\_ERR\_EPERM条目，UMQ\_FAIL通过ShouldOverrideWithSavedErrno覆盖机制处理
9. **umq\_dev\_info\_get使用CONNECT映射表**：确认-UMQ\_ERR\_EINVAL→EINVAL、-UMQ\_ERR\_ENODEV→ENODEV在kCommonErrnoMappings中有对应条目
10. **umq\_state\_get GET\_STATE路径**：确认Convert(GET\_STATE)对QUEUE\_STATE\_ERR/MAX一律返回EIO，QUEUE\_STATE\_IDLE/READY返回0；不触发ShouldOverrideWithSavedErrno
11. **umq\_init/umq\_dev\_add(后端)归类CONNECT**：确认umq\_init和umq\_dev\_add(后端)直接使用Convert(CONNECT)，不使用CREATE；CREATE仅用于ConvertHandleResult(umq\_create)
12. **调用点流程归属与op标注**：对照"调用点流程归属总览"章节，确认C/A/W/R勾选列与代码实际调用方一致；op标注规则：共用代码(同一函数被多流程调用)按枚举优先级取最小值(CONNECT\<ACCEPT)；独立调用点(某流程专属)取实际流程op(WRITEV/READV/ACCEPT等)；UmqOperation(BufStatus)按数据方向标注(WRITEV/READV)；具体确认：PrefillRx共用→CONNECT，umq\_dev\_add(acceptor)独立→ACCEPT，umq\_bind(connector)独立→CONNECT，umq\_bind(acceptor)独立→ACCEPT(两处独立调用点，非共用代码)，TX数据通路→WRITEV，RX数据通路→READV
13. **PrefillRx建立阶段调用点**：确认connect/accept建立阶段通过PrefillRx函数调用umq\_buf\_alloc、umq\_post(RX)、umq\_state\_get，此三API在总览表中标注为C=✓A=✓W=—R=—(建立阶段)，而非数据通路；umq\_post(TX)和umq\_post(RX, refill)仍在数据通路
14. **writev/readv重点API详表override交互**：对照第五/六节重点API详表，确认writev数据通路每个Convert(WRITEV)调用点的override交互(UMQ\_FAIL+savedErrno覆盖、-UMQ\_ERR\_ENODEV+savedErrno覆盖)与3-4节connect/accept同类；readv同理
15. **umq\_poll仅ret<0走Convert**：确认umq\_poll(TX/RX)仅当返回值<0(负值)时走Convert路径(errno映射)；ret==0时直接返回-1不经过Convert，不设置errno；buf→status≠0走ConvertBufStatus路径(buf status映射)；两条路径独立
16. **umq\_cq\_buffer\_get不存在**：确认xlsx中umq\_cq\_buffer\_get在代码中不存在(全仓库搜索确认)；实际对应umq\_poll + buf->status检查两条路径
17. **epoll\_runner umq\_post硬编码UMQ\_FAIL**：确认ProcessShareJfrEvent中umq\_post失败时传UMQ\_FAIL(-1)到Convert而非实际返回值；override逻辑不受影响但丢失具体UMQ错误码信息
18. **umq\_dev\_add(acceptor)跳过-EEXIST**：确认umq\_dev\_add(acceptor)与umq\_dev\_add(后端)行为相同——返回-EEXIST时跳过映射(视为设备已添加)；其余失败走Convert(ACCEPT)
19. **umq\_rearm\_interrupt(TX, data)成功路径设EAGAIN**：确认DpRearmTxInterrupt中umq\_rearm\_interrupt成功(ret==0)时直接设置errno=EAGAIN并返回-1，不经过Convert；仅失败(ret≠0)时走Convert(WRITEV)
20. **代码位置列指向UMQ API调用行**：总览表中代码位置列指向UMQ API调用行(而非Convert行)，返回值判断列描述触发Convert/ConvertHandleResult的条件

***

## 一、UMQ错误码定义 (umq\_errno.h)

| 宏定义                            | 值                | 说明              |
| ------------------------------ | ---------------- | --------------- |
| UMQ\_SUCCESS                   | 0                | 成功              |
| UMQ\_FAIL                      | -1               | 通用失败            |
| UMQ\_INVALID\_HANDLE           | 0                | 无效句柄            |
| UMQ\_INVALID\_FD               | -1               | 无效FD            |
| UMQ\_ERR\_EPERM                | EPERM(1)         | 操作不允许           |
| UMQ\_ERR\_EAGAIN               | EAGAIN(11)       | 资源暂时不可用         |
| UMQ\_ERR\_ENOMEM               | ENOMEM(12)       | 内存不足            |
| UMQ\_ERR\_EBUSY                | EBUSY(16)        | 设备或资源忙          |
| UMQ\_ERR\_EEXIST               | EEXIST(17)       | 已存在             |
| UMQ\_ERR\_EINVAL               | EINVAL(22)       | 参数无效            |
| UMQ\_ERR\_ENODEV               | ENODEV(19)       | 设备不存在           |
| UMQ\_ERR\_ENOSR                | ENOSR(63)        | 流资源不足(Jetty/TP) |
| UMQ\_ERR\_ETIMEOUT             | ETIMEDOUT(110)   | 连接超时            |
| UMQ\_ERR\_EINPROGRESS          | EINPROGRESS(115) | 操作进行中           |
| UMQ\_ERR\_ETSEG\_NON\_IMPORTED | 0x0201(513)      | TSEG未导入         |
| UMQ\_ERR\_EFLOWCTL             | 0x0202(514)      | 流控错误            |

> **注意**: UMQ\_FAIL(=-1) 与 UMQ\_ERR\_EPERM(=EPERM=1) 的绝对值相同，代码中只映射UMQ\_ERR\_EPERM。

> **errno数值速查**: 本文引用的 14 个 Linux errno 符号(EPERM/EIO/EBADF/EAGAIN/ENOMEM/ENOEXEC/EBUSY/EEXIST/ENODEV/EINVAL/EPIPE/ENOSR/ETIMEDOUT/EINPROGRESS)的数值与标准解释，见附录A。

***

## 二、UMQ Buf状态定义 (umq\_buf\_status\_t)

| 枚举值                                  | 数值  | 说明                        |
| ------------------------------------ | --- | ------------------------- |
| UMQ\_BUF\_SUCCESS                    | 0   | 成功                        |
| UMQ\_BUF\_UNSUPPORTED\_OPCODE\_ERR   | 1   | WR操作码不支持                  |
| UMQ\_BUF\_LOC\_LEN\_ERR              | 2   | 本地数据过长                    |
| UMQ\_BUF\_LOC\_OPERATION\_ERR        | 3   | 本地操作错误                    |
| UMQ\_BUF\_LOC\_ACCESS\_ERR           | 4   | 本地内存访问错误                  |
| UMQ\_BUF\_REM\_RESP\_LEN\_ERR        | 5   | 远端响应长度错误                  |
| UMQ\_BUF\_REM\_UNSUPPORTED\_REQ\_ERR | 6   | 远端不支持请求                   |
| UMQ\_BUF\_REM\_OPERATION\_ERR        | 7   | 目标jetty无法完成操作             |
| UMQ\_BUF\_REM\_ACCESS\_ABORT\_ERR    | 8   | 远端访问内存错误或中止操作             |
| UMQ\_BUF\_ACK\_TIMEOUT\_ERR          | 9   | 重传超过最大次数(ACK超时)           |
| UMQ\_BUF\_RNR\_RETRY\_CNT\_EXC\_ERR  | 10  | RNR重试超限(远端jfr无buffer)     |
| UMQ\_BUF\_WR\_FLUSH\_ERR             | 11  | Jetty错误态，硬件已处理WR          |
| UMQ\_BUF\_WR\_SUSPEND\_DONE          | 12  | 硬件构造fake CQE, user\_ctx无效 |
| UMQ\_BUF\_WR\_FLUSH\_ERR\_DONE       | 13  | 硬件构造fake CQE, user\_ctx无效 |
| UMQ\_BUF\_WR\_UNHANDLED              | 14  | flush jetty/jfs返回，硬件未处理WR |
| UMQ\_BUF\_LOC\_DATA\_POISON          | 15  | 本地数据中毒                    |
| UMQ\_BUF\_REM\_DATA\_POISON          | 16  | 远端数据中毒                    |
| UMQ\_BUF\_FLOW\_CONTROL\_UPDATE      | 128 | 流控窗口更新(非错误)               |
| UMQ\_MEMPOOL\_UPDATE\_SUCCESS        | 129 | 内存池更新成功(非错误)              |
| UMQ\_MEMPOOL\_UPDATE\_FAILED         | 130 | 内存池更新失败                   |
| UMQ\_IMPORT\_TSEG\_SUCCESS           | 131 | 导入TSEG成功(非错误)             |
| UMQ\_IMPORT\_TSEG\_FAILED            | 132 | 导入TSEG失败                  |
| UMQ\_FAKE\_BUF\_FC\_UPDATE           | 192 | 虚拟buffer流控更新(非错误)         |
| UMQ\_FAKE\_BUF\_FC\_ERR              | 193 | 虚拟buffer流控错误(CR状态异常)      |

***

## 调用点流程归属总览

> 本表列出全部UMQ API调用点及其归属流程(connect/accept/writev/readv)，第三至六节的调用点总览均引用此表。UmqOperation(errno)按枚举优先级标注最小值(CONNECT\<ACCEPT\<WRITEV\<READV\<CREATE\<BIND\_INFO\_GET\<GET\_STATE)；op对errno映射无区分作用(统一查kCommonErrnoMappings)。BufStatus路径op按数据方向区分。

| 调用阶段  | UMQ API                                | 映射方式                   | C | A  | W | R | op(errno)         | op(BufStatus) | 返回值判断                                     | 代码位置                                 |
| ----- | -------------------------------------- | ---------------------- | - | -- | - | - | ----------------- | ------------- | ----------------------------------------- | ------------------------------------ |
| 后端初始化 | umq\_init                              | Convert(统一表+override)  | ✓ | ✓  | — | — | CONNECT           | —             | ret≠0                                     | umq\_backend.cpp:60                  |
| 后端初始化 | umq\_dev\_info\_list\_get              | ConvertHandleResult    | ✓ | ✓  | — | — | BIND\_INFO\_GET   | —             | 返回nullptr                                 | umq\_backend.cpp:170                 |
| 后端初始化 | umq\_dev\_add(后端)                      | Convert(统一表+override)  | ✓ | ✓  | — | — | CONNECT           | —             | ret≠0且≠-EEXIST                            | umq\_backend.cpp:152                 |
| 建立阶段  | umq\_dev\_add(acceptor)                | Convert(统一表+override)  | — | ✓  | — | — | **ACCEPT**        | —             | ret≠0且≠-EEXIST                           | umq\_socket\_acceptor.cpp:349        |
| 建立阶段  | umq\_dev\_info\_get                    | Convert(统一表+override)  | ✓ | ✓  | — | — | CONNECT           | —             | ret≠0                                     | umq\_socket.cpp:515                  |
| 建立阶段  | umq\_create                            | ConvertHandleResult    | ✓ | ✓  | — | — | CREATE            | —             | 返回0(UMQ\_INVALID\_HANDLE=UMQ\_SUCCESS=0)³ | umq\_socket.cpp:162                  |
| 建立阶段  | umq\_buf\_alloc(prefill)               | 直接判断NULL               | ✓ | ✓  | — | — | —                 | —             | 返回NULL                                    | umq\_socket.cpp:254                  |
| 建立阶段  | umq\_post(RX, prefill)                 | Convert(统一表+override)  | ✓ | ✓  | — | — | CONNECT           | —             | ret≠UMQ\_SUCCESS                          | umq\_socket.cpp:263                  |
| 建立阶段  | umq\_state\_get(PrefillRx loop)        | Convert(GET\_STATE)    | ✓ | ✓  | — | — | GET\_STATE        | —             | state≠IDLE                                | umq\_socket.cpp:212                  |
| 建立阶段  | umq\_state\_get(PrefillRx exit)        | Convert(GET\_STATE)+ETIMEDOUT⁵ | ✓ | ✓  | — | — | GET\_STATE        | —             | state≠READY                               | umq\_socket.cpp:224                  |
| 建立阶段  | umq\_interrupt\_fd\_get(TX)            | Convert(统一表+override)  | ✓ | ✓  | — | — | CONNECT           | —             | fd<0                                      | umq\_socket.cpp:319                  |
| 建立阶段  | umq\_interrupt\_fd\_get(RX, share jfr) | Convert(统一表+override)  | ✓ | ✓  | — | — | CONNECT           | —             | fd<0                                      | umq\_socket.cpp:382                  |
| 建立阶段  | umq\_rearm\_interrupt(TX, init)        | Convert(统一表+override)  | ✓ | ✓  | — | — | CONNECT           | —             | ret<0                                     | umq\_socket.cpp:336                  |
| 建立阶段  | umq\_rearm\_interrupt(RX, init)        | Convert(统一表+override)  | ✓ | ✓  | — | — | CONNECT           | —             | ret<0                                     | umq\_socket.cpp:412/423              |
| 建立阶段  | umq\_bind\_info\_get                   | ConvertHandleResult    | ✓ | ✓  | — | — | BIND\_INFO\_GET   | —             | 返回0                                       | connector.cpp:433 / acceptor.cpp:130 |
| 建立阶段  | umq\_bind                              | Convert(统一表+override)  | ✓ | ✓¹ | — | — | CONNECT / ACCEPT¹ | —             | ret≠UMQ\_SUCCESS                          | connector.cpp:468 / acceptor.cpp:163 |
| 建立阶段  | umq\_get\_route\_list                  | Convert(统一表+override)  | ✓ | —  | — | — | CONNECT           | —             | ret≠0                                     | connector.cpp:547                    |
| 数据通路  | umq\_buf\_alloc(data)                  | 直接判断NULL               | — | —  | ✓ | ✓ | —                 | —             | 返回NULL                                    | tx\_ops:22, rx\_ops:140, epoll:129   |
| 数据通路  | umq\_post(TX)                          | Convert(统一表+override)  | — | —  | ✓ | — | WRITEV            | —             | ret≠UMQ\_SUCCESS                          | tx\_ops:103                          |
| 数据通路  | umq\_post(RX, refill)                  | Convert(统一表+override)  | — | —  | — | ✓ | READV             | —             | ret≠UMQ\_SUCCESS                          | rx\_ops:145, epoll:132               |
| 数据通路  | umq\_poll(TX)                          | Convert(统一表+override)  | — | —  | ✓ | — | WRITEV            | —             | ret<0(仅负值走Convert)                       | tx\_ops:261                          |
| 数据通路  | umq\_poll(RX)                          | Convert(统一表+override)  | — | —  | — | ✓ | READV             | —             | ret<0(仅负值走Convert)                       | rx\_ops:125 / epoll:57/104           |
| 数据通路  | CQE buf status(TX)                     | ConvertBufStatus(方向区分) | — | —  | ✓ | — | —                 | WRITEV        | buf->status≠SUCCESS                       | tx\_ops:351                          |
| 数据通路  | CQE buf status(RX)                     | ConvertBufStatus(方向区分) | — | —  | — | ✓ | —                 | READV         | buf->status≠SUCCESS                       | rx\_ops:208                          |
| 数据通路  | umq\_get\_cq\_event(TX)                | Convert(统一表+override)  | — | —  | ✓ | — | WRITEV            | —             | events<0                                  | tx\_ops:207                          |
| 数据通路  | umq\_get\_cq\_event(RX)                | Convert(统一表+override)  | — | —  | — | ✓ | READV             | —             | events<0                                  | rx\_ops:187 / epoll:194              |
| 数据通路  | umq\_rearm\_interrupt(TX, data)        | Convert(统一表+override)  | — | —  | ✓ | — | WRITEV            | —             | ret≠0(ret==0设EAGAIN不走Convert)            | tx\_ops:492                          |
| 数据通路  | umq\_rearm\_interrupt(RX, data)        | Convert(统一表+override)  | — | —  | — | ✓ | READV             | —             | ret<0                                     | rx\_ops:299 / epoll:206              |
| 释放    | umq\_buf\_free                         | N/A                    | — | —  | ✓ | ✓ | —                 | —             | void返回                                    | 多处²                                  |
| 释放    | umq\_ack\_interrupt                    | N/A                    | — | —  | ✓ | ✓ | —                 | —             | void返回                                    | tx\_ops:219, rx\_ops:199, epoll:217  |

> C=connect, A=accept, W=writev, R=readv。✓表示该流程调用此API，—表示不调用。
>
> op标注规则：共用代码(同一函数被多流程调用)按枚举优先级取最小值(CONNECT\<ACCEPT)；独立调用点(某流程专属)取实际流程op。op对errno映射无区分作用(统一表)，但标识调用点所属流程，对日志诊断有价值。
>
> ¹ accept流程umq\_bind是独立调用点，实际传UmqOperation::ACCEPT。connector的umq\_bind是独立调用点，传CONNECT。表中按独立调用点分别标注。
>
> ² umq\_buf\_free在socket teardown/FlushRxQueue和全局allocator(UmqZeroCopyAllocator)中也有调用，不属于C/A/W/R四个主流程。
>
> ³ UMQ\_INVALID\_HANDLE与UMQ\_SUCCESS同值(均为0)，umq\_create用`handle==0`判断失败。详见一节UMQ\_INVALID\_HANDLE说明。
>
> ⁴ 数据通路API（umq\_poll/umq\_get\_cq\_event/umq\_rearm\_interrupt）：errno映射使用统一表kCommonErrnoMappings(op=WRITEV/READV，独立调用点取实际流程op)；BufStatus映射按方向区分(WRITEV/READV)，详见第五/六节及第七节7.5。
>
> ⁵ umq\_state\_get(PrefillRx exit)：WaitUntilReady在循环超时退出时，若Convert(GET\_STATE)返回0(state仍为IDLE)，则override errno为ETIMEDOUT——区分"超时未就绪"与"成功(READY)"。Convert(GET\_STATE)本身将IDLE→0，此override确保IDLE超时场景产生有意义的errno。

***

## 三、connect 错误码映射 (xlsx D-F列)

### 3.1 UBSocket connect → TCP errno 映射总表

#### 映射规则说明

ubsocket使用以下映射策略：

| 映射策略                   | UmqOperation                | 规则                                                                                                                  | 适用API                                                        |
| ---------------------- | --------------------------- | ------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------ |
| Convert(统一表+override)  | CONNECT/ACCEPT/WRITEV/READV | 查kCommonErrnoMappings；ShouldOverrideWithSavedErrno生效时返回savedErrno覆盖表结果；表未命中回退savedErrno(>0时)，再回退EIO。op对errno映射无区分作用 | 全部UMQ API（见调用点总览）                                            |
| Convert(GET\_STATE)    | GET\_STATE                  | 返回值是umq\_state\_t枚举(0-3)，非UMQ\_ERR\_\*负值；不查映射表，不走ShouldOverride；ERR/MAX→EIO，IDLE/READY→0。PrefillRx exit调用点额外：若结果为0(state=IDLE超时)则override errno=ETIMEDOUT⁵ | umq\_state\_get                                              |
| ConvertHandleResult    | CREATE                      | 仅透传EINVAL和EPERM，其余一律EIO                                                                                             | umq\_create                                                  |
| ConvertHandleResult    | BIND\_INFO\_GET             | 仅透传ENOMEM和EINVAL，其余一律EIO                                                                                            | umq\_dev\_info\_list\_get, umq\_bind\_info\_get              |
| ConvertBufStatus(方向区分) | WRITEV, READV               | op决定BufStatus描述表选择：WRITEV查kWritevBufStatusMappings，READV查kReadvBufStatusMappings；errno映射值均为EIO                      | CQE buf status(TX/RX)                                        |
| ConvertBufStatus(方向区分) | CONNECT, ACCEPT             | op决定BufStatus描述表选择：CONNECT/ACCEPT查kCommonConnectAcceptBufStatusMappings；errno映射值均为EIO                               | CQE buf status(连接阶段)                                         |
| 不映射                    | —                           | void返回或NULL判断                                                                                                       | umq\_buf\_alloc(返回NULL), umq\_buf\_free, umq\_ack\_interrupt |

<br />

<br />

**ShouldOverrideWithSavedErrno生效条件** (Convert统一表路径均可触发；GET\_STATE/ConvertHandleResult/ConvertBufStatus不触发)：

| 条件                                                                                      | 覆盖行为                       |
| --------------------------------------------------------------------------------------- | -------------------------- |
| umqRet为UMQ\_FAIL(绝对值=UMQ\_ERR\_EPERM)且savedErrno∈{EINVAL, ENODEV, ENOMEM, ENOEXEC, EIO} | 用savedErrno覆盖表映射结果(EIO)    |
| umqRet为-UMQ\_ERR\_ENODEV且savedErrno∈{EINVAL, EIO}                                       | 用savedErrno覆盖表映射结果(ENODEV) |

> UMQ\_FAIL(=-1)与UMQ\_ERR\_EPERM(=EPERM=1)绝对值相同，映射表中只保留UMQ\_ERR\_EPERM条目，UMQ\_FAIL通过override机制处理。

#### 调用点总览

> 见"调用点流程归属总览"章节。connect流程标记C列(✓)的所有行。connect建立阶段新增umq\_buf\_alloc(prefill)、umq\_post(RX, prefill)、umq\_state\_get三个调用点(均在PrefillRx函数中)。

#### 重点API详表

##### umq\_bind — Convert(CONNECT)

override与表映射交互是此API的重点。完整映射见第七节7.1。

| umqRet            | savedErrno | override    | 最终结果    | 说明                       |
| ----------------- | ---------- | ----------- | ------- | ------------------------ |
| UMQ\_FAIL(-1)     | EINVAL     | 覆盖表映射EIO    | EINVAL  | urma\_bind\_jetty参数校验    |
| UMQ\_FAIL(-1)     | EIO        | 覆盖表映射EIO    | EIO     | urma内部故障                 |
| UMQ\_FAIL(-1)     | ENODEV     | 覆盖表映射EIO    | ENODEV  | urma无可用设备                |
| UMQ\_FAIL(-1)     | 0          | 不生效         | EIO     | 表映射(UMQ\_ERR\_EPERM→EIO) |
| -UMQ\_ERR\_ENODEV | EINVAL     | 覆盖表映射ENODEV | EINVAL  | override对ENODEV+EINVAL生效 |
| -UMQ\_ERR\_EINVAL | —          | 不生效         | EINVAL  | 表直接映射                    |
| 其他UMQ\_ERR\_\*    | —          | 不生效         | 见第七节7.1 | 表直接映射                    |

##### umq\_post — Convert(CONNECT)

override逻辑对UMQ\_FAIL场景生效，与umq\_bind同类。完整映射见第七节7.2。

| umqRet              | savedErrno | override | 最终结果    | 说明                 |
| ------------------- | ---------- | -------- | ------- | ------------------ |
| UMQ\_FAIL(-1)       | EAGAIN     | 覆盖表映射EIO | EAGAIN  | urma\_post资源暂时不可用  |
| UMQ\_FAIL(-1)       | ENOMEM     | 覆盖表映射EIO | ENOMEM  | urma\_post传入超出限制   |
| UMQ\_FAIL(-1)       | EINVAL     | 覆盖表映射EIO | EINVAL  | urma\_post访问无效地址空间 |
| -UMQ\_ERR\_EFLOWCTL | —          | 不生效      | EIO     | 表映射                |
| 其他UMQ\_ERR\_\*      | —          | 不生效      | 见第七节7.2 | 表直接映射              |

#### 特殊case脚注

- **umq\_dev\_add(后端/acceptor)**: 返回-EEXIST时跳过映射(视为设备已添加)；其余失败走Convert(CONNECT/ACCEPT)，使用kCommonErrnoMappings统一映射表和ShouldOverrideWithSavedErrno逻辑。后端使用CONNECT，acceptor使用ACCEPT。
- **umq\_create**: ConvertHandleResult(CREATE)仅透传EINVAL/EPERM，原xlsx标注ENOMEM/EEXIST应透传但代码统一兜底EIO。详见第七节7.4。
- **umq\_dev\_info\_get**: 使用Connect映射(CONNECT)，返回-UMQ\_ERR\_EINVAL→EINVAL, -UMQ\_ERR\_ENODEV→ENODEV均在kCommonErrnoMappings中。
- **umq\_state\_get**: 返回值是umq\_state\_t枚举(QUEUE\_STATE\_IDLE=0/READY=1/ERR=2/MAX=3)，不是UMQ\_ERR\_\*负值，无法查映射表；使用GET\_STATE枚举值，ERR/MAX映射EIO，IDLE/READY映射0。PrefillRx exit(WaitUntilReady超时退出)额外：若Convert返回0(state仍为IDLE)，override errno为ETIMEDOUT⁵。

### 3.2 汇总映射

> connect场景的UMQ错误码→TCP errno完整映射对照见第七节7.1(kCommonErrnoMappings，统一用于CONNECT/ACCEPT/WRITEV/READV)。

***

## 四、accept 错误码映射 (xlsx D-F列)

### 4.1 UBSocket accept → TCP errno 映射总表

> accept与connect共享3.1的全部映射规则，以下仅标注差异。映射规则说明见3.1。

#### 与connect的差异

| 差异点                      | connect                    | accept                                | 说明                                                                                |
| ------------------------ | -------------------------- | ------------------------------------- | --------------------------------------------------------------------------------- |
| umq\_dev\_add调用次数        | 1次(后端初始化, CONNECT)         | 2次(后端初始化CONNECT + acceptor设备添加ACCEPT) | acceptor的umq\_dev\_add使用ACCEPT，但errno映射使用同一kCommonErrnoMappings                   |
| umq\_bind的UmqOperation   | CONNECT(connector独立调用点)    | ACCEPT(acceptor独立调用点)                 | connector/acceptor各有独立umq\_bind调用，分别传CONNECT/ACCEPT。使用同一kCommonErrnoMappings统一映射表 |
| umq\_get\_route\_list    | CONNECT(connect专属)         | 不调用(accept从对端recv route)              | 仅connect流程调用GetDevRouteList                                                       |
| umq\_bind\_info\_get调用位置 | umq\_socket\_connector.cpp | umq\_socket\_acceptor.cpp             | 映射方式相同(ConvertHandleResult BIND\_INFO\_GET)                                       |

#### 调用点总览

> 见"调用点流程归属总览"章节。accept流程标记A列(✓)的所有行。accept专属调用点为umq\_dev\_add(acceptor)(A列仅✓)和umq\_bind(accept实际传ACCEPT)。accept建立阶段新增umq\_buf\_alloc(prefill)、umq\_post(RX, prefill)、umq\_state\_get三个调用点(均在PrefillRx函数中，与connect共用代码)。

#### 重点API详表

##### umq\_dev\_add(acceptor) — Convert(ACCEPT)

使用kCommonErrnoMappings统一映射表（与connect的umq\_bind共享同一映射表，op对errno映射无区分作用）。override逻辑相同。完整映射见第七节7.1。

| umqRet            | savedErrno | override | 最终结果    | 说明            |
| ----------------- | ---------- | -------- | ------- | ------------- |
| UMQ\_FAIL(-1)     | EINVAL     | 覆盖表映射EIO | EINVAL  | urma内部参数错误    |
| UMQ\_FAIL(-1)     | EIO        | 覆盖表映射EIO | EIO     | urma内部硬件/驱动故障 |
| -UMQ\_ERR\_EINVAL | —          | 不生效      | EINVAL  | 表直接映射         |
| 其他UMQ\_ERR\_\*    | —          | 不生效      | 见第七节7.1 | 表直接映射         |

##### umq\_bind(accept) — Convert(ACCEPT, acceptor独立调用点)

与connect的umq\_bind(CONNECT)使用同一映射表(kCommonErrnoMappings)，op对errno映射无区分作用。connector和acceptor各有独立umq\_bind调用点：connector传CONNECT，acceptor传ACCEPT。override行为完全相同。详见表见3.1 umq\_bind详表。

##### umq\_create — ConvertHandleResult(CREATE)

与connect完全相同：仅透传EINVAL/EPERM，其余EIO。xlsx差异说明见3.1特殊case脚注。

##### umq\_bind\_info\_get — ConvertHandleResult(BIND\_INFO\_GET)

与connect完全相同：仅透传ENOMEM/EINVAL，其余EIO。

### 4.2 汇总映射

> accept场景的UMQ错误码→TCP errno完整映射与connect使用同一映射表，对照见第七节7.1(kCommonErrnoMappings)。

***

## 五、writev 错误码映射 (xlsx D-F列)

> **errno映射已统一**: writev的errno映射与connect/accept/readv使用同一kCommonErrnoMappings统一映射表，op对errno映射无区分作用。writev与connect/accept的差异仅在：1) op=WRITEV(标识数据通路TX方向)；2) BufStatus描述语义（writev用"Broken pipe"，readv用"Connection reset by peer"）。映射规则说明见3.1。

### 5.1 UBSocket writev → TCP errno 映射总表

> writev与connect共享3.1的全部映射规则，以下仅标注writev数据通路特有内容。映射规则说明、ShouldOverrideWithSavedErrno生效条件见3.1。

#### 与connect的差异

| 差异点              | connect       | writev                   | 说明                                                    |
| ---------------- | ------------- | ------------------------ | ----------------------------------------------------- |
| UmqOperation     | CONNECT(建立阶段) | WRITEV(数据通路TX方向)         | op对errno映射无区分作用(统一表)，标识调用所属流程                         |
| ConvertBufStatus | 不调用(建立阶段无CQE) | ConvertBufStatus(WRITEV) | writev数据通路有CQE buf status检查，查kWritevBufStatusMappings |
| 调用点范围            | 建立阶段API       | 数据通路API                  | 建立阶段API见3.1，数据通路API见下方调用点总览                           |
| BufStatus远端错误描述  | —             | "Broken pipe"(远端断开)      | readv用"Connection reset by peer"，errno映射值相同(均为EIO)    |

#### 调用点总览

> 见"调用点流程归属总览"章节。writev流程标记W列(✓)的所有行。writev专属调用点均为数据通路：umq\_post(TX)、umq\_poll(TX)、umq\_get\_cq\_event(TX)、umq\_rearm\_interrupt(TX, data)、CQE buf status(TX)。

#### 重点API详表

所有writev数据通路API使用Convert(WRITEV)查kCommonErrnoMappings统一映射表，override逻辑与3.1 umq\_bind/umq\_post完全相同。以下列出override交互要点。

##### umq\_post(TX) — Convert(WRITEV)

override与表映射交互与3.1 umq\_post(CONNECT)同类。完整映射见第七节7.1。

| umqRet              | savedErrno | override | 最终结果    | 说明                 |
| ------------------- | ---------- | -------- | ------- | ------------------ |
| UMQ\_FAIL(-1)       | EAGAIN     | 覆盖表映射EIO | EAGAIN  | urma\_post资源暂时不可用  |
| UMQ\_FAIL(-1)       | ENOMEM     | 覆盖表映射EIO | ENOMEM  | urma\_post传入超出限制   |
| UMQ\_FAIL(-1)       | EINVAL     | 覆盖表映射EIO | EINVAL  | urma\_post访问无效地址空间 |
| -UMQ\_ERR\_EFLOWCTL | —          | 不生效      | EIO     | 表映射                |
| -UMQ\_ERR\_EAGAIN   | —          | 不生效      | EAGAIN  | 表直接映射              |
| 其他UMQ\_ERR\_\*      | —          | 不生效      | 见第七节7.1 | 表直接映射              |

> **注意**: umq\_post(TX)有两条错误路径(有bad\_qbuf/无bad\_qbuf，代码位置tx\_ops:111/149)，均使用Convert(WRITEV)，override逻辑相同。EAGAIN时设置need\_fc\_awake\_标志(不关闭连接)；非EAGAIN时标记flagEIO=1(连接关闭)。

##### umq\_poll(TX) — Convert(WRITEV)

override逻辑与umq\_post同类。完整映射见第七节7.1。

| umqRet            | savedErrno | override | 最终结果    | 说明             |
| ----------------- | ---------- | -------- | ------- | -------------- |
| UMQ\_FAIL(-1)     | EINVAL     | 覆盖表映射EIO | EINVAL  | urma\_poll参数校验 |
| UMQ\_FAIL(-1)     | EIO        | 覆盖表映射EIO | EIO     | urma内部故障       |
| -UMQ\_ERR\_EINVAL | —          | 不生效      | EINVAL  | 表直接映射          |
| 其他UMQ\_ERR\_\*    | —          | 不生效      | 见第七节7.1 | 表直接映射          |

> **注意**: umq\_poll(TX)存在两条独立映射路径：返回值≤0触发Convert(WRITEV)路径(errno映射)；buf→status≠0触发ConvertBufStatus(WRITEV)路径(buf status映射)。两条路径独立，不可混为一谈。

##### umq\_get\_cq\_event(TX) — Convert(WRITEV)

override逻辑与umq\_post同类。完整映射见第七节7.1。

| umqRet            | savedErrno | override | 最终结果    | 说明                |
| ----------------- | ---------- | -------- | ------- | ----------------- |
| UMQ\_FAIL(-1)     | EIO        | 覆盖表映射EIO | EIO     | urma\_wait\_jfc失败 |
| -UMQ\_ERR\_EINVAL | —          | 不生效      | EINVAL  | 表直接映射             |
| 其他UMQ\_ERR\_\*    | —          | 不生效      | 见第七节7.1 | 表直接映射             |

##### umq\_rearm\_interrupt(TX, data) — Convert(WRITEV)

override逻辑与umq\_post同类。完整映射见第七节7.1。

> **注意**: ret==0(成功)时直接设置errno=EAGAIN并返回-1(不经过Convert)；仅ret≠0(失败)时走Convert(WRITEV)。

| umqRet            | savedErrno | override | 最终结果    | 说明     |
| ----------------- | ---------- | -------- | ------- | ------ |
| ret==0(成功)       | —          | 不经过Convert  | EAGAIN  | 直接设置EAGAIN |
| UMQ\_FAIL(-1)     | EINVAL     | 覆盖表映射EIO | EINVAL  | 参数校验失败 |
| -UMQ\_ERR\_EINVAL | —          | 不生效      | EINVAL  | 表直接映射  |
| 其他UMQ\_ERR\_\*    | —          | 不生效      | 见第七节7.1 | 表直接映射  |

##### CQE buf status(TX) — ConvertBufStatus(WRITEV)

不使用Convert，使用ConvertBufStatus(WRITEV)查kWritevBufStatusMappings。ShouldOverrideWithSavedErrno不生效。几乎全部映射EIO，仅4种非错误状态映射0。完整对照见第七节7.5 Writev表(24项)。

##### umq\_buf\_alloc(TX) — 直接判断NULL

不使用Convert/ConvertBufStatus。umq\_buf\_alloc返回NULL时直接判断为内存不足，不调用errno映射函数。返回-1(连接关闭)。

##### umq\_ack\_interrupt — void返回

不映射。umq\_ack\_interrupt返回void，无法检查返回值。

### 特殊case脚注

- **umq\_poll(TX)双路径**: umq\_poll返回值≤0触发Convert(WRITEV)路径(errno映射)；buf→status≠0触发ConvertBufStatus(WRITEV)路径(buf status映射)。两条路径独立，最终errno可能来自任一路径，不可混为一谈。
- **umq\_post(TX) EAGAIN特殊处理**: EAGAIN时不关闭连接(设置need\_fc\_awake\_标志)；非EAGAIN时标记flagEIO=1导致连接关闭。
- **EBADF来源**: EBADF不在kCommonErrnoMappings中，也不在ShouldOverrideWithSavedErrno白名单中。只能通过FindErrno fallback路径产生：UMQ返回未定义错误码(表miss)+savedErrno=EBADF(>0)→fallback返回EBADF。属罕见边缘场景。
- **umq\_buf\_alloc(TX)**: 返回NULL时直接判断，不调用Convert。xlsx标注ENOMEM但代码无Convert调用，连接直接关闭。

### 5.2 汇总映射

> writev场景的UMQ错误码→TCP errno完整映射对照见第七节7.1(kCommonErrnoMappings，统一用于CONNECT/ACCEPT/WRITEV/READV)。BufStatus映射见第七节7.5 Writev表(kWritevBufStatusMappings, 24项)。

***

## 六、readv 错误码映射 (xlsx D-F列)

> **errno映射已统一**: readv的errno映射与connect/accept/writev使用同一kCommonErrnoMappings统一映射表。与writev的差异仅在：1) op=READV(标识数据通路RX方向)；2) BufStatus描述语义（readv用"Connection reset by peer"，writev用"Broken pipe"）。映射规则说明见3.1。

### 6.1 UBSocket readv → TCP errno 映射总表

> readv与connect共享3.1的全部映射规则，以下仅标注readv数据通路特有内容。映射规则说明、ShouldOverrideWithSavedErrno生效条件见3.1。

#### 与writev的差异

| 差异点               | writev                   | readv                      | 说明                                               |
| ----------------- | ------------------------ | -------------------------- | ------------------------------------------------ |
| UmqOperation      | WRITEV(TX方向)             | READV(RX方向)                | op对errno映射无区分作用(统一表)，标识调用所属方向                    |
| ConvertBufStatus表 | kWritevBufStatusMappings | kReadvBufStatusMappings    | errno映射值相同(均为EIO或0)，仅描述语义不同                      |
| BufStatus远端错误描述   | "Broken pipe"            | "Connection reset by peer" | 同一buf status值映射相同errno(EIO)，仅语义描述不同              |
| 调用点范围             | TX数据通路                   | RX数据通路(含共享JFR路径)           | readv有epoll\_runner共享JFR额外调用点                    |
| umq\_post(refill) | umq\_post(TX)            | umq\_post(RX, refill)      | readv有RX buffer refill场景(umq\_post(UMQ\_IO\_RX)) |

#### 调用点总览

> 见"调用点流程归属总览"章节。readv流程标记R列(✓)的所有行。readv专属调用点均为数据通路：umq\_poll(RX)、umq\_post(RX, refill)、umq\_get\_cq\_event(RX)、umq\_rearm\_interrupt(RX, data)、CQE buf status(RX)。epoll\_runner有共享JFR路径的额外调用点(umq\_poll/umq\_post/umq\_get\_cq\_event/umq\_rearm\_interrupt均标op=READV)。

#### 重点API详表

所有readv数据通路API使用Convert(READV)查kCommonErrnoMappings统一映射表，override逻辑与3.1及5.1完全相同。以下列出override交互要点。

##### umq\_poll(RX) — Convert(READV)

override与5.1 umq\_poll(TX)同类，仅op=READV。完整映射见第七节7.1。

| umqRet            | savedErrno | override | 最终结果    | 说明             |
| ----------------- | ---------- | -------- | ------- | -------------- |
| UMQ\_FAIL(-1)     | EINVAL     | 覆盖表映射EIO | EINVAL  | urma\_poll参数校验 |
| -UMQ\_ERR\_EINVAL | —          | 不生效      | EINVAL  | 表直接映射          |
| 其他UMQ\_ERR\_\*    | —          | 不生效      | 见第七节7.1 | 表直接映射          |

> **注意**: umq\_poll(RX)有3个调用点(rx\_ops:117, epoll:60, epoll:103)，override逻辑相同。与umq\_poll(TX)同理存在双路径：返回值≤0触发Convert(READV)；buf→status≠0触发ConvertBufStatus(READV)。

##### umq\_post(RX, refill) — Convert(READV)

override与5.1 umq\_post(TX)同类，仅op=READV。完整映射见第七节7.1。

| umqRet            | savedErrno | override | 最终结果    | 说明                |
| ----------------- | ---------- | -------- | ------- | ----------------- |
| UMQ\_FAIL(-1)     | EAGAIN     | 覆盖表映射EIO | EAGAIN  | urma\_post资源暂时不可用 |
| -UMQ\_ERR\_EAGAIN | —          | 不生效      | EAGAIN  | 表直接映射             |
| 其他UMQ\_ERR\_\*    | —          | 不生效      | 见第七节7.1 | 表直接映射             |

> **注意**: epoll\_runner共享JFR路径的umq\_post(refill)(代码位置epoll:119)使用Convert(READV)但**硬编码传UMQ\_FAIL**到Convert，丢弃了umq\_post实际返回值⁷。override逻辑仍相同(UMQ\_FAIL的override行为与UMQ\_ERR\_EPERM相同)，但丢失了具体UMQ错误码信息。
>
> ⁷ 代码位置epoll\_runner\_ops.cpp:121，传`UMQ\_FAIL`而非`umq\_ret`到Convert。

##### umq\_get\_cq\_event(RX) — Convert(READV)

override与5.1 umq\_get\_cq\_event(TX)同类，仅op=READV。完整映射见第七节7.1。有2个调用点(rx\_ops:180, epoll:183)。

##### umq\_rearm\_interrupt(RX, data) — Convert(READV)

override与5.1 umq\_rearm\_interrupt(TX)同类，仅op=READV。完整映射见第七节7.1。有2个调用点(rx\_ops:291, epoll:195)。

##### CQE buf status(RX) — ConvertBufStatus(READV)

不使用Convert，使用ConvertBufStatus(READV)查kReadvBufStatusMappings。ShouldOverrideWithSavedErrno不生效。几乎全部映射EIO，仅4种非错误状态映射0。与writev主要差异在远端错误语义描述（readv用"Connection reset by peer"，writev用"Broken pipe"），errno映射值相同。完整对照见第七节7.5 Readv差异对比表。

##### umq\_buf\_alloc(RX) — 直接判断NULL

与writev相同：不使用Convert/ConvertBufStatus，返回NULL时直接判断为内存不足。

##### umq\_ack\_interrupt — void返回

与writev相同：不映射。

#### 特殊case脚注

- **umq\_poll(RX)双路径**: 与writev同类，umq\_poll返回值≤0触发Convert(READV)；buf→status≠0触发ConvertBufStatus(READV)。两条路径独立。
- **epoll\_runner umq\_post硬编码UMQ\_FAIL⁷**: 共享JFR路径ProcessShareJfrEvent中(epoll\_runner\_ops.cpp:121)，umq\_post失败时硬编码传UMQ\_FAIL到Convert，丢弃实际返回值。override逻辑不受影响(UMQ\_FAIL与UMQ\_ERR\_EPERM绝对值相同)，但丢失了具体UMQ错误码信息(如-UMQ\_ERR\_EAGAIN被当作UMQ\_FAIL处理，可能导致EIO而非EAGAIN)。
- **BufStatus语义差异**: REM\_OPERATION\_ERR/REM\_ACCESS\_ABORT\_ERR/REM\_DATA\_POISON等远端错误在writev中描述为"Broken pipe"，在readv中描述为"Connection reset by peer"，映射errno均为EIO。WR\_FLUSH\_ERR/WR\_FLUSH\_ERR\_DONE在readv中额外标注"Connection reset by peer"前缀。

### 6.2 汇总映射

> readv场景的UMQ错误码→TCP errno完整映射对照见第七节7.1(kCommonErrnoMappings，统一用于CONNECT/ACCEPT/WRITEV/READV)。BufStatus映射见第七节7.5 Readv差异对比表(kReadvBufStatusMappings, 24项)。

***

## 七、代码实现 vs xlsx文档 对照检查

### 7.1 统一 Errno映射 (kCommonErrnoMappings)

| UMQ错误码                         |  代码映射errno  | xlsx建议errno | 是否一致 | 备注                                                    |
| ------------------------------ | :---------: | :---------: | :--: | ----------------------------------------------------- |
| UMQ\_SUCCESS                   |      0      |      0      |   ✅  | -                                                     |
| UMQ\_ERR\_EPERM (=UMQ\_FAIL)   |     EIO     |     EIO     |   ✅  | UMQ\_FAIL和UMQ\_ERR\_EPERM值相同(-1)，代码只映射UMQ\_ERR\_EPERM |
| UMQ\_ERR\_EAGAIN               |    EAGAIN   |    EAGAIN   |   ✅  | -                                                     |
| UMQ\_ERR\_ENOMEM               |    ENOMEM   |    ENOMEM   |   ✅  | -                                                     |
| UMQ\_ERR\_EBUSY                |    EBUSY    |      -      |   ✅  | umq\_errno.h有定义，代码覆盖更全面                               |
| UMQ\_ERR\_EEXIST               |    EEXIST   |    EEXIST   |   ✅  | -                                                     |
| UMQ\_ERR\_EINVAL               |    EINVAL   |    EINVAL   |   ✅  | -                                                     |
| UMQ\_ERR\_ENODEV               |    ENODEV   |    ENODEV   |   ✅  | -                                                     |
| UMQ\_ERR\_ENOSR                |    ENOSR    |      -      |   ✅  | umq\_errno.h有定义(Jetty/TP资源不足)，代码覆盖更全面                 |
| UMQ\_ERR\_ETIMEOUT             |  ETIMEDOUT  |  ETIMEDOUT  |   ✅  | -                                                     |
| UMQ\_ERR\_EINPROGRESS          | EINPROGRESS | EINPROGRESS |   ✅  | -                                                     |
| UMQ\_ERR\_ETSEG\_NON\_IMPORTED |     EIO     |     EIO     |   ✅  | -                                                     |
| UMQ\_ERR\_EFLOWCTL             |     EIO     |     EIO     |   ✅  | -                                                     |

### 7.2 Writev/Readv Errno映射 (原kCommonIoErrnoMappings — 已合并入kCommonErrnoMappings)

> kCommonIoErrnoMappings与kCommonConnectAcceptErrnoMappings完全一致(13条映射值+描述逐行相同)，已合并为kCommonErrnoMappings。对照见8.1。

### 7.3 ShouldOverrideWithSavedErrno 逻辑对照

代码中的覆盖逻辑（当UMQ返回UMQ\_FAIL/UMQ\_ERR\_EPERM时，用savedErrno覆盖）：

| 条件                                          | 代码行为      | xlsx对照                       | 是否一致 | 备注 |
| ------------------------------------------- | --------- | ---------------------------- | :--: | -- |
| umqRet=UMQ\_FAIL(EPERM), savedErrno=EINVAL  | 返回EINVAL  | EINVAL→EINVAL(urma直接抛出errno) |   ✅  | -  |
| umqRet=UMQ\_FAIL(EPERM), savedErrno=ENODEV  | 返回ENODEV  | ENODEV→ENODEV                |   ✅  | -  |
| umqRet=UMQ\_FAIL(EPERM), savedErrno=ENOMEM  | 返回ENOMEM  | ENOMEM→ENOMEM                |   ✅  | -  |
| umqRet=UMQ\_FAIL(EPERM), savedErrno=ENOEXEC | 返回ENOEXEC | ENOEXEC→ENOEXEC              |   ✅  | -  |
| umqRet=UMQ\_FAIL(EPERM), savedErrno=EIO     | 返回EIO     | EIO                          |   ✅  | -  |
| umqRet=UMQ\_ERR\_ENODEV, savedErrno=EINVAL  | 返回EINVAL  | EINVAL→EINVAL                |   ✅  | -  |
| umqRet=UMQ\_ERR\_ENODEV, savedErrno=EIO     | 返回EIO     | EIO                          |   ✅  | -  |

### 7.4 ConvertHandleResult 逻辑对照

| 操作              | 条件                | 代码行为     | xlsx对照 | 是否一致 | 备注 |
| --------------- | ----------------- | -------- | ------ | :--: | -- |
| CREATE          | savedErrno=EINVAL | 返回EINVAL | EINVAL |   ✅  | -  |
| CREATE          | savedErrno=EPERM  | 返回EPERM  | EPERM  |   ✅  | -  |
| CREATE          | 其他savedErrno      | 返回EIO    | EIO    |   ✅  | -  |
| BIND\_INFO\_GET | savedErrno=ENOMEM | 返回ENOMEM | ENOMEM |   ✅  | -  |
| BIND\_INFO\_GET | savedErrno=EINVAL | 返回EINVAL | EINVAL |   ✅  | -  |
| BIND\_INFO\_GET | 其他savedErrno      | 返回EIO    | EIO    |   ✅  | -  |

### 7.5 Buf Status映射对照

**Connect/Accept (kCommonConnectAcceptBufStatusMappings) — 17项**

| Buf Status                           | 代码映射 | xlsx建议 | 是否一致 | 备注                                                                |
| ------------------------------------ | :--: | :----: | :--: | ----------------------------------------------------------------- |
| UMQ\_BUF\_SUCCESS                    |   0  |    0   |   ✅  | -                                                                 |
| UMQ\_BUF\_UNSUPPORTED\_OPCODE\_ERR   |  EIO |   EIO  |   ✅  | -                                                                 |
| UMQ\_BUF\_LOC\_LEN\_ERR              |  EIO |   EIO  |   ✅  | -                                                                 |
| UMQ\_BUF\_LOC\_OPERATION\_ERR        |  EIO |   EIO  |   ✅  | -                                                                 |
| UMQ\_BUF\_LOC\_ACCESS\_ERR           |  EIO |   EIO  |   ✅  | -                                                                 |
| UMQ\_BUF\_REM\_RESP\_LEN\_ERR        |  EIO |   EIO  |   ✅  | -                                                                 |
| UMQ\_BUF\_REM\_UNSUPPORTED\_REQ\_ERR |  EIO |   EIO  |   ✅  | -                                                                 |
| UMQ\_BUF\_REM\_OPERATION\_ERR        |  EIO |   EIO  |   ✅  | 代码: "Remote operation error"; readv语义: "Connection reset by peer" |
| UMQ\_BUF\_REM\_ACCESS\_ABORT\_ERR    |  EIO |   EIO  |   ✅  | 代码: "Connection reset by peer"                                    |
| UMQ\_BUF\_ACK\_TIMEOUT\_ERR          |  EIO |   EIO  |   ✅  | -                                                                 |
| UMQ\_BUF\_RNR\_RETRY\_CNT\_EXC\_ERR  |  EIO |   EIO  |   ✅  | -                                                                 |
| UMQ\_BUF\_WR\_FLUSH\_ERR             |  EIO |   EIO  |   ✅  | -                                                                 |
| UMQ\_BUF\_WR\_SUSPEND\_DONE          |  EIO |   EIO  |   ✅  | -                                                                 |
| UMQ\_BUF\_WR\_FLUSH\_ERR\_DONE       |  EIO |   EIO  |   ✅  | -                                                                 |
| UMQ\_BUF\_WR\_UNHANDLED              |  EIO |   EIO  |   ✅  | -                                                                 |
| UMQ\_BUF\_LOC\_DATA\_POISON          |  EIO |   EIO  |   ✅  | -                                                                 |
| UMQ\_BUF\_REM\_DATA\_POISON          |  EIO |   EIO  |   ✅  | connect: "Connection reset by peer, remote data poison"           |

**Writev (kWritevBufStatusMappings) — 24项**

| Buf Status                           | 代码映射 | 代码描述                                    | 是否一致 | 备注                     |
| ------------------------------------ | :--: | --------------------------------------- | :--: | ---------------------- |
| UMQ\_BUF\_SUCCESS                    |   0  | Buffer operation success                |   ✅  | -                      |
| UMQ\_BUF\_UNSUPPORTED\_OPCODE\_ERR   |  EIO | Protocol not supported                  |   ✅  | -                      |
| UMQ\_BUF\_LOC\_LEN\_ERR              |  EIO | Message too long                        |   ✅  | -                      |
| UMQ\_BUF\_LOC\_OPERATION\_ERR        |  EIO | Local operation error                   |   ✅  | -                      |
| UMQ\_BUF\_LOC\_ACCESS\_ERR           |  EIO | Permission denied                       |   ✅  | -                      |
| UMQ\_BUF\_REM\_RESP\_LEN\_ERR        |  EIO | Remote response length error            |   ✅  | -                      |
| UMQ\_BUF\_REM\_UNSUPPORTED\_REQ\_ERR |  EIO | Remote unsupported request              |   ✅  | -                      |
| UMQ\_BUF\_REM\_OPERATION\_ERR        |  EIO | Broken pipe                             |   ✅  | writev语义: 对端断开→EPIPE   |
| UMQ\_BUF\_REM\_ACCESS\_ABORT\_ERR    |  EIO | Broken pipe, remote access abort        |   ✅  | writev语义: 对端断开→EPIPE   |
| UMQ\_BUF\_ACK\_TIMEOUT\_ERR          |  EIO | Acknowledgement timeout                 |   ✅  | -                      |
| UMQ\_BUF\_RNR\_RETRY\_CNT\_EXC\_ERR  |  EIO | RNR retry count exceeded                |   ✅  | -                      |
| UMQ\_BUF\_WR\_FLUSH\_ERR             |  EIO | Write flush error                       |   ✅  | -                      |
| UMQ\_BUF\_WR\_SUSPEND\_DONE          |  EIO | Connection aborted                      |   ✅  | -                      |
| UMQ\_BUF\_WR\_FLUSH\_ERR\_DONE       |  EIO | Write flush error done                  |   ✅  | -                      |
| UMQ\_BUF\_WR\_UNHANDLED              |  EIO | Write unhandled                         |   ✅  | -                      |
| UMQ\_BUF\_LOC\_DATA\_POISON          |  EIO | Local data poison                       |   ✅  | -                      |
| UMQ\_BUF\_REM\_DATA\_POISON          |  EIO | Broken pipe, remote data poison         |   ✅  | writev语义: 对端数据中毒→EPIPE |
| UMQ\_BUF\_FLOW\_CONTROL\_UPDATE      |   0  | Flow control update success             |   ✅  | 非错误                    |
| UMQ\_MEMPOOL\_UPDATE\_SUCCESS        |   0  | Mempool update success                  |   ✅  | 非错误                    |
| UMQ\_MEMPOOL\_UPDATE\_FAILED         |  EIO | Mempool update failed                   |   ✅  | -                      |
| UMQ\_IMPORT\_TSEG\_SUCCESS           |   0  | Import TSEG success                     |   ✅  | 非错误                    |
| UMQ\_IMPORT\_TSEG\_FAILED            |  EIO | Import TSEG failed                      |   ✅  | -                      |
| UMQ\_FAKE\_BUF\_FC\_UPDATE           |   0  | Fake buffer flow control update success |   ✅  | 非错误                    |
| UMQ\_FAKE\_BUF\_FC\_ERR              |  EIO | Fake buffer flow control error          |   ✅  | -                      |

**Readv (kReadvBufStatusMappings) — 24项**

与Writev主要差异在远端错误的语义描述（writev用"Broken pipe"，readv用"Connection reset by peer"）：

| Buf Status                        | 代码映射 | 代码描述                                             | 差异说明                            |
| --------------------------------- | :--: | ------------------------------------------------ | ------------------------------- |
| UMQ\_BUF\_REM\_OPERATION\_ERR     |  EIO | Connection reset by peer                         | writev为"Broken pipe"            |
| UMQ\_BUF\_REM\_ACCESS\_ABORT\_ERR |  EIO | Connection reset by peer, remote access abort    | writev为"Broken pipe"            |
| UMQ\_BUF\_WR\_FLUSH\_ERR          |  EIO | Connection reset by peer, write flush error      | writev为"Write flush error"      |
| UMQ\_BUF\_WR\_FLUSH\_ERR\_DONE    |  EIO | Connection reset by peer, write flush error done | writev为"Write flush error done" |
| UMQ\_BUF\_REM\_DATA\_POISON       |  EIO | Connection reset by peer, remote data poison     | writev为"Broken pipe"            |

***

## 八、待确认项汇总

无待确认项，所有映射已补全。

***

## 附录A: Linux errno 检索表 (aarch64)

> 数值来自 Linux `asm-generic/errno-base.h` + `asm-generic/errno.h`（aarch64/x86\_64/所有新架构共用同一源文件，编号相同）。alpha/sparc/mips/parisc 等遗留架构有独立编号，与本文无关。
> 仅收录本文档实际出现的 errno 符号。完整列表参见 [errno(3)](https://man7.org/linux/man-pages/man3/errno.3.html)。

| errno 符号     | 数值 | 标准解释                              | 本文用途                                              |
| ----------- | --: | --------------------------------- | ------------------------------------------------- |
| EPERM       |   1 | Operation not permitted           | UMQ\_ERR\_EPERM 映射目标; CREATE 透传; override 白名单    |
| EIO         |   5 | Input/output error                | 默认兜底映射; override 白名单; BufStatus 默认映射             |
| ENOEXEC     |   8 | Exec format error                 | override 白名单 (UMQ\_FAIL 场景)                       |
| EBADF       |   9 | Bad file descriptor               | FindErrno fallback 罕见边缘场景                          |
| EAGAIN      |  11 | Resource temporarily unavailable  | UMQ\_ERR\_EAGAIN 映射目标; rearm 成功路径直接设置            |
| ENOMEM      |  12 | Cannot allocate memory            | UMQ\_ERR\_ENOMEM 映射目标; override 白名单; BIND\_INFO\_GET 透传 |
| EBUSY       |  16 | Device or resource busy           | UMQ\_ERR\_EBUSY 映射目标                              |
| EEXIST      |  17 | File exists                       | UMQ\_ERR\_EEXIST 映射目标; umq\_dev\_add 跳过-EEXIST   |
| ENODEV      |  19 | No such device                    | UMQ\_ERR\_ENODEV 映射目标; override 白名单               |
| EINVAL      |  22 | Invalid argument                  | UMQ\_ERR\_EINVAL 映射目标; override 白名单; CREATE/BIND\_INFO\_GET 透传 |
| EPIPE       |  32 | Broken pipe                       | writev BufStatus 远端错误语义描述                          |
| ENOSR       |  63 | No STREAM resources               | UMQ\_ERR\_ENOSR 映射目标 (Jetty/TP)                    |
| ETIMEDOUT   | 110 | Connection timed out              | UMQ\_ERR\_ETIMEOUT 映射目标; PrefillRx 超时 override   |
| EINPROGRESS | 115 | Operation now in progress         | UMQ\_ERR\_EINPROGRESS 映射目标                         |
