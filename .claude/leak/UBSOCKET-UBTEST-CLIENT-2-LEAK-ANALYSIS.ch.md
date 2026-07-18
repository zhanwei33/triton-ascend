# ub_test Client RespClosure 泄露分析(同根变体,7 对象)

> **现象**:ASan 报告
> ```
> Indirect leak of 168 byte(s) in 7 object(s) allocated from:
>     #1 PerformanceTest::SendRequest() 0x445614
>     #2 PerformanceTest::HandleResponse(RespClosure*) 0x4466f8
>     #3 brpc::internal::FunctionClosure1<RespClosure*>::Run()
>     #4 brpc::Controller::EndRPC controller.cpp:981
>     ...
>     #7 brpc::policy::ProcessRpcResponse baidu_rpc_protocol.cpp:1106
> ```
>
> 本文确认其与 `UBSOCKET-UBTEST-CLIENT-LEAK-ANALYSIS.ch.md`(1680B/70)是**同分配点、同对象、同根因**的 RespClosure 泄露,仅对象计数不同(7 vs 70,疑不同测试轮次/参数)。

## 1. 与既有 RespClosure 泄露的同根性

| 维度 | 既有报告(1680B/70) | 本报告(168B/7) |
|------|---------------------|-----------------|
| 分配点 frame #1 | `SendRequest 0x445614` | **`SendRequest 0x445614`(地址完全一致)** |
| frame #2 | `HandleResponse 0x4466f8` | **`HandleResponse 0x4466f8`(一致)** |
| 对象 | `RespClosure`(3 指针=24B) | **`RespClosure`(24B)** |
| 大小/个 | 1680B / 70 | 168B / 7(=24B × 7) |
| 根因 | `HandleResponse` 不 `delete closure` | **同** |
| 修复 | `HandleResponse` 末尾 `delete closure`(方案1) | **同** |

ASan 按分配栈分组。两报告分配栈地址完全一致(`0x445614`/`0x4466f8`),理论上同组。本报告独立出现(7 而非并入 70),最可能是**不同 ASan 运行轮次**(测试参数/时长不同导致完成 RPC 数不同:70 vs 7)。机理、对象、修复完全相同。

## 2. 泄露对象

- **对象**:`PerformanceTest::RespClosure`(`client.cpp:339-343`):
  ```cpp
  struct RespClosure {
      brpc::Controller* cntl;        // 8B
      test::PerfTestResponse* resp;  // 8B
      PerformanceTest* test;         // 8B
  };
  ```
  = 3 指针 = 24 字节(64 位)。168 / 7 = 24,吻合。
- **分配点**:`SendRequest`(`client.cpp:321`)`RespClosure* closure = new RespClosure;`(frame #1 `0x445614`)。
- **调用路径**:frame #2 `HandleResponse 0x4466f8` 即 `HandleResponse` 末尾 `closure->test->SendRequest()`(`client.cpp:391` 重发维持窗口)——重发路径分配的 closure。

## 3. 根因(同既有报告)

`HandleResponse`(`client.cpp:346-392`)用 `unique_ptr` 管 `cntl`/`resp` 成员,但**漏 `closure` 本体**:

```cpp
static void HandleResponse(RespClosure* closure) {
    std::unique_ptr<brpc::Controller> cntl_guard(closure->cntl);          // 管 cntl ✓
    std::unique_ptr<test::PerfTestResponse> response_guard(closure->resp);// 管 resp ✓
    ...
    cntl_guard.reset(NULL);       // ✓ 释放 cntl
    response_guard.reset(NULL);   // ✓ 释放 resp
    // ← 缺:无 unique_ptr/delete 管 closure 本体

    if (...) { return; }          // 退出1:closure 泄露
    if (...) { return; }          // 退出2:closure 泄露
    closure->test->SendRequest(); // 退出3:旧 closure 泄露,新建一个
}
```

三条退出路径全部不 `delete closure`。每个完成的 RPC(无论来自 RunTest 初始突发还是 HandleResponse 重发)都泄露 1 个 `RespClosure`(24B)。本报告 7 个 = 本次运行完成的 7 个 RPC。

## 4. 与 RunTest 突发(240B/10)、done 在途(224B/7)的关系

| 报告 | 对象 | 大小/个 | 调用栈 frame #2 | 个数 | 文档 |
|------|------|---------|----------------|------|------|
| 1680B/70 | RespClosure | 24B | HandleResponse | 70 | CLIENT(同根主) |
| 240B/10 | RespClosure | 24B | RunTest | 10 | RUNTEST(同根变体一) |
| **168B/7** | **RespClosure** | **24B** | **HandleResponse** | **7** | **本报告(同根变体二)** |
| 224B/7 | done(FunctionClosure1) | 32B | HandleResponse | 7 | DONE(不同对象,在途未 Run) |

本报告与 1680B/70 是**同分配栈同对象**(HandleResponse 重发路径 RespClosure),只是不同运行计数;与 240B/10 是**同对象不同调用栈**(RunTest 初始突发 vs HandleResponse 重发);与 224B/7 是**同调用栈不同对象**(RespClosure 24B vs done 32B,前者 HandleResponse 跑了不删 closure,后者 HandleResponse 没跑 done 不自删)。

## 5. 触发条件

无条件——任一 RPC 完成都泄露 1 个 `RespClosure`(HandleResponse 跑了但没删 closure)。与 UB 配置无关。

## 6. 修复方案

**与 `UBSOCKET-UBTEST-CLIENT-LEAK-ANALYSIS.ch.md` §5 方案1 完全相同**——`HandleResponse` 顶部捕获 `test`、末尾 `delete closure`:

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
    delete closure;          // ← 新增:释放 RespClosure 本体(消本 168B/7 + 1680B/70 + 240B/10 三条同根报告)
    if (!stop) test->SendRequest();
}
```

**一次修复消除三条 RespClosure 同根报告**(本 168B/7 + 1680B/70 + 240B/10)。注意 `done` 在途(224B/7)是不同对象不同根因(done->Run 未调),需配合 drain 在途 RPC 修复(见 `UBSOCKET-UBTEST-DONE-CALLBACK-LEAK-ANALYSIS.ch.md`)。

## 7. 验证

修复后 ASan 重跑:本 168B/7 + 1680B/70 + 240B/10 **三条 RespClosure 报告同时消失**。可用 `FLAGS_test_iterations` 设小值跑短测验证,对象计数应归零。

## 8. 关键代码位置索引

| 位置 | 作用 | 泄露相关性 |
|------|------|-----------|
| `client.cpp:321` | `new RespClosure`(24B) | **分配点**(frame #1 `0x445614`,同 1680B/70) |
| `client.cpp:333` | `NewCallback(&HandleResponse, closure)` 传指针 | 所有权移交 HandleResponse |
| `client.cpp:339-343` | `RespClosure` 结构(3 指针=24B) | 泄露对象 |
| `client.cpp:346-392` | `HandleResponse` 不 `delete closure` | **根因**(同 1680B/70) |
| `client.cpp:372-375` | 迭代退出 return 不 delete | 泄露出口 |
| `client.cpp:387-390` | 超时退出 return 不 delete | 泄露出口 |
| `client.cpp:391` | `SendRequest()` 重发(旧 closure 不 delete) | 本 7 个的来源(重发路径) |
| brpc `controller.cpp:981` `EndRPC`→`done->Run` | 触发 HandleResponse | brpc 侧已正确释放 `done` 本身 |

## 9. 与其他泄露的关系

| 维度 | 本泄露(RespClosure 168B/7) | RespClosure 1680B/70 / 240B/10 | done 224B/7 | bvar 家族 | UmqSocket 析构链 | AsyncEventPoll | RX/TX Event | RpcMeta |
|------|------------------------------|-------------------------------|-------------|-----------|----------------|---------------|------------|---------|
| 归属 | ub_test 应用层 | ub_test 应用层 | ub_test 应用层 | brpc | ubsocket core | ubsocket core | ubsocket umq | brpc/protobuf |
| 类别 | HandleResponse 不 delete closure(同根变体) | 同 | 在途 done->Run 未调 | bthread 抛弃 | 析构不 delete | 析构不清 map | 退出未释放/buffer | 解析逃逸 |
| 与 RespClosure 1680B 关系 | **同分配点同对象同根(变体二)** | 自身 | 不同对象 | 独立 | 独立 | 独立 | 独立 | 独立 |
| ubs-comm 修复 | ❌(client.cpp) | ❌ | ❌ | ❌ | ✓ | ✓ | ✓ | ❌ |

本泄露是 RespClosure 泄露的**同根变体二**(不同运行计数),与 1680B/70、240B/10 共用 `HandleResponse` 末尾 `delete closure` 修复,一次消除三条。属 ub_test 应用层,ubs-comm 无修复点。

## 参考

- `UBSOCKET-UBTEST-CLIENT-LEAK-ANALYSIS.ch.md` — RespClosure 1680B/70(同分配点同根,§5 方案1 共用)
- `UBSOCKET-UBTEST-RUNTEST-CLOSURE-LEAK-ANALYSIS.ch.md` — RespClosure 240B/10(同对象不同调用栈,同根)
- `UBSOCKET-UBTEST-DONE-CALLBACK-LEAK-ANALYSIS.ch.md` — done 224B/7(同调用栈不同对象,在途未 Run,需 drain)
- 其他 ubs-comm/brpc 泄露文档
- 源码:`brpc/example/ub_test/client.cpp:321,333,339-343,346-392`
