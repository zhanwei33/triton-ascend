# ub_test Client RunTest 初始突发 RespClosure 泄露分析

> **现象**:ASan 报告
> ```
> Direct leak of 240 byte(s) in 10 object(s) allocated from:
>     #1 PerformanceTest::SendRequest() ub_test_client
>     #2 PerformanceTest::RunTest(void*) ub_test_client
>     #3 bthread::TaskGroup::task_runner
> ```
>
> 本文确认其与 `UBSOCKET-UBTEST-CLIENT-LEAK-ANALYSIS.ch.md`(RespClosure 1680B/70)同根,并解释 ASan 为何分两条报告。

## 1. 与 RespClosure 泄露同根

**本泄露与 `UBSOCKET-UBTEST-CLIENT-LEAK-ANALYSIS.ch.md` 是同一根因、同一对象、同一修复点。** 二者均为 `PerformanceTest::RespClosure`(24B/个,3 指针)在 `SendRequest`(`client.cpp:321` `new RespClosure`)分配,而 `HandleResponse`(`client.cpp:346-392`)只用 `unique_ptr` 管 `cntl`/`resp`,**漏 `delete closure` 本体**,所有退出路径都泄露。

| 维度 | RespClosure 泄露 | 本泄露(RunTest 初始突发) |
|------|-----------------|--------------------------|
| 报告 | 1680B / 70 个 | 240B / 10 个 |
| 对象 | `RespClosure`(24B) | `RespClosure`(24B) |
| 分配点 | `SendRequest` `client.cpp:321` | 同一分配点 |
| 调用栈 frame #2 | `HandleResponse`(`client.cpp:446a40`)← 重发路径 | `RunTest`(`client.cpp:446a40`)← 初始突发 |
| 根因 | `HandleResponse` 不 `delete closure` | 同 |
| 修复 | `HandleResponse` 顶部 `unique_ptr<RespClosure>` 或末尾 `delete closure` | 同 |

## 2. ASan 为何分两条报告

ASan 的 leak detector 按**分配时的完整调用栈**分组。`new RespClosure` 虽在同一行(`client.cpp:321`),但调用者不同:

- **本报告(240B/10)**:`SendRequest` ← `RunTest` ← `bthread task_runner`。即 `RunTest`(`client.cpp:399-401`)循环 `FLAGS_queue_depth` 次发初始在途请求,这批 closure 的分配栈含 `RunTest` 帧。
- **RespClosure 报告(1680B/70)**:`SendRequest` ← `HandleResponse` ← `FunctionClosure1::Run` ← `Controller::EndRPC`。即 `HandleResponse` 末尾 `closure->test->SendRequest()`(`client.cpp:391`)重发维持窗口,这批 closure 的分配栈含 `HandleResponse` 帧。

两组分配栈不同 → ASan 分两份报告。**对象类型、大小、根因完全一致**,修复一次即同时消除两条。

## 3. 10 个 = queue_depth 初始突发

`RunTest`(`client.cpp:394-404`):

```cpp
static void* RunTest(void* arg) {
    PerformanceTest* test = (PerformanceTest*)arg;
    test->_start_time = butil::gettimeofday_us();
    test->_iterations = FLAGS_test_iterations;

    for (int i = 0; i < FLAGS_queue_depth; ++i) {   // ← FLAGS_queue_depth 个初始在途
        test->SendRequest();
    }
    return NULL;
}
```

`FLAGS_queue_depth` 默认 1(`client.cpp:50`),本次测试设为 **10** → 初始发 10 个请求 → 10 个 `RespClosure`。这 10 个在测试结束(`_stop`/超时)时其 `HandleResponse` 走早退路径(`:372-375` / `:387-390`)不 `delete closure` → 全部泄露,240B/10 吻合。

## 4. 触发条件

与 RespClosure 泄露完全一致:**任一 RPC 完成都会泄露 1 个 `RespClosure`**(无论来自初始突发还是重发)。本报告仅是初始突发的那个子集,长期压测会持续累积,每 RPC 24B。

## 5. 修复方案

**与 `UBSOCKET-UBTEST-CLIENT-LEAK-ANALYSIS.ch.md` 方案 1 完全相同,改动一次消除两条 ASan 报告:**

`HandleResponse`(`client.cpp:346-392`)顶部捕获 `test`、末尾 `delete closure`:

```cpp
static void HandleResponse(RespClosure* closure) {
    PerformanceTest* test = closure->test;                          // 捕获,后续 SendRequest 用
    std::unique_ptr<brpc::Controller> cntl_guard(closure->cntl);
    std::unique_ptr<test::PerfTestResponse> response_guard(closure->resp);
    ... // 原统计逻辑不变
    cntl_guard.reset(NULL);
    response_guard.reset(NULL);

    bool stop = false;
    if (test->_iterations == 0 && FLAGS_test_iterations > 0) { test->_stop = true; stop = true; }
    else {
        --test->_iterations;
        uint64_t now = butil::gettimeofday_us();
        ... // CPU/内存统计
        if (now - test->_start_time > FLAGS_test_seconds * 1000000u) { test->_stop = true; stop = true; }
    }
    delete closure;          // ← 新增:释放 RespClosure 本体(消除本报告 + RespClosure 报告)
    if (!stop) test->SendRequest();
}
```

要点:`delete closure` 在不再访问 `closure->*` 之后、`SendRequest` 之前;`test` 是非拥有指针(由外层 `tests[]` 数组管理,`Test` 末尾 `delete t`),捕获为裸指针安全。

## 6. 验证

修复后 ASan 重跑:本 240B/10 与 RespClosure 1680B/70 **两条报告应同时消失**。可用 `FLAGS_queue_depth=10` + `FLAGS_test_iterations=小值` 短测验证。

## 7. 关键代码位置索引

| 位置 | 作用 | 泄露相关性 |
|------|------|-----------|
| `client.cpp:321` | `new RespClosure`(24B) | **分配点**(与 RespClosure 共用) |
| `client.cpp:399-401` | `RunTest` 循环发 `queue_depth` 个初始请求 | 本报告 10 个的来源 |
| `client.cpp:346-392` | `HandleResponse` 不 `delete closure` | **根因**(与 RespClosure 共用) |
| `client.cpp:372-375` | 迭代退出 return 不 delete | 泄露出口 |
| `client.cpp:387-390` | 超时退出 return 不 delete | 泄露出口 |
| `client.cpp:391` | `SendRequest()` 重发(旧 closure 不 delete) | RespClosure 报告的来源 |

## 8. 与其他泄露的关系

本泄露是 `RespClosure` 泄露的**分配栈变体**,非独立缺陷。ubs-comm 侧的 `RX`/`TX Event`/`Acceptor` 三类与本类**机理独立、修复点不重叠**。本类属 ub_test 应用层,应由 `client.cpp` 修复(与 `RespClosure` 共用方案 1)。

## 参考

- `UBSOCKET-UBTEST-CLIENT-LEAK-ANALYSIS.ch.md` — RespClosure 泄露(同根,方案 1 共用)
- `UBSOCKET-UMQ-RX-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-TX-EVENT-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md` — ubs-comm 侧泄露(机理独立)
- `UBSOCKET-UBTEST-PROTO-META-LEAK-ANALYSIS.ch.md` — brpc/protobuf 层泄露(机理独立)
- 源码:`brpc/example/ub_test/client.cpp:321,346-404`
