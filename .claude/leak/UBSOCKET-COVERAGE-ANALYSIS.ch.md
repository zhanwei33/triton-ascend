# UBSocket csrc UT 覆盖率基线与提升计划

> 最后更新: 2026-05-27
> 关联文档: `doc/ubsocket/UBSOCKET-CLAIMING.md`, `doc/ubsocket/UBSOCKET-ARCHITECTURE.ch.md`
> 关联skill: `.opencode/skills/ut-gen/SKILL.md`, `.opencode/skills/ut-coverage-coord/SKILL.md`
> 关联子skill: `.opencode/skills/ut-gen-umq/SKILL.md`, `.opencode/skills/ut-gen-core/SKILL.md`, `.opencode/skills/ut-gen-common/SKILL.md`, `.opencode/skills/ut-gen-under-api/SKILL.md`, `.opencode/skills/ut-gen-profiling/SKILL.md`
> 关联项目上下文: `AGENTS.md` (Coverage Baseline section)

***

## 一、覆盖率基线

### 1.1 构建命令

```bash
UMQ_BUILD=on UBSOCKET_UT=on UBSOCKET_COVERAGE=on bash build/build_umq_and_ubsocket.sh
```

### 1.2 总体覆盖率 (2026-05-27 baseline, iobuf UT后更新)

| 指标 | 基线值 | iobuf后 | 目标 | 缺口 |
|------|--------|---------|------|------|
| 行覆盖率 | 11.1% (623/5637) | 15.1% (850/5637) | ≥80% | +3650行 |
| 函数覆盖率 | 19.0% (117/615) | 27.0% (166/615) | — | +449函数 |
| 分支覆盖率 | 5.3% (361/6861) | 7.0% (479/6861) | ≥50% | +2953分支 |

### 1.3 模块级覆盖率明细

| 模块 | 行% | 总行 | 已覆盖行 | 函数% | 总函数 | 分支% | 总分支 | 文件数 | 达80%需增行 |
|------|-----|------|---------|-------|--------|-------|--------|--------|------------|
| core/umq | 12.0% | 2148 | 258 | 0.6% | 160 | 0.0% | 2693 | 19 | +1460 |
| core/socket | 2.4% | 1452 | 35 | 0.6% | 157 | 0.0% | 1768 | 22 | +1126 |
| common | 20.1% | 642 | 129 | 4.7% | 149 | 0.0% | 1062 | 16 | +384 |
| iobuf | 0.0%→57.3% | 343 | 0→196 | 0%→79% | 29 | 0%→23% | 328 | 3 | +77 |
| entry | 0.0% | 294 | 0 | 0.0% | 39 | 0.0% | 290 | 4 | +235 |
| under_api/urma | 0.0% | 267 | 0 | 0.0% | 3 | 0.0% | 258 | 1 | +213 |
| under_api | 5.2% | 269 | 14 | 0.0% | 57 | 0.0% | 168 | 5 | +201 |
| profiling | 84.2% | 222 | 187 | 61.9% | 21 | 0.0% | 294 | 9 | +0 (已达标) |

### 1.4 覆盖率报告产出

| 产物 | 路径 | 说明 |
|------|------|------|
| HTML 报告 | `src/ubsocket/build/coverage_report/index.html` | 文件级+行级可视化 |
| 汇要 TXT | `src/ubsocket/build/coverage_summary.txt` | 总览 3 行数字 |
| 详细 TXT | `src/ubsocket/build/coverage_detailed.txt` | 每文件行/函数/分支率 |
| 过滤后 info | `src/ubsocket/build/coverage_filtered.info` | lcov tracefile |

***

## 二、文件完备性验证

### 2.1 覆盖率报告覆盖范围

报告共 79 个文件，涵盖 csrc 目录下所有含可执行代码且参与当前构建的文件。

### 2.2 未纳入的 24 个文件及原因

#### A. 条件编译未参与 (10个，`BUILD_URMA_DLOPEN_BACKEND=OFF` / `BUILD_UMQ_DLOPEN_BACKEND=OFF`)

| 文件 | 原因 |
|------|------|
| csrc/core/urma/urma_backend.cpp | URMA_DLOPEN_BACKEND 未开启 |
| csrc/core/urma/urma_backend.h | 同上 |
| csrc/core/urma/urma_setting.cpp | 同上 |
| csrc/core/urma/urma_setting.h | 同上 |
| csrc/core/urma/urma_socket.cpp | 同上 |
| csrc/core/urma/urma_socket.h | 同上 |
| csrc/core/urma/urma_socket_types.h | 同上 |
| csrc/core/urma/urma_wrapper.cpp | 同上 |
| csrc/core/urma/urma_wrapper.h | 同上 |
| csrc/under_api/dl_umq_api.cpp | UMQ_DLOPEN_BACKEND 未开启，代码在 `#ifdef UMQ_DLOPEN_BACKEND_ENABLED` 内 |

#### B. 纯声明头文件 — 无可执行行，gcov 报 "No executable lines" (13个)

| 文件 | 内容类型 |
|------|----------|
| csrc/common/ubsocket_common_includes.h | 纯 #include 聚合 |
| csrc/common/ubsocket_defines.h | typedef + enum + struct + 宏 |
| csrc/common/ubsocket_errno.h | enum 错误码 |
| csrc/common/ubsocket_version.h | 宏版本号 |
| csrc/common/ubsocket_signal_handler.h | 函数声明 |
| csrc/core/ubsocket_wakeup_event.h | 函数声明 |
| csrc/core/umq/umq_epoll_ops.h | 函数声明 |
| csrc/core/umq/umq_qbuf_list.h | 类声明(无inline实现) |
| csrc/core/umq/umq_setting.h | 类声明(无inline实现) |
| csrc/under_api/umq_api.h | 函数声明 |
| csrc/under_api/urma/dl_urma_api.h | 函数声明(967行，但全是声明无实现) |
| csrc/under_api/urma/urma_opcode.h | enum 操作码 |
| csrc/under_api/urma/urma_types.h | typedef + struct |

#### C. 无可执行行的 .cpp (1个)

| 文件 | 原因 |
|------|------|
| csrc/ubsocket_cntl.cpp | 仅含 2 行 `#include`，无函数定义 |

### 2.3 include/ 目录 (5个头文件)

| 文件 | 行数 | 不纳入原因 |
|------|------|-----------|
| include/ubsocket.h | 75 | 纯 C API 函数声明 |
| include/ubsocket_def.h | 84 | typedef + struct + 宏 |
| include/ubsocket_ctnl.h | 28 | 纯 C 函数声明 |
| include/ubsocket_epoll.h | 30 | 纯 C 函数声明 |
| include/ubsocket_sock.h | 51 | 纯 C 函数声明 |

以上 5 个文件共 268 行，全部是 extern "C" 函数声明和类型定义，不含可执行代码。

### 2.4 结论

**csrc 目录下所有含可执行代码且参与当前构建的文件已 100% 纳入覆盖率报告**。未纳入的文件分三类：(A) 条件编译排除、(B) 纯声明无可执行行、(C) 仅含 #include。这些文件不需要 UT，也无法被覆盖率工具追踪。

***

## 三、覆盖率基础设施 (CMake 配置)

### 3.1 CMakeLists.txt 关键配置

位置: `src/ubsocket/CMakeLists.txt`

```
行 15: option(UBSOCKET_ENABLE_COVERAGE "Enable coverage reporting for ubsocket UT" OFF)
行 57-61: --coverage -fprofile-arcs -ftest-coverage 编译/链接选项
行 126-183: coverage custom target
```

### 3.2 coverage target 流程

1. **zerocounters** — 清零旧 .gcda，防止增量构建覆盖率虚高
2. **ctest** — 运行所有 UT
3. **initial capture** — `--initial --capture` 采集零覆盖率基线(无 .gcda 的文件也纳入)
4. **runtime capture** — `--capture` 采集运行后覆盖率
5. **combine** — `-a initial -a runtime` 合并
6. **filter** — 排除 `_deps/` `/usr/include/` `3rdparty/` `unit_test/` `tools/`
7. **TXT summary + detailed** — `--summary` + `--list` 输出文本报告
8. **genhtml** — HTML 报告 + branch-coverage

### 3.3 lcov 版本与兼容性

- 环境: lcov 1.16, gcov 12.3.1 (GCC 12, aarch64)
- `--ignore-errors gcov` — lcov 1.16 仅支持 `gcov, source, graph` 三个错误类别
- backup 版旧配置中 `--ignore-errors unused,range` **不适用于 lcov 1.16**（会报 unknown argument 错误）
- `--rc lcov_branch_coverage=1` — 必须显式开启分支覆盖率采集

### 3.4 从 backup CMakeLists.txt 同步的改进

| 改进 | 来源 | 说明 |
|------|------|------|
| --zerocounters | backup | 每次跑覆盖率前清零，防旧 .gcda 累积 |
| --ignore-errors gcov | backup(adapted) | backup 用 unused,range，适配为 lcov 1.16 的 gcov |
| TXT summary/detailed | backup | `--summary` + `--list` 输出，便于 CI 快速查看 |
| --initial capture | 新增 | backup 无此步骤，零覆盖率文件会被遗漏 |

***

## 四、零覆盖率文件优先级排序

### 4.1 零覆盖率 .cpp 文件 (22个，按行数降序)

| # | 模块 | 文件 | 行数 | 函数数 | 分支数 | Mock难度 |
|---|------|------|------|--------|--------|----------|
| 1 | core/umq | umq_socket_connector.cpp | 410 | 21 | 640 | hard(umq:6+sock:7) |
| 2 | core/socket | ubsocket_event_epoll.cpp | 404 | 29 | 500 | hard(epoll:14+pthread:2) |
| 3 | under_api/urma | dl_urma_api.cpp | 267 | 3 | 258 | easy(dl:3) |
| 4 | iobuf | ubsocket_zcopy_adapter.cpp | 242 | 19 | 278 | easy(umq_buf) |
| 5 | core/socket | ubsocket_socket_helper.cpp | 181 | 11 | 282 | medium(sock:5) |
| 6 | entry | ubsocket_sock.cpp | 143 | 24 | 132 | hard(sock:15) |
| 7 | under_api | dl_libc_api.cpp | 124 | 3 | 130 | medium(dl:3) |
| 8 | core/umq | umq_setting.cpp | 115 | 8 | 106 | easy(无外部调用) |
| 9 | core/socket | ubsocket_socket_acceptor.cpp | 114 | 8 | 162 | medium(sock:2) |
| 10 | core/umq | umq_epoll_runner_ops.cpp | 112 | 2 | 162 | hard(umq:14) |
| 11 | entry | ubsocket.cpp | 107 | 9 | 126 | hard(综合依赖) |
| 12 | core/socket | ubsocket_socket.cpp | 91 | 6 | 110 | medium(sock:2) |
| 13 | common | ubsocket_thread_pool.cpp | 78 | 10 | 106 | medium(pthread:2) |
| 14 | common | ubsocket_global_setting.cpp | 69 | 4 | 78 | easy(无外部调用) |
| 15 | core/socket | ubsocket_data_rx.cpp | 66 | 4 | 52 | easy |
| 16 | core/socket | ubsocket_wakeup_event.cpp | 63 | 6 | 62 | medium(epoll:2) |
| 17 | core/socket | ubsocket_data_tx.cpp | 43 | 2 | 56 | easy |
| 18 | entry | ubsocket_epoll.cpp | 37 | 5 | 32 | hard(epoll:6) |
| 19 | under_api | dl_api.cpp | 25 | 2 | 28 | medium(dl:3) |
| 20 | core/socket | ubsocket_socket_connector.cpp | 24 | 2 | 32 | easy |
| 21 | core/socket | ubsocket_core_types.cpp | 21 | 6 | 66 | easy |
| 22 | common | ubsocket_signal_handler.cpp | 4 | 1 | 16 | easy |

### 4.2 零覆盖率 .h 文件 (inline/template，按行数降序取 top 10)

| # | 模块 | 文件 | 行数 | 函数数 | 分支数 |
|---|------|------|------|--------|--------|
| 1 | common | ubsocket_setting_validator.h | 111 | 8 | 226 |
| 2 | core/socket | ubsocket_qbuf_queue.h | 111 | 9 | 132 |
| 3 | iobuf | ubsocket_iobuf.h | 93 | 6 | 42 |
| 4 | core/socket | ubsocket_event_epoll.h | 88 | 24 | 48 |
| 5 | under_api | dl_libc_api.h | 62 | 27 | 4 |
| 6 | core/socket | ubsocket_ring_buffer.h | 57 | 6 | 46 |
| 7 | common | ubsocket_set.h | 38 | 6 | 34 |
| 8 | common | ubsocket_functions.h | 32 | 0 | 142 |
| 9 | core/socket | ubsocket_buf_converter.h | 32 | 8 | 24 |
| 10 | core/umq | umq_buf_converter.h | 30 | 4 | 16 |

### 4.3 有部分覆盖但缺口大的文件

| 模块 | 文件 | 当前行% | 总行 | 需增行(达80%) | Mock难度 |
|------|------|--------|------|--------------|----------|
| core/umq | umq_data_tx_ops.cpp | 9.3% | 375 | +276 | hard(umq:18) |
| core/umq | umq_socket.cpp | 5.5% | 293 | +220 | hard(umq:31+epoll:3+sock:2) |
| core/umq | umq_socket_acceptor.cpp | 7.6% | 185 | +133 | hard(umq:6+sock:3) |
| core/umq | umq_data_rx_ops.cpp | 18.0% | 217 | +132 | hard(umq:19) |
| core/umq | umq_eid_table.h | 18.0% | 122 | +73 | easy(纯inline) |
| common | ubsocket_lock.cpp | 29.8% | 131 | +69 | medium(pthread:18) |

***

## 五、Mock 依赖分析

### 5.1 外部 API 调用统计 (按 mock 难度降序)

| 难度 | 文件 | 外部调用总数 | Mock 类别明细 |
|------|------|-------------|---------------|
| hard | umq_socket.cpp | 36 | umq:31, epoll:3, sock:2 |
| hard | ubsocket_event_epoll.cpp | 16 | epoll:14, pthread:2 |
| hard | umq_data_rx_ops.cpp | 19 | umq:19 |
| hard | ubsocket_lock.cpp | 18 | pthread:18 |
| hard | umq_data_tx_ops.cpp | 18 | umq:18 |
| hard | umq_epoll_runner_ops.cpp | 14 | umq:14 |
| hard | umq_socket_connector.cpp | 13 | umq:6, sock:7 |
| hard | ubsocket_sock.cpp | 15 | sock:15 |
| hard | umq_socket_acceptor.cpp | 9 | umq:6, sock:3 |
| hard | umq_backend.cpp | 8 | umq:8 |
| hard | ubsocket_epoll.cpp | 6 | epoll:6 |
| medium | ubsocket_socket_helper.cpp | 5 | sock:5 |
| medium | dl_libc_api.cpp | 3 | dl:3 |
| medium | dl_umq_api.cpp | 3 | dl:3 |
| medium | dl_urma_api.cpp | 3 | dl:3 |
| medium | ubsocket_thread_pool.cpp | 2 | pthread:2 |
| medium | ubsocket_socket_acceptor.cpp | 2 | sock:2 |
| medium | ubsocket_wakeup_event.cpp | 2 | epoll:2 |
| easy | umq_setting.cpp | 0 | 无外部调用 |
| easy | ubsocket_global_setting.cpp | 0 | 无外部调用 |
| easy | ubsocket_data_rx.cpp | 0 | 无外部调用 |
| easy | ubsocket_data_tx.cpp | 0 | 无外部调用 |
| easy | ubsocket_zcopy_adapter.cpp | 1 | 内部依赖 |

### 5.2 Mock 桩基础设施现状

| 桩 | Mock 覆盖的 API | 影响模块 | 优先级 | 形式 | 状态 |
|----|-----------------|----------|--------|------|------|
| umq_api_helper.h | umq_* API测试常量+AllocMockBuf | core/umq | P0 | helper头文件 | done |
| libc_api_helper.h | LibcApi测试常量+MakeTestSockAddr+MakeTestEpollEvent | core/socket, entry | P0 | helper头文件 | done |
| dl_helper.h | dlopen测试常量 | under_api | P2 | helper头文件 | done |
| fake_epoll_static | epoll_create1/ctl/wait/pwait + eventfd/write/read + close(dlsym转发) | core/socket, entry, core/umq | P0 | 静态库 | done |
| LockRegistry注入 | LOCK_OPS/RW_LOCK_OPS函数指针表 | common, core/socket | P1 | 已有基础设施 | done |
| pthread不需要mock | `ubsocket_lock.cpp`通过LOCK_OPS函数指针注入 | common | P1 | 无需mock | N/A |
| socket/bind/listen等 | LibcApi::_ptr函数指针(mockcpp或lambda) | core/socket, core/umq, entry | P1 | mockcpp+helper | done |

***

## 六、任务分发准备清单

### 6.1 人员分配建议 (16人)

| 模块 | 分配人数 | 需覆盖行数 | 难度 | 关键 Mock |
|------|---------|-----------|------|----------|
| core/umq | 5-6人 | +1460行 | hard | UMQ API + epoll + socket ops |
| core/socket | 4-5人 | +1126行 | hard | epoll + socket ops + pthread |
| common | 2人 | +384行 | medium | pthread (lock/thread_pool) |
| iobuf | 1人 | +274行 | easy | UMQ buf (zcopy) |
| entry | 1人 | +235行 | hard | 依赖 core/umq 和 core/socket (建议延后) |
| under_api + urma | 1人 | +414行 | easy | dlopen |
| profiling | 0人 | 已达标84.2% | done | — |

**建议**: core/umq 和 core/socket 是主力战场(共 +2586 行)，分配 10-11 人。entry 建议等 core 模块进展后再做(依赖关系强)。

### 6.2 分发前必须完成的 5 项准备

#### P0: Mock 桩基础设施 (已完成)

`unit_test/stub/` 目录已建立:
- `umq_api_helper.h` — AllocMockBuf + MakeTest* 工厂函数 + 测试常量
- `libc_api_helper.h` — MakeTestSockAddr + MakeTestEpollEvent + 测试常量
- `dl_helper.h` — dlopen测试常量
- `fake_epoll/fake_epoll.h + fake_epoll.cpp` — 静态库, 链接时替换epoll/eventfd/close, `close()`对非fake fd通过`dlsym(RTLD_NEXT)`转发
- `LockRegistry::RegisterDefaultOps()` — 函数指针注入, pthread无需mock

#### P1: 新测试二进制 CMake 模板 (已完成)

当前5个测试二进制: umq_errno_converter_test(91), umq_ops_errno_test(51), mock_infrastructure_test(32), iobuf_zcopy_adapter_test(45), profiling_test(1)。
CMake模板在`.opencode/skills/ut-gen/SKILL.md`中已文档化(Converter-only和Ops级两种模式)。
Ops级测试必须链接`fake_epoll_static`。

```
unit_test/
  CMakeLists.txt               — 顶层 add_subdirectory 各模块
  stub/                         — 共享 mock 桩
    CMakeLists.txt
    umq_stub.h / umq_stub.cpp
    epoll_stub.h / epoll_stub.cpp
    ...
  umq_socket_test/
    CMakeLists.txt
    umq_socket_connector_test.cpp
    umq_socket_test.cpp
  core_socket_test/
    CMakeLists.txt
    ubsocket_event_epoll_test.cpp
    ...
  common_test/
    ...
```

#### P2: 文件认领机制

在共享文档维护认领表，防止两人同时写同一 .cpp 的 UT:

```
| 文件 | 认领人 | 状态 | 当前行覆盖率 | 目标行覆盖率 |
|------|--------|------|------------|------------|
| umq_socket_connector.cpp | 张三 | in_progress | 0% | 80% |
```

#### P3: 覆盖率验证工作流标准化

每位同事必须知道:
- 构建命令: `UMQ_BUILD=on UBSOCKET_UT=on UBSOCKET_COVERAGE=on bash build/build_umq_and_ubsocket.sh`
- 查看覆盖率: `coverage_summary.txt` + `coverage_detailed.txt` + HTML 报告
- 只跑自己的 test: `ctest --test-dir build -R <test_name>`
- 覆盖率目标: 行≥80%、分支≥50%

#### P4: 已完成 — CLAIMING.md已包含构建/运行/覆盖率命令

构建/运行/覆盖率命令已纳入 `doc/ubsocket/UBSOCKET-CLAIMING.md` 快速上手章节和关键规则。

### 6.3 执行顺序

```
P0 Mock桩 → P1 CMake模板 → P2 认领表 → P3 工作流文档 → P4 已完成 → 开始分发任务
```

***

## 七、URMA 后端状态说明

`BUILD_URMA_DLOPEN_BACKEND=ON` 当前**不可实际启用**:

1. `UrmaSocket` 是空壳 — 无 Initialize/TX/RX 实现
2. `urma_setting.cpp` 全是 no-op
3. `urma_wrapper.h:15` 引用 `#include "under_api/urma/urma_api_dl.h"` — 该文件不存在(实际叫 `dl_urma_api.h`)，启用编译会直接报错
4. `URMA_BACKEND_ENABLED` 宏已定义但无代码使用

urma 9 个源文件 + dl_urma_api.cpp 属于早期脚手架，不需 UT，不需纳入覆盖率。

***

## 八、已知陷阱 (覆盖率相关)

| 陷阱 | 说明 | 解决方案 |
|------|------|----------|
| lcov `--ignore-errors unused,range` | lcov 1.16 不支持此语法，会报 unknown argument 错误 | 使用 `--ignore-errors gcov` |
| `*/mockcpp/*` 过滤不匹配 `sj_mockcpp-src/src/` | `sj_mockcpp-src` 不是 `mockcpp` 目录名，源文件漏过滤 | 改用 `*/_deps/*` 一并过滤所有 FetchContent 依赖 |
| `--directory` 范围过窄 | 仅指向 `build/csrc` 时，部分 OBJECT 库的 gcno 不被采集 | 改为 `build` (CMAKE_BINARY_DIR)，让 lcov 递归搜索 |
| 零覆盖率文件默认不纳入 | lcov `--capture` 只处理有 .gcda 的文件，无 .gcda 的文件被跳过 | 增加 `--initial --capture` 步骤采集零覆盖率基线 |
| coverage target 中括号 | `cmake -E echo` 不支持括号等 shell 特殊字符 | 去掉消息中的括号 |
| mockcpp::Result vs ock::ubs::Result | 同名类型冲突 | `using namespace ock::ubs;` + 显式限定 |
| IO_SIZE_MB macro vs constexpr | 头文件已定义 macro，测试不可重定义 | 使用 header macro |
| UMQ_INITED static bool | Backend 状态需重置 | SetUp 中设为 false |
| EidRegistry LeakySingleton | 需清理 | TearDown 中 UnregisterEid |
| errno 必须在调用前设置 | 生产代码在 UMQ API 调用后立即保存 errno | 测试中 MOCKER 设置后、调用前设 errno |
| Share-JFR handle 语义 | PrefillRx 中本地 umq_handle 在 share JFR=true 时指向主 UMQ | 传 umq_handle_ 而非本地 umq_handle |

***

## 九、Skill 文件体系与使用说明

### 9.1 Skill 文件一览

本项目的 UT 生成和协调依赖 7 个 skill 文件，形成层次结构：

```
ut-gen (root)               ← 通用模式、mockcpp技巧、分析方法论、构建命令
  ├── ut-gen-umq            ← UMQ模块特定：umq_* mock、Share-JFR、UmqOperation
  ├── ut-gen-core           ← core/socket特定：epoll mock、OsAPiMgr、socket ops
  ├── ut-gen-common         ← common特定：singleton cleanup、pthread mock
  ├── ut-gen-under-api      ← under_api特定：dlopen mock、两后端模式
  ├── ut-gen-profiling      ← profiling特定：无mock纯逻辑 (84.2%已达标)
  └── ut-coverage-coord     ← 协调层：认领、进度、里程碑、基线数据
```

### 9.2 各 Skill 与本文档的关系

| Skill | 引用本文档的章节 | 作用 |
|-------|----------------|------|
| ut-gen | 一(基线)、二(完备性) | 写 UT 时参考覆盖率目标和文件列表 |
| ut-gen-umq | 四.4.1(零覆盖率umq文件) | 写 UMQ UT 时参考优先级和 mock 需求 |
| ut-gen-core | 四.4.1(零覆盖率core文件) | 写 core UT 时参考优先级和 mock 需求 |
| ut-gen-common | 四.4.1(零覆盖率common文件) | 写 common UT 时参考优先级 |
| ut-gen-under-api | 四.4.1(零覆盖率under_api文件) | 写 dlopen UT 时参考优先级 |
| ut-gen-profiling | 一(基线已达标) | profiling 无需额外 UT |
| ut-coverage-coord | 全文(权威数据源) | 协调跟踪层，认领表+里程碑 |

### 9.3 如何使用 Skill

#### 写 UT 时 (opencode 交互)

1. 告诉 opencode 要为哪个文件写 UT，例如："为 umq_socket_connector.cpp 写 UT"
2. opencode 自动加载 `ut-gen` + `ut-gen-umq` (根据文件所在模块)
3. Skill 中的覆盖率数据、mock 策略、已知陷阱会作为上下文注入
4. 写完 UT 后更新 ut-coverage-coord 的进度表

#### 协调进度时

1. 加载 `ut-coverage-coord` skill
2. 查看认领表、里程碑、构建命令
3. 更新认领状态或覆盖率数字

#### 发现新陷阱时 (知识回流)

1. 模块特定陷阱 → 更新对应子 skill (e.g. ut-gen-umq "Common Pitfalls")
2. 跨模块模式 → 更新 ut-gen root skill
3. 构建系统问题 → 更新 AGENTS.md "Known Gotchas"
4. 覆盖率数据变更 → 更新本文档对应章节 + ut-coverage-coord 的基线表

### 9.4 维护规则

- **本文档 (UBSOCKET-COVERAGE-ANALYSIS.ch.md)** 是覆盖率**权威数据源**，skill 文件引用但不重复详细数据
- **ut-coverage-coord** 是**动态跟踪层**，负责认领和进度，基线数字从本文档引用
- **子 skill** 引用本文档的章节号，数据变更时只需更新本文档一处，skill 自动通过引用保持一致
- **AGENTS.md** 维护项目级上下文（构建命令、覆盖率基线摘要），详细数据指向本文档