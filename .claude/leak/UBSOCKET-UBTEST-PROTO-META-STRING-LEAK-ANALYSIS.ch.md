# brpc RpcMeta 解析字符串泄露分析( RpcMeta 子消息泄露的伴生 )

> **现象**:ASan 报告
> ```
> Direct leak of 224 byte(s) in 7 object(s) allocated from:
>     #1 ArenaStringPtr::NewString arenastring.h:398
>     #2 ArenaStringPtr::MutableNoCopy arenastring.cc:194
>     #3 ReadStringNoArena generated_message_tctable_lite.cc:1664
>     #4 TcParser::SingularString generated_message_tctable_lite.cc:1720
>     #5 TcParser::FastBS1 generated_message_tctable_lite.cc:1741
>     ...
>     #10 ParsePbFromZeroCopyStreamInlined protocol.cpp:216
>     #11 ParsePbFromIOBuf protocol.cpp:238
>     #12 ProcessRpcResponse baidu_rpc_protocol.cpp:999
> ```
>
> 本文确认其与 `UBSOCKET-UBTEST-PROTO-META-LEAK-ANALYSIS.ch.md`(RpcMeta 子消息 280B/7)是**同一调用点、同一批 7 个响应、同一逃逸机制**的伴生泄露,仅字段类型(message→string)不同。

## 1. 与 RpcMeta 子消息泄露的强相关

| 维度 | RpcMeta 子消息泄露 | 本泄露(RpcMeta 字符串) |
|------|-------------------|----------------------|
| 调用点 frame #12 | `ProcessRpcResponse:999` `ParsePbFromIOBuf(&meta, msg->meta)` | **同** |
| 对象 | protobuf 子 message(`MessageCreator::New`,FastMtS1) | `std::string`(`ArenaStringPtr::NewString`,FastBS1) |
| 大小 | 40B/个 × 7 = 280B | 32B/个 × 7 = 224B |
| 数量 | 7 | **7(完全一致)** |
| 父对象 | 栈 `RpcMeta meta` | **同** |
| 归属 | brpc/protobuf 协议解析层 | **同** |

**两者都是 `ProcessRpcResponse:999` 解析 `RpcMeta meta` 时由 protobuf `TcParser` 创建的堆对象**:`FastMtS1` 创建子 message(如 `RpcResponseMeta`),`FastBS1` 创建 string 字段(如 `error_text`/`method_name`)。**同 7 个响应**——这 7 个的 `RpcMeta` 整组堆分配(子消息 + 字符串)都泄露,强烈指向**这 7 个响应的 `meta` 整体所有权逃逸**,而非单字段缺陷。

## 2. 泄露对象

- **对象**:`std::__cxx11::basic_string`(libstdc++ SSO 实现),由 `ArenaStringPtr::NewString(Arena*)` 在 `ReadStringNoArena` 路径堆分配。
- **大小**:32 字节/个(libstdc++ `basic_string` SSO 在 aarch64 64 位下 = 32B),7 × 32 = 224B,与报告吻合。
- **归属字段**:`RpcMeta` 或其子消息(`RpcResponseMeta.error_text` / `RpcRequestMeta.method_name` 等)的 string 字段。ub_test 成功响应 `error_text` 默认空(不分配),故这 7 个泄露的 string 多半来自**带 error_text 的错误响应**,或 `meta.user_fields()` 的 string(但 ub_test 不用 user_fields)。

## 3. 调用栈解读

```
bthread task_runner
  → Socket::ProcessEvent (socket.cpp:1225)
    → InputMessenger::OnNewMessages → InputMessageClosure dtor (input_messenger.cpp:228)
      → ProcessInputMessage (input_messenger.cpp:184)
        → ProcessRpcResponse (baidu_rpc_protocol.cpp:995)
          → ParsePbFromIOBuf(&meta, msg->meta) (baidu_rpc_protocol.cpp:999)   ← 同 RpcMeta 泄露
            → ParseFromCodedStream → MergeFromImpl → TcParser::ParseLoop
              → FastBS1 → SingularString → ReadStringNoArena → NewString   ← string 堆分配
```

`RpcMeta meta` 是 `ProcessRpcResponse` 的**栈对象**(`baidu_rpc_protocol.cpp:998`)。`ParsePbFromIOBuf` 把 `msg->meta` 解析进 `meta`,期间 `TcParser::FastBS1`(Fast String 1-byte tag)为 string 字段调 `ArenaStringPtr::NewString(nullptr Arena)` 堆分配 `std::string`,所有权归 `meta`。`meta` 离开作用域应 `~RpcMeta` 析构释放所有 string——**7 个未释放说明这些 string 脱离了 `meta` 所有权**。

## 4. 疑点:栈对象的 string 为何泄露(同 RpcMeta 伴生)

与 `UBSOCKET-UBTEST-PROTO-META-LEAK-ANALYSIS.ch.md` §4 完全相同的悖论:`RpcMeta meta` 栈对象,所有退出路径(`:1001`/`:1020`/`:1106`)都让 `meta` 离开作用域析构 → 应释放其 string 与子消息。**7 个 string 与 7 个子消息同时泄露**说明这 7 个响应的 `meta` 整组堆分配都逃逸,机制同前:

- **机制 A**:Controller 拷贝 `meta.stream_settings()`(`:1026`)成堆 `StreamSettings`,若 Controller 析构不释放,其内部 string 也跟着泄露(ub_test 无 streaming,此路径不应触发,除非 7 个是带 stream 的特殊响应)
- **机制 B**:protobuf `TcParser` / `ArenaStringPtr` 在特定字段组合下产生脱离父消息的孤儿 string(protobuf 库内部)
- **机制 D**:与 RespClosure/done 泄露的衍生交互(在途 RPC 的 Controller 未删 → 拷贝自 meta 的 string 残留)

**子消息(280B/7)与 string(224B/7)同时泄露、数量完全一致**,最可能指向机制 B(protobuf 解析器对这 7 个特定响应的 meta 整体产生孤儿堆对象),而非单字段缺陷。

## 5. 当前可下结论

| 维度 | 结论 |
|------|------|
| 归属 | brpc/protobuf 协议解析层(第三方) |
| 是否 ubs-comm | ❌ 否 |
| 是否 ub_test 应用层 | ❌ 否 |
| 对象 | `RpcMeta` 解析创建的 string 字段(`std::string` SSO 32B) |
| 规模 | 224B(7×32B),极小 |
| 与 RpcMeta 子消息泄露关系 | **伴生**(同调用点、同 7 个、同机制) |
| 根因 | `meta` 整组堆分配逃逸,具体点需 brpc/protobuf 侧定位 |
| 优先级 | 低(224B 不影响功能与容量) |
| ubs-comm 修复 | ❌ 无 |

## 6. 验证与定位步骤

与 `UBSOCKET-UBTEST-PROTO-META-LEAK-ANALYSIS.ch.md` §6 共用:

1. **先修 RespClosure + done 在途泄露**(`UBSOCKET-UBTEST-CLIENT-LEAK-ANALYSIS.ch.md` 方案1 + `UBSOCKET-UBTEST-DONE-CALLBACK-LEAK-ANALYSIS.ch.md` drain)。修复后重跑 ASan,若本 224B 与 RpcMeta 280B **同时消失** → 机制 D(衍生交互)坐实。
2. 若仍存在,在 `baidu_rpc_protocol.cpp:1026`/`:1031` 加计数(对 `meta.stream_settings()`/`meta.user_fields()` 拷贝次数 vs Controller 析构释放次数),判定机制 A。
3. 若机制 A/B 排除,则属 protobuf `TcParser`/`ArenaStringPtr` 内部,换 protobuf 版本复测或上报 protobuf 社区。

**关键观察点**:子消息(280B/7)与 string(224B/7)数量严格相等,修复时应**两者同时观察**——若只消一个,说明是单字段缺陷;若同时消,说明是 meta 级逃逸(机制 B/D)。

## 7. 与其他泄露的关系

| 维度 | 本泄露(meta string) | RpcMeta 子消息 | done 在途 | RespClosure/RunTest | RX/TX/Acceptor |
|------|---------------------|---------------|-----------|---------------------|----------------|
| 归属 | brpc/protobuf | brpc/protobuf | ub_test | ub_test | ubs-comm |
| 与 RpcMeta 子消息关系 | **伴生同根** | 自身 | 独立 | 独立 | 独立 |
| ubs-comm 修复 | ❌ | ❌ | ❌(drain) | ❌(delete closure) | ✓ |

本泄露与 RpcMeta 子消息泄露是**伴生对**,应作为一组观察/定位。ubs-comm 侧无修复点。

## 参考

- `UBSOCKET-UBTEST-PROTO-META-LEAK-ANALYSIS.ch.md` — RpcMeta 子消息泄露(伴生,同 7 个)
- `UBSOCKET-UBTEST-DONE-CALLBACK-LEAK-ANALYSIS.ch.md` — done 在途泄露(可能衍生交互,机制 D)
- `UBSOCKET-UBTEST-CLIENT-LEAK-ANALYSIS.ch.md` — RespClosure 泄露
- `UBSOCKET-UMQ-RX-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-TX-EVENT-LEAK-ANALYSIS.ch.md` / `UBSOCKET-UMQ-ACCEPTOR-LEAK-ANALYSIS.ch.md` — ubs-comm 侧泄露
- brpc 源码:`src/brpc/policy/baidu_rpc_protocol.cpp:995-1107`、`src/brpc/protocol.cpp:205-238`
- protobuf:`arenastring.h:398`、`arenastring.cc:194`、`generated_message_tctable_lite.cc:1664,1720,1741`
