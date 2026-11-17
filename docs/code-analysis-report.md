# csilk 代码库完整分析报告（当前基线）

> 生成日期：2026-11-17 · §1–§10 基于早期 `88c1d7a7` 基线；§12 为 2026-11-17 全量复审（`master` @ `785aa5a7`，v1.1.0，Post-concurrency-fixes，CI 全绿）
> 方法：全库扫描（src/include/cmake/tests）+ codegraph 语义索引 + 多模块并行审计 + 关键风险逐行复核
> 定位：C23 轻量级 HTTP 框架，P99 ≤ 5ms @ 10K QPS；默认 libuv / 可选 io_uring 双后端。

---

## 1. 规模概览

| 维度 | 数值 |
|---|---|
| src C/H 总行数 | **68,952** |
| include 公共头 | **14,865** |
| tests 测试代码 | **51,651**（与源码 1:1 覆盖，单测极密） |
| 源码模块 | `src` 20 个子目录，~300 文件 |
| cmake 模块 | 11 个（options/targets/sources/tests/tooling/install/format/…） |
| fuzz 用例 | 11 个 harness（test/url/yaml/headers/h2/h3/websocket/raft_rpc/mcp/sql_rewrite/wasm） |

模块行数 Top：`core` 24.6k · `drivers` 13.7k · `middleware` 7.4k · `workflow` 7.1k · `core/primitives` 5.8k · `protocols` 5.3k · `messaging` 4.9k。

---

## 2. 核心请求生命周期（已逐行验证）

```
TCP accept (per-worker uv_server / io_uring ring)
  → on_read → csilk_client_t (per-worker lock-free pool)
  → TLS read → arena buffer (zero-copy views)
  → llhttp on_message_complete (http1_parse.c:496)
       → finalize_request(): 存 headers / split URL / URL-decode / parse query
       → _csilk_dispatch_request() (context.c:86)
            → 触发 REQUEST_BEGIN hooks
            → csilk_server_router_acquire (RCU 读侧临界区, EBR 无锁)
            → csilk_router_match_ctx (trie 迭代, 非递归)
            → csilk_next()  洋葱链执行中间件
            → 路由未命中 → not_found_handler / 404
            → release router
            → 同步: _csilk_send_response;  异步: csilk_client_read_stop
  → 响应构建 arena → 流式 watermark 背压 (64KB high / 16KB low)
  → 连接归还池 (keep-alive) 或 generation-tagged recycle (client_destroy)
```

**关键设计点（验证结论）**

- **`_csilk_dispatch_request`**：路由匹配在 RCU 读侧临界区内完成，匹配成功后 `csilk_next` 在 `router_release` 前执行——即**整个 handler 链都在 EBR reader 区**，保证热重载 swap 时读者不与 retired trie 并发。代价是热重载回收需等所有 reader 的 epoch grace period。
- **`csilk_next` 语义**：`aborted`/`panicked` 直接 no-op；推进 `handler_index`。设置终止响应（`csilk_string`/`csilk_json_error`）后通常**不再** `csilk_next`。`csilk_abort` 仅置标志，不关连接，响应在链返回后由 `_csilk_dispatch_request` 统一发出。
- **recovery**：`csilk_recovery_handler` 包整条链；`csilk_panic` → `csilk_ctx_defer_free`（LIFO 清理）→ 500。
- **`csilk_response_write`**：watermark 达 high 时置 `write_paused=1` 返回 0（生产者应暂停，drain 恢复）；超 `max_write_buffer_size` 丢写。

---

## 3. 并发与内存模型（验证结论）

- **Worker**：每核一线程，各自 libuv/io_uring 事件循环，`pin_thread_to_core` 绑核。`wp->active_clients` 严格单线程私有。跨 worker 一律 `csilk_dispatch`（lfq + async_send）。
- **Client 回收（generation tag 防 ABA/UAF）**：`client_destroy` 用 `state: CLOSING→CLOSED` CAS 保证幂等；owner 线程直接销毁，非 owner 线程 `_csilk_client_check_recycle` 入 lfq 派发给 owner，回调校验 `client->generation == gen && state == CLOSING && ref_count<=0 && pending_io<=0`。双 ref（`ref_count` 基础引用 + `pending_io` 在途 I/O）分离，SSE/WS/streaming 靠 `csilk_client_ref/unref` + `pending_io_inc/dec`。设计严谨。
- **RCU/EBR（server_rcu.c，已逐行验证）**：
  - 读侧 thread-local slot（`CSILK_RELOAD_MAX_READERS` 静态 + overflow 动态 calloc 链表 + 新 slot），fast path 复用 TLS slot；嵌套 acquire 用 `nesting_depth` 计数，仅 depth 0 时取 epoch。
  - 全局 `global_epoch` 单调；`server_gen` 表（1024 槽）做 server 身份 + thread-exit destructor 清理 slot；碰撞仅告警不阻断（旧 server 即将 free，安全）。
  - 所有权契约（**已被 CI 修复锁定**）：`csilk_server_free` 仅回收 retired 链表；**最终 active router 由 caller 负责** `csilk_router_free`。`test_rcu_lifecycle_stress` Test 6 此前误以为 server 拥有 active router 导致 ASAN leak，已修。
- **Arena（arena.c）**：bump allocator + 4/16/64KB 三级 TLS free-list，chunk 按 64B cache line 对齐防 false sharing，`max_total_bytes` 软上限，`reset ≤5ns`，`csilk_arena_flush_free_list` worker 退出冲刷。堆对象绑定 arena 生命周期用 `csilk_set_ex(c,key,ptr,dtor)`。
- **Body 池**：`_Thread_local csilk_body_pool_t` 5 档（64K/128K/256K/512K/1M）size-class 复用，TLS destructor 清理。

---

## 4. 双 IO 后端（验证结论）

- `CSILK_USE_URING` 决定：`csilk_io_loop_t` = 内嵌 `io_uring` ring + op 池（自研）/ 或全部 typedef 到 libuv。`uring_op_context_t` 把 opcode+owner+**64 位 generation** 塞进 `user_data`，CQE 分发 O(1) 免哈希 + 防 ABA。
- **已知语义差异（双栈陷阱）**：uring 后端 fs op 退化为**同步** `open/preadv/pwritev/sendfile`（非真异步）；`csilk_io_default_loop` uring 下是 per-thread TLS loop；`csilk_io_queue_work` uring 线程池满（`URING_TP_MAX_WORK=4096`）直接返回 -1 不阻塞，DB async 会丢任务。同一 API 面两后端行为不完全等价。

---

## 5. 驱动层（DB/AI/Vector/Cipher/Reflection/Workflow）

- **DB（db.c）**：仿 Go database/sql 可插拔注册表（16 driver + `registry_mutex`）+ 连接池。holder 跟踪（`csilk_thread_self`）修 csilk#1 UAF：跨线程 put 被拒。
  - SQLite：`PRAGMA journal_mode=WAL + busy_timeout=5000` 解决多 worker 写竞争；prepared 按 `column_type` 保留类型。
  - PG：`?`→`$N` 重写 + `PQprepare` 命名 plan，**绑定值全 text 化**（jsonb/数组精度受限）。
  - MySQL：`MYSQL_STMT` 原生 prepared，动态扩 buffer。
  - Redis：`redisCommandArgv` 免 `%` 注入，MULTI/EXEC；无 prepared（no-op 桩）。
  - Mongo：多文档事务不支持（-1）。
  - 风险：acquire 池耗尽直接 -1（无阻塞等待）；`csilk_db_pool_get_connection` 已 deprecate（裸指针无锁 UB）。
- **AI/Workflow**：OpenAI/Ollama/DeepSeek LLM 客户端 + tool_registry（JSON schema 生成/分发）+ ai_trace（端到端延迟/token/工具 span）+ ReAct 多步 agent loop + DAG scheduler + Skill L1/L2/L3（LOCAL bounded fork/execvpe，REMOTE via MQ）。
- **Vector**：HNSW 跳表 + AVX2 cosine/L2/dot SIMD kernel。
- **Cipher**：AES-256-GCM / RSA-OAEP / RSA-PSS（OpenSSL）。
- **Reflection**：struct↔JSON 绑定 marshal/unmarshal/free。

---

## 6. 协议与消息

- **WebSocket**：RFC6455 + frame 编解码；`csilk_ws_send` 每次 `malloc`（on_ws_write 回调释放，所有权依赖回调必触发）；`realloc` 失败静默丢数据无日志。
- **HTTP/2**：nghttp2，stream map（inline buckets + 动态扩展），stream refcount。
- **HTTP/3**：ngtcp2/nghttp3 + QUIC varint + QPACK。**半成品**：静态响应数组 + 不完整 ngtcp2 集成，`csilk_h3` 多 API 为 stub。
- **MCP**：stdio client process pool（可复用子进程），fork/exec 模型。
- **MQ**：异步线程安全 + PubSub + WAL（双 CRC）+ Raft RPC/consensus/snapshot。offload 走线程池（fire-and-forget，深拷贝）。
  - **风险**：MQ io_uring 同步内联 dispatch 打破"主循环异步投递"契约；topic 链表注册无锁，与发布遍历并发竞态；Raft `apply_cb`/`snapshot_apply_cb` 在 `node->mutex` 内调用用户代码，可能阻塞整个共识状态机；WAL 单全局 writer 线程，shutdown 下 cond_wait 唤醒依赖。

---

## 7. 中间件链（洋葱模型，按注册顺序）

| 中间件 | 核心风险 |
|---|---|
| jwt | 强 HMAC 校验 + `explicit_bzero` 清理签名栈；payload 存 ctx 一次性消费（`csilk_ctx_get_jwt_payload_json` 取后清 slot）；非对称校验重 CPU |
| session | ⚠️ `session_max` 非原子主线程 set_max vs worker 读写 = 数据竞争 UB；同 session 并发 KV 无锁写 |
| session_store | ⚠️ Redis 连接无锁共享，多 worker 并发 `redisCommand` 不安全；连接失败**静默 no-op 降级** |
| ratelimit | ⚠️ EVICT_LRU 饱和策略允许攻击流量**洗表重置限额**；window flip 时 count 偏乐观 |
| sliding_ratelimit | 全互斥，误用 per-key 实为全局限流 |
| otlp_trace | ⚠️ **arena==NULL 时把栈局部 `trace_id_buf/span_id_buf` 指针存入 ctx（kv_store.c 验证 csilk_set 仅存指针不拷贝）→ 栈帧销毁后 UAF** |
| gzip | 异步 work cb 跨线程读 body；gzip bomb 防护（≥10× 放弃）；deflate 单次 Z_FINISH |
| circuit_breaker | HALF_OPEN 不限探测并发 + 依赖业务手动 record_* |

---

## 8. 构建 / 测试 / CI

- **薄编排器** CMakeLists → 11 个 `cmake/*.cmake`；`cmake_minimum_required(3.24)` + C23；Release 默认；SIMD AVX2/AVX-512F 探测。
- **版本单一事实源** `cmake/options.cmake`，18 处由 `scripts/tag-release.sh` 同步，`check_version_sync.sh` 校验。
- **Sanitizer 变体各自独立 build 目录**：ASAN（Linux，`detect_leaks=1:abort_on_error=1`）/ TSAN（`halt_on_error=1`）/ Coverage（gcc，需 `ENABLE_OOM_TEST=OFF`）/ Fuzzer（clang-19+）。抑制文件 `cmake/.lsan-suppressions`、`.tsan-suppressions`。
- **clang-format pin v18**（`CSILK_CLANG_FORMAT_VERSION 18`）：Ubuntu 24.04 无 v22，CI 装 `clang-format-18`。本轮已全树归一到 v18，`session_store.c` 因 v18 对 C cast+解引用 `(uint32_t)*(uint8_t*)p` 的已知 bug 误改写，用 `/* clang-format off */` 守卫。
- **CI 矩阵**：ubuntu-24.04/macos-15 × Debug/Release + Lint + TSAN + Fuzz + io_uring（libuv+`UV_USE_IO_URING=1` + native `CSILK_USE_URING`）+ ARM64 交叉 + DB drivers + Perf + Coverage + Doxygen。当前全绿（12/12 + 4 辅助）。

---

## 9. 已验证的关键风险（按严重度排序）

1. **`ws_room.c` COW 快照生命周期竞态 → UAF（最严重）**：`on_room_message`（MQ 广播线程）load 快照后**释放 manager 锁**即无锁遍历 `snap->clients`（L119-125）；`csilk_ws_join_room`/`csilk_ws_leave_room` 在 `room->mutex` 下 `free(old_snap->clients)` + `free(old_snap)`（L198-203 / L255-258）。广播者可能持有正被并发释放的旧快照 → use-after-free。根因：COW 快照**缺引用计数**，join/leave 与 broadcast 不在同一把锁域内。修复方向：给 snapshot 加 atomic refcount（reader 读前 `fetch_add`、读完 `fetch_sub` 归零才 free；写者发布新快照后 `fetch_sub`），或广播遍历也持 `room->mutex`。
2. **`otlp_trace.c:200-201` 栈指针入 ctx → UAF**：arena==NULL 时 `csilk_set(c,"otlp_trace_id", stack_ptr)`；`csilk_next` 返回后栈帧销毁，后续 `csilk_ctx_get_trace_id` 读即 UAF。修复：arena 缺失时也 strdup 堆分配并登记 dtor，或干脆 arena 必有时才存。
3. **`session.c` `session_max` 数据竞争**：主线程 `set_max` 普通 int 写 vs worker 读 = TSAN 必报 UB。修复：`_Atomic int`。
4. **`session_store.c` Redis 共享连接无锁 + 失败静默 no-op**：多 worker 并发 `redisCommand` 不安全；持久化丢失无感知。修复：per-worker 连接或加锁；失败需显式降级告警。
5. **`ratelimit.c` EVICT_LRU 洗表绕过**：攻击流量填满表可重置限额。修复：饱和时拒绝/限速而非 LRU 驱逐。
6. **RCU reader 嵌套跨线程**：`_csilk_dispatch_request` 在 reader 区内调整条 handler 链；若某中间件 `csilk_next` 后跨线程 dispatch 同 ctx，嵌套 RCU slot 的 TLS 归属需复核。
7. **MQ io_uring 同步内联 dispatch** + topic 注册无锁并发。
8. **Raft apply 回调持锁调用用户代码**：可阻塞共识状态机。

---

## 10. 修复建议（本轮仅分析，未改动）

| 优先级 | 风险 | 建议 |
|---|---|---|
| P0 | ws_room UAF | snapshot 加 atomic refcount；reader 持有期间写者不得 free |
| P0 | otlp_trace UAF | arena 缺失路径堆分配 + dtor 登记 |
| P1 | session 数据竞争 | `session_max` 改 `_Atomic int` |
| P1 | session_store Redis | per-worker 连接池 + 失败告警 |
| P2 | ratelimit 洗表 | 饱和策略改拒绝 |
| P2 | MQ/Raft | apply 回调移出锁；topic 注册加锁 |

---

---

## 11. 总评（2026-11-17 @ `785aa5a7`，Post-concurrency-fixes）

csilk 是一个**工程密度极高**的 C23 框架：核心请求链路（RCU 读侧 handler 链、generation-tagged client 回收、arena bump 分配、双 IO 后端）经逐行复核**判定健康**，并发纪律是全项目最强资产。本轮确证的 5 项边缘缺陷（向量 dim 契约 / 限流 int 溢出 / 熔断单探针 / secret 锁内回调 / savepoint 截断）**已全部修复**（`d0e966e6`/`fe0a43e6`/`0b6d22fd`/`f7f7f7c9`/`f5d33801`，各带回归测试）；上轮已修复 6 项（ws_room refcount / otlp_trace / session_max / redis lock / ratelimit doc / mq topics_mutex）已合入。遗留仅 §12.3 的 3 项 P2 架构项（WAF O(n·m)、JWT timing oracle、Raft `apply_cb` 移锁内）按风险排期。

> 注：本文件 §1–§10 为早期 `88c1d7a7` 基线快照；§12 为 2026-11-17 全量复审（`785aa5a7` @ v1.1.0）；早期基线（`5ab5f305`）分析见同目录历史文档。

---

## 12. 2026-11-17 全量复审（`785aa5a7` @ v1.1.0，Post-concurrency-fixes）

> 方法：`code_explore` 语义图 + 逐行 `read` 独立验证；子代理候选缺陷**均经复核**，驳回 3 项误报。
> 上轮已修复的 6 项（ws_room refcount / otlp_trace / session_max / redis lock / ratelimit doc / mq topics_mutex）已合入，不在此列。

### 12.1 核心层健康结论（逐行验证）

- **连接回收**（`connection_close.c`）：`client_destroy` CAS(CLOSING→CLOSED) 幂等；非 owner 线程经 dispatch，回调三重判据 `generation==gen && state==CLOSING && ref<=0 && pending_io<=0`。**无 UAF/ABA 路径。**
- **RCU**（`server_rcu.c`）：reader TLS 快照 + `seq_cst` fence；writer `config_mutex` 下 exchange+epoch++；reclaim 取 `min_active_epoch`；`_csilk_reload_mgr_free`(L584-590) **确实**统一释放 overflow 动态槽（一度怀疑为进程级泄漏，复核为非缺陷）。**实现正确自洽。**
- **ctx 异步**（`ctx_async.c`）：`csilk_async_op_t` 带 generation/request_seq/stream_gen 三重标签 + `completed` CAS + refcount；`csilk_ctx_cleanup` 先释放 RCU 租约再 LIFO defer，顺序正确。

### 12.2 本轮确证的新遗留缺陷（5 项，均在边缘，非热路径）

> **状态：本轮全部已修复**（5 个独立 commit，`d0e966e6`/`fe0a43e6`/`0b6d22fd`/`f7f7f7c9`/`f5d33801`），每项含回归测试。

| 级别 | 位置 | 缺陷 | 触发 / 根因 | 修复 |
|---|---|---|---|---|
| **P1** | `drivers/vector/vector.c:182` | embedded HNSW 的 `csilk_vector_db_search` **丢弃** `dimension` 参数 | 内核恒用 `idx->dim`；调用方传错长度查询 → 结果静默错误。**降级自子代理误报的“堆 OOB”**——多出的 float 被丢弃，不越界。 | ✅ embedded 路径校验 `dimension==idx->dim`，不符返回 -1（`0b6d22fd`） |
| **P1** | `middleware/sliding_ratelimit.c:27-28` | 计数器 `int`(32-bit) 溢出使限流器静默失效 | `curr/prev_window_count` 有符号 32 位；单键 >2^31 次或 `weight` 中间值回绕为负 → `estimated_count>=limit` 永假 → 高流量下限流自动关闭。 | ✅ 计数器改 `uint64_t` + 0xFFFFFFFF 饱和截断，`limit` 改 `int64_t`（`d0e966e6`） |
| **P1** | `middleware/circuit_breaker.c:74-90` | HALF_OPEN 无单探针门控 | 冷却后**每个**到达请求各自翻 HALF_OPEN 并 `csilk_next`；`record_success` 单成功即 CLOSED。重试风暴下韧性失效。 | ✅ `half_open_probing` 单探针门控 + 探针失败回 OPEN + `half_open_success_threshold` 连续成功才 CLOSED（`fe0a43e6`） |
| **P2** | `core/config/secret.c:213-245` | 外部 secret provider 在全局锁内回调且无缓存 | `s_secret_manager(...)` 持 `s_secret_lock` 调用；vault 慢回调串行化全部 secret 读取、进程级 stall，且无负缓存。 | ✅ 回调移出锁 + 64 槽 FNV/TTL(60s) 缓存；过期用 `clock_gettime`（避免 libuv 耦合导致 OOM 测试 fork 卡死）（`f7f7f7c9`） |
| **P2** | `drivers/db/tx_scope.c:40-44` | savepoint 名 `snprintf` 截断 | `scope->name[128]`/`sql[128]` 无截断检查；超长名 → `SAVEPOINT <trunc>;` 语法错误。begin/commit/rollback 用同一截断串，故不会误操作外层 tx（**驳回子代理 P0 误判**）。 | ✅ 基础名+seq 后缀放不下时 begin 返回 -1，不再静默截断（`f5d33801`） |

### 12.3 待展开的 P2（见 §6 中间件表已有记录）

- WAF body 匹配 O(n·m)（`waf.c`，1MB×22 pattern 二次方 CPU，建议 Aho-Corasick + 工作预算）。
- JWT HS256 长度不匹配强制 `cmp_result=1` 的弱 timing oracle（`jwt.c:356-363`，建议恒定 32B `CRYPTO_memcmp`）。
- Raft `apply_cb`/`snapshot_apply_cb` 在 `node->mutex` 内回调用户代码（已知 P2 架构项，独立立项）。

### 12.4 已驳回的子代理误报（保持报告可信度）

| 声称 | 复核结论 | 证据 |
|---|---|---|
| HNSW 3072 维查询 → 堆 OOB（P0） | 降级 P1（契约断裂非 OOB） | `csilk_hnsw_search` 内核恒用 `idx->dim`，多余 float 被丢弃不越界 |
| AI 异步 calloc 失败 → `req_copy.messages` UAF（P1） | 误报，路径安全 | `ai.c:584-588` calloc 失败分支已正确置 `messages=NULL; message_count=0` |
| tx_scope 截断 → 误操作外层 savepoint（P0） | 降级 P2 | begin/commit/rollback 用同一截断串，仅超长名致语法错误 |

---

## 11. 总评（历史快照 `88c1d7a7`）
