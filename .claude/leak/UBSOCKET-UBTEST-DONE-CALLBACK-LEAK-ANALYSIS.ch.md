# ub_test Client done 回调对象泄露分析

> **现象**:ASan 报告
> ```
> Direct leak of 224 byte(s) in 7 object(s) allocated from:
>     #1 brpc::NewCallback<RespClosure*>(...) ub_test_client
>     #2 PerformanceTest::SendRequest() ub_test_client
>     #3 PerformanceTest::HandleResponse(RespClosure*) ub_test_client
>     #4 brpc::internal::FunctionClosure1<RespClosure*>::Run()
>     #5 brpc::Controller::EndRPC controller.cpp:981
>     ...
>     #8 brpc::policy::ProcessRpcResponse baidu_rpc_protocol.cpp:1106
> ```
>
> 本文确认其与 RespClosure 泄露**机制不同**(不是同一根因),并定位 `done` 回调的自删语义缺口。

## 1. 泄露对象

- **对象**:brpc `NewCallback` 返回的 `google::protobuf::Closure*`,具体类型 `brpc::internal::FunctionClosure1<PerformanceTest::RespClosure*>`。
- **分配点**:`client.cpp:333` `google::protobuf::Closure* done = brpc::NewCallback(&HandleResponse, closure);`
- **规模**:7 个 × 32 字节 = **224 字节**(224/7=32)。32B = vtable 指针(8) + 函数指针(8) + 参数 `RespClosure*`(8) + 对齐填充 → 32,吻合。

## 2. 关键语义:`done` 自删(self-deleting)

protobuf/brpc 的 `NewCallback` 返回的闭包是**自删对象**:其 `Run()` 在末尾 `delete this`(protobuf `NewCallback` 标准语义)。因此:

- **RPC 正常完成**(成功/失败/超时):brpc 调 `done->Run()` → `HandleResponse(closure)` 执行 → `Run()` 末尾自删 → `done` 释放。**无泄露**。
- **RPC 在途未完成**(`done->Run()` 从未被调用):`done` 永不自删 → **泄露**。

7 个 `done` 泄露 = 7 个 RPC 在测试结束时仍在途、`done->Run()` 从未被触发。

## 3. 与 RespClosure 泄露的本质区别(非同根)

| 维度 | RespClosure 泄露(70+10) | 本泄露(done 7) |
|------|------------------------|------------------|
| 对象 | `RespClosure` 结构体(24B) | `FunctionClosure1` 闭包(32B) |
| `HandleResponse` 是否执行 | ✓ 执行了 | ❌ **未执行**(`done->Run` 没调) |
| 泄露机理 | HandleResponse 跑了但**不 `delete closure`** | HandleResponse 压根没跑,`done` 没机会自删 |
| RespClosure 修复能否消除 | ✓ 能(方案1 `delete closure`) | ❌ **不能**(HandleResponse 没执行,加的 delete 不生效) |
| 场景 | 已完成的 RPC | 在途未完成的 RPC |

**关键**:RespClosure 修复(`HandleResponse` 末尾 `delete closure`)**无法**消除本泄露——因为这 7 个 RPC 的 `HandleResponse` 从未被调用,修复代码不会执行。本泄露是**独立的、不同的根因**,需单独处理。

## 4. 调用栈解读

```
bthread task_runner
  → Socket::ProcessEvent (socket.cpp:1225)
    → InputMessenger::OnNewMessages → InputMessageClosure dtor (input_messenger.cpp:228)
      → ProcessInputMessage (input_messenger.cpp:184)
        → ProcessRpcResponse (baidu_rpc_protocol.cpp:1106)
          → OnVersionedRPCReturned → EndRPC (controller.cpp:981)
            → FunctionClosure1::Run → HandleResponse → SendRequest → NewCallback  ← 泄露分配
```

本泄露的分配栈 frame #3 是 `HandleResponse`(`:4466f8`),说明这 7 个 `done` 是**重发路径**分配的(`HandleResponse` 末尾 `SendRequest()`(`client.cpp:391`)维持窗口时新建的 `done`)。即:这 7 个是某轮重发后留在途、测试结束前未收到响应的 RPC。

## 5. 为何 7 个在途未完成

测试流程:
- `RunTest` 发 `FLAGS_queue_depth` 初始在途(`client.cpp:399-401`)
- 每个 RPC 完成后 `HandleResponse` 再 `SendRequest` 维持窗口(`client.cpp:391`)
- 超时/迭代到 → `_stop=true`,`HandleResponse` 不再重发(`:372-375` / `:387-390` 早退)

测试停止瞬间,**最后一批在途 RPC**(已 `SendRequest` 但响应未到达)的 `done` 无人调用:
- brpc 的 `Controller`/`Channel` 析构理应取消在途并调 `done->Run()`(带 ECLOSE)
- 但若进程退出/bthread 未及调度取消、或 `Channel` 析构链未覆盖这些在途 RPC,`done->Run()` 跳过 → 泄露

7 个 = 在途未完成数(跨 630 个 PerformanceTest 的小残余,具体数取决于退出时刻的调度快照)。

## 6. 衍生:这 7 个在途 RPC 的 closure/cntl/resp 也泄露

这 7 个在途 RPC 的完整对象组都泄露(因 `HandleResponse` 未执行,无任何清理):

| 对象 | 大小 | 归属 ASan 报告 |
|------|------|---------------|
| `done`(`FunctionClosure1`) | 32B | **本报告 224B/7** |
| `closure`(`RespClosure`) | 24B | 计入 RespClosure(70)报告的子集(同分配栈) |
| `closure->cntl`(`brpc::Controller`) | 较大 | 应有独立 ASan 报告(未粘贴) |
| `closure->resp`(`PerfTestResponse`) | 中 | 应有独立报告(未粘贴) |

即 RespClosure 报告的 80 个(70+10)中,7 个是这种"在途未完成"的(其 `done` 也泄露,即本报告),其余 73 个是"已完成但 closure 未删"的(`done` 已自删)。

## 7. 修复方案

本泄露的根因是**在途 RPC 未被取消/未触达 `done->Run()`**,与 RespClosure 的 `delete closure` 修复无关。两种修法:

### 方案 1【推荐】测试退出前 drain 在途 RPC

在 `Test`(`client.cpp:660+`)结束前、`delete t` 之前,等待每个 PerformanceTest 的在途 RPC 归零:

```cpp
// PerformanceTest 增加在途计数(原子),SendRequest ++,HandleResponse --
// Test 末尾:
for (auto t : success_tests) {
    while (t->InFlightCount() > 0) {
        bthread_usleep(1000);   // 等待在途 RPC 自然完成(响应到达→done->Run→自删)
    }
}
// 之后再 delete t(此时 Channel 析构已无在途,clean)
```

要点:让在途 RPC **自然收到响应**(server 正常回包)→ `done->Run()` 被调 → 自删 + HandleResponse 跑(配合 RespClosure 修复 `delete closure`)。需要 server 仍在运行且能回完最后一批包。

### 方案 2 依赖 brpc Channel 析构取消

确保 `~PerformanceTest`(`delete _channel`)→ brpc `Channel`/`Socket` 析构**同步**取消所有在途 RPC 并调 `done->Run(ECLOSE)`。若 brpc 当前实现是异步/不完整,需 brpc 侧补全。这是 brpc 行为依赖,方案 1 更可控。

### 方案 3【结合 RespClosure 修复】

即便 drain,仍需 RespClosure 修复(`HandleResponse` 末尾 `delete closure`)才能消除 closure 本体泄露。两者配合:drain 让 `done->Run()` 触发 → HandleResponse 执行 → RespClosure 修复的 `delete closure` 生效 → `done` 自删 + `closure` 删除,**两类泄露同时消除**。

## 8. 触发条件

- 测试**非自然结束**(超时 `_stop` 或迭代到顶)时仍有在途 RPC
- 退出流程未等待在途 RPC 完成或被 brpc 显式取消
- 与 UB 配置无关,纯 TCP 模式也会出现(只要测试用异步 RPC + 提前结束)

## 9. 关键代码位置索引

| 位置 | 作用 | 泄露相关性 |
|------|------|-----------|
| `client.cpp:333` | `brpc::NewCallback(&HandleResponse, closure)` | **分配点**(32B `FunctionClosure1`) |
| `client.cpp:391` | `HandleResponse` 末尾 `SendRequest()` 重发 | 本 7 个的来源(重发后留途) |
| `client.cpp:372-390` | `_stop` 早退,不重发 | 在途 RPC 停止推进 |
| brpc `controller.cpp:981` `EndRPC`→`done->Run` | 正常完成触发自删 | 在途未达此路径→泄露 |
| brpc `~Channel`/`~Socket` | 理应取消在途 | 若未覆盖→泄露(方案2依赖) |

## 10. 与其他泄露的关系

| 维度 | 本泄露(done 在途) | RespClosure | RunTest 突发 | RpcMeta | RX/TX/Acceptor |
|------|-------------------|-------------|-------------|---------|----------------|
| 归属 | ub_test 应用层 | ub_test 应用层 | ub_test 应用层 | brpc/protobuf | ubs-comm |
| 根因 | 在途未 `done->Run` | HandleResponse 不 delete closure | 同 RespClosure | 解析子消息逃逸 | 各自独立 |
| 与 RespClosure 修复关系 | **独立**(不能消) | 自身 | 同根消 | 独立 | 独立 |
| 优先级 | 中(需 drain 配合) | 中 | 中(随 RespClosure 消) | 低 | 高/低 |

**本泄露是 6 类中唯一需要"测试退出流程改造"(drain 在途)的**,不能仅靠单点 `delete` 修复。建议与 RespClosure 修复配合实施:RespClosure 修复消"已完成 closure"泄露,drain 修复消"在途 done"泄露。

## 参考

- `UBSOCKET-UBTEST-CLIENT-LEAK-ANALYSIS.ch.md` — RespClosure 泄露(已完成路径,方案1 `delete closure`)
- `UBSOCKET-UBTEST-RUNTEST-CLOSURE-LEAK-ANALYSIS.ch.md` — RunTest 初始突发(RespClosure 同根变体)
- `UBSOCKET-UBTEST-PROTO-META-LEAK-ANALYSIS.ch.md` — brpc/protobuf 层泄露
- `UBSOCKET-UMQ-RX-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-TX-EVENT-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md` — ubs-comm 侧泄露
- 源码:`brpc/example/ub_test/client.cpp:333,372-391`
