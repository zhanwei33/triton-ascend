# UBSocket UT覆盖率Sprint — 启动指南与认领表

> 本文档是Sprint的**唯一入口**：从快速上手到文件认领，读完这份即可开工。
> 深入参考: 架构→`UBSOCKET-ARCHITECTURE.ch.md`, 覆盖率数据→`UBSOCKET-COVERAGE-ANALYSIS.ch.md`
> 协调者负责更新认领表和里程碑；同事认领/完成时通知协调者。

***

## 目标

将 `src/ubsocket/csrc/` 单元测试覆盖率从 **11.1%行/5.3%分支** 提升至 **≥80%行/≥50%分支**。

***

## 快速上手(5步)

### 1. 拉代码 & 构建

```bash
git pull
UMQ_BUILD=on UBSOCKET_UT=on UBSOCKET_COVERAGE=on bash build/build_umq_and_ubsocket.sh
ctest --test-dir src/ubsocket/build --output-on-failure   # 验证已有143个case全部PASS
```

### 2. 读架构文档(必读)

读 `doc/ubsocket/UBSOCKET-ARCHITECTURE.ch.md`，理解五层架构、Socket对象层次、状态机、关键调用链。**不读这个写不出正确的UT。**

需要覆盖率数据细节时再翻 `UBSOCKET-COVERAGE-ANALYSIS.ch.md`。

### 3. 认领文件

在下方的认领表里填你的名字和test binary名。规则:
- 每人同一时间只认领一个 `.cpp` 文件
- 优先从零覆盖率表(#1→#22)选，easy优先快速出活
- 状态流转: `unclaimed` → `in_progress` → `review` → `done`
- 完成时记录实际达到的行覆盖率

### 4. 写UT

参考 `src/ubsocket/unit_test/` 下已有test binary学习代码风格:

| Binary | Cases数 | 类型 | 学习重点 |
|--------|---------|------|----------|
| `umq_errno_converter_test` | 91 | converter级纯逻辑 | 最简单，无mock依赖 |
| `umq_ops_errno_test` | 51 | ops级mock测试 | mockcpp典型模式 |
| `mock_infrastructure_test` | 32 | mock桩验证 | Helper/Static/Block/Socket四种mock方式 |
| `iobuf_zcopy_adapter_test` | 45 | iobuf模块全覆盖 | BlockMem/ZcopyAdapter/DynSymScanner |

### 5. 构建验证

依赖匹配的 `umdk` 包，从 [atomgit.com/src-openeuler/umdk](https://atomgit.com/src-openeuler/umdk/tree/openEuler-24.03-LTS-SP3) 下载 `umdk-openEuler-24.03-LTS-SP3.zip` 并安装（安装方法联系 fanzhaonan）。

```bash
UMQ_BUILD=on UBSOCKET_UT=on UBSOCKET_COVERAGE=on bash build/build_umq_and_ubsocket.sh
ctest --test-dir src/ubsocket/build --output-on-failure
# 覆盖率报告: src/ubsocket/build/coverage_report/index.html
```

***

## mock基础设施

所有桩已就绪(`src/ubsocket/unit_test/stub/`)，epoll依赖不再是阻塞项。

**设计原则: helper是简单case的快捷方式，不是唯一入口。** 复杂场景(多步序列/自定义状态)可直接在test case中手写mock环境，绕过helper的固定行为。

| 桩 | 位置 | 快捷用法 | 灵活绕过方式 |
|----|------|----------|------------|
| `socket_test_helper.h` | stub/ | `MakeTestUmqSocket(fd, handle, state, shareHandle)` 构造SocketPtr; `DestroyTestSocketOps()` 清理 | 手写MakeRef→设private字段→自定义ops/state/shareHandle; 参数支持state(INIT/RAW_ESTABLISHED/ESTABLISHED/SHUTDOWN)和shareUmqHandle(JFR场景) |
| `umq_api_helper.h` | stub/ | `AllocMockBufWithBlock(size)` 8K对齐Block+umq_buf_t(8槽池); `AllocMockBuf(size)` 简单buf | 手写umq_buf_t字段+8K对齐placement new Block; 自定义opcode/nshared/data_size等字段 |
| `libc_api_helper.h` | stub/ | `MakeTestSockAddr()` + FD常量; LibcApi `_ptr` 函数指针替换 | test case中直接设`_ptr`字段为自定义mock函数; MockOpenSuccess/MockOpenFail仅为示例 |
| `dl_helper.h` | stub/ | TEST_LIB_PATH/TEST_SYM_NAME/TEST_DL_HANDLE常量 | test case中定义局部常量 |
| `fake_epoll_static` | stub/fake_epoll/ | `FakeEpollCtl` API: SetNext*/AllocFakeFd/ReleaseFakeFd/Reset | 多步epoll_wait序列需多次SetNext; 更复杂序列可扩展FakeEpollState增加队列 |
| LockRegistry注入 | 生产代码内 | 已有函数指针表，无需额外桩 | — |

***

## 关键规则(违反必返工)

1. **errno必须在调用被测函数之前设置** — 生产代码在UMQ API调用后立即保存errno，之后才设errno会导致断言错误
2. **mockcpp `.stubs()` 默认返回void** — 非void函数必须加 `.will(returnValue(...))`
3. **`LibcApi::open` 是variadic** — mockcpp无法mock variadic函数，改用 `LibcApi::_ptr` 函数指针替换(见 `libc_api_helper.h`)
4. **`LibcApi::_ptr` 初始为nullptr** — 未调用 `LibcApi::Load()` 时通过nullptr函数指针调用→segfault，SetUp中必须设置 `_ptr` 或调用 `Load()`
5. **`UmqErrnoConverter` 是冻结/final** — 永远不要修改它的头文件
6. **构造依赖lock/socket的对象前必须先初始化** — `LockRegistry::RegisterDefaultOps()` + `SocketSet::Instance().Init()`，否则crash
7. **`EidRegistry` 是 LeakySingleton** — 测试结束需 `UnregisterEid()` 清理
8. **`UmqBackend::UMQ_INITED` 是 static bool** — SetUp中设为false重置
9. **mockcpp `Result` vs `ock::ubs::Result` 类型冲突** — 用 `using namespace ock::ubs;` + 显式限定 `ock::ubs::Result`
10. **每个使用mock的test case末尾必须调用 `GlobalMockObject::verify()`** — 重置mock状态，否则下一个case受影响

更完整的陷阱列表(22条)见 `.opencode/skills/ut-gen/SKILL.md` §常见陷阱。

***

## 文件认领 — 按难度推荐起步

| 难度 | 推荐起步# | 说明 |
|------|-----------|------|
| easy | #8, #14, #22 | 几乎纯getter/setter，mock简单 |
| easy | #3, #4 | dlopen/dlsym逻辑，参考iobuf_test |
| medium | #7, #5 | LibcApi `_ptr`路径，已有helper |
| hard | #1, #2, #6 | 深度依赖链，有经验后再认领 |

***

## 文件认领表

| # | 模块 | 文件 | 行数 | 分支数 | 当前行% | 当前分支% | mock难度 | 认领人 | 状态 | 达成(行/分支) |
|---|------|------|------|--------|---------|----------|---------|--------|------|--------------|
| 1 | core/umq | umq_socket_connector.cpp | 410 | 640 | 23.4% | 29.4% | hard | | unclaimed | |
| 2 | core/socket | ubsocket_event_epoll.cpp | 404 | 500 | 0% | 0% | hard | | unclaimed | |
| 3 | under_api/urma | dl_urma_api.cpp | 267 | 258 | 0% | 0% | easy | | unclaimed | |
| 4 | iobuf | ubsocket_zcopy_adapter.cpp | 242 | 278 | 0% | 0% | easy | | unclaimed | |
| 5 | core/socket | ubsocket_socket_helper.cpp | 181 | 282 | 0% | 0% | medium | | unclaimed | |
| 6 | entry | ubsocket_sock.cpp | 143 | 132 | 0% | 0% | hard | | unclaimed | |
| 7 | under_api | dl_libc_api.cpp | 124 | 130 | 0% | 0% | medium | | unclaimed | |
| 8 | core/umq | umq_setting.cpp | 115 | 106 | 0% | 0% | easy | | unclaimed | |
| 9 | core/socket | ubsocket_socket_acceptor.cpp | 114 | 162 | 0% | 0% | medium | | unclaimed | |
| 10 | core/umq | umq_epoll_runner_ops.cpp | 112 | 162 | 0% | 0% | hard | | unclaimed | |
| 11 | entry | ubsocket.cpp | 107 | 126 | 0% | 0% | hard | | unclaimed | |
| 12 | core/socket | ubsocket_socket.cpp | 91 | 110 | 0% | 0% | medium | | unclaimed | |
| 13 | common | ubsocket_thread_pool.cpp | 78 | 106 | 0% | 0% | medium | | unclaimed | |
| 14 | common | ubsocket_global_setting.cpp | 69 | 78 | 0% | 0% | easy | | unclaimed | |
| 15 | core/socket | ubsocket_data_rx.cpp | 66 | 52 | 0% | 0% | easy | | unclaimed | |
| 16 | core/socket | ubsocket_wakeup_event.cpp | 63 | 62 | 0% | 0% | medium | | unclaimed | |
| 17 | core/socket | ubsocket_data_tx.cpp | 43 | 56 | 0% | 0% | easy | | unclaimed | |
| 18 | entry | ubsocket_epoll.cpp | 37 | 32 | 0% | 0% | hard | | unclaimed | |
| 19 | under_api | dl_api.cpp | 25 | 28 | 0% | 0% | medium | | unclaimed | |
| 20 | core/socket | ubsocket_socket_connector.cpp | 24 | 32 | 0% | 0% | easy | | unclaimed | |
| 21 | core/socket | ubsocket_core_types.cpp | 21 | 66 | 0% | 0% | easy | | unclaimed | |
| 22 | common | ubsocket_signal_handler.cpp | 4 | 16 | 100% | ~50% | easy | | unclaimed | |
| 23 | core/umq | umq_data_tx_ops.cpp | 374 | 394 | 9.1% | 6.3% | hard | | unclaimed | |
| 24 | core/umq | umq_socket.cpp | 293 | 408 | 5.5% | 2.9% | hard | | unclaimed | |
| 25 | core/umq | umq_socket_acceptor.cpp | 193 | 306 | 7.3% | 4.9% | hard | | unclaimed | |
| 26 | core/umq | umq_data_rx_ops.cpp | 217 | 304 | 18.0% | 9.9% | hard | | unclaimed | |
| 27 | core/umq | umq_eid_table.h | 122 | 106 | 18.0% | 6.6% | easy | | unclaimed | |
| 28 | common | ubsocket_lock.cpp | 131 | 174 | 29.8% | 6.9% | medium | | unclaimed | |

***

## 覆盖率里程碑

| 里程碑 | 目标 | 达成日期 | 实际值 |
|--------|------|----------|--------|
| 基线 | Phase 0 | 2026-05-27 | 11.1%行 / 5.3%分支 |
| 30%行覆盖率 | Phase 1中期 | _待定_ | _待定_ |
| 50%行 / 25%分支 | Phase 2中期 | _待定_ | _待定_ |
| 80%行 / 50%分支 | Sprint结束 | _待定_ | _待定_ |