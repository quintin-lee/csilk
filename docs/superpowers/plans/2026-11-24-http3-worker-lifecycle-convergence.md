# HTTP/3 (QUIC) 协议栈与 Multi-Worker 连接池全生命周期收敛实施计划 (Implementation Plan)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 HTTP/3 (QUIC) 协议栈与 Multi-Worker 核心底座及代际连接池深度收敛，实现基于 `SO_REUSEPORT` 的多 Worker 跨核线速分流、`client_pool` 代际管理（Generation Tag 防 ABA/UAF）、超时定时器监控与优雅关机生命周期对齐。

**Architecture:** 在 `worker_pool_t` 中引入 Worker 局部 `csilk_io_udp_t` 句柄与 `h3_transport`，由各 Worker 线程在 `SO_REUSEPORT` 下独立监听 UDP 443 端口。废除 `http3.c` 裸堆内存分配，通过 `pool_get(wp)` / `pool_put(wp)` 与 `client_list_add/remove` 将 HTTP/3 客户端完整接入状态机（`CSILK_CONN_ACCEPTED/PROCESSING/CLOSING`）与活跃连接追踪，并在 `server_shutdown.c` 中闭环优雅停机。

**Tech Stack:** C23, libuv / io_uring (`csilk_io_udp_t`), ngtcp2, nghttp3, OpenSSL QUIC, Clang 19 / GCC 13.

**Spec:** [docs/superpowers/specs/2026-11-24-http3-worker-lifecycle-convergence-design.md](file:///data/home/quintin/workspace/source/c/csilk/docs/superpowers/specs/2026-11-24-http3-worker-lifecycle-convergence-design.md)

## Global Constraints

- C23 standard compliance (`CMAKE_C_STANDARD 23`).
- Strict LLVM 18 `clang-format` (`cmake --build build --target check-format`).
- Zero broken baseline public symbols (`cmake/abi_baseline_symbols.txt`).
- All public functions must have `csilk_` prefix; internal helpers use `_csilk_`.
- No raw pthread or direct unabstracted IO in cross-backend code; use `csilk_io_*` and `csilk_thread_*`.
- Automatic git push and monitor GitHub Actions CI to completion upon finishing.

## Review Focus

1. **UDP SO_REUSEPORT 跨平台与跨后端兼容**：在 macOS（仅单 Worker 支持）与 Linux、libuv 与 io_uring 双后端下的安全降级。
2. **连接池代数（Generation Tag）防 ABA 校验**：回收任务在极端乱序与竞态下验证 `client->generation == gen && client->state == CSILK_CONN_CLOSING`。
3. **关闭时 TCP 句柄与 UDP 区分**：HTTP/3 客户端的 `client->handle` 为未初始化的 TCP handle，`close_active_clients` 严禁对其调用 `csilk_io_close`。
4. **Arena 资源归还**：HTTP/3 客户端使用 `pool_get_arena(wp)` 获取的 Arena 必须通过 `pool_put_arena(wp)` 归还，禁止双重释放。
5. **TSAN 数据竞争防线**：各 Worker 的连接表与客户端列表必须 100% 线程局部约束（Thread-Confined），跨线程通信必须使用 `csilk_dispatch()`。

---

## Tasks

### Task 1: Worker Pool HTTP/3 Handle Infrastructure & State Tracking

**Deliverable:** `worker_pool_t` 拥有 `h3_udp_handle`、`h3_udp_active`、`h3_transport` 成员，`csilk_server_t` 拥有 `h3_port` 与 `h3_ssl_ctx`，且 `reset_hot_state` 重置 `client->h3_conn`。

- [ ] **Step 1: Write the failing unit test**
  - Create `tests/protocols/test_h3_worker.c` checking that worker pools carry clean H3 transport/handle fields and `reset_hot_state` clears `h3_conn`.
  - Register `test_h3_worker` in `cmake/tests.cmake` under `CSILK_PROTOCOL_TESTS`.
- [ ] **Step 2: Run test to make sure it fails (Red)**
  - Run `ctest --test-dir build -R test_h3_worker --output-on-failure`.
- [ ] **Step 3: Implement worker pool data structures & hot state reset**
  - Edit [src/core/internal/srv_internal.h](file:///data/home/quintin/workspace/source/c/csilk/src/core/internal/srv_internal.h):
    - Add `csilk_io_udp_t h3_udp_handle; bool h3_udp_active; void* h3_transport;` to `struct CSILK_CACHE_ALIGNED worker_pool_t`.
    - Add `int h3_port; void* h3_ssl_ctx;` to `struct csilk_server_s`.
  - Edit [src/core/server/connection_pool.c](file:///data/home/quintin/workspace/source/c/csilk/src/core/server/connection_pool.c):
    - In `reset_hot_state(client)`, ensure `client->h3_conn = NULL;` is cleared alongside `h2_session`.
  - Edit [src/protocols/http3_internal.h](file:///data/home/quintin/workspace/source/c/csilk/src/protocols/http3_internal.h):
    - Add `void* wp;` (pointing to `worker_pool_t*`) to `struct csilk_h3_transport_s`.
- [ ] **Step 4: Run test to verify it passes (Green)**
  - Run `ctest --test-dir build -R test_h3_worker --output-on-failure`.
- [ ] **Step 5: Format and commit**
  - Run `cmake --build build --target format && cmake --build build --target check-format`.
  - Commit: `git commit -am "feat(h3): 🎯 add worker-pool HTTP/3 handle fields and client hot-state reset"`.

---

### Task 2: Worker-Confined HTTP/3 Connection Pool Integration (`pool_get` / `pool_put` & Generation Safety)

**Deliverable:** HTTP/3 连接与流彻底废除裸 `calloc/free`，改用 `pool_get(wp)` / `pool_put(wp)` 与 `client_list_add/remove`，集成 Generation Tag、`csilk_conn_state_t` 与空闲超时定时器。

- [ ] **Step 1: Write the failing unit tests for H3 pool lifecycle**
  - Add test cases in `tests/protocols/test_h3_worker.c`:
    - Verify `_h3_new_conn` retrieves `csilk_client_t` with incremented `generation` and state `CSILK_CONN_ACCEPTED`.
    - Verify client is registered in `wp->active_clients` and `wp->active_connections` increments.
    - Verify `_h3_del_conn` sets state `CSILK_CONN_CLOSING`, unlinks from `wp->active_clients`, and returns client to `wp->client_pool`.
    - Verify keep-alive timeout arms `client->timer`.
- [ ] **Step 2: Run test to make sure it fails (Red)**
  - Run `ctest --test-dir build -R test_h3_worker --output-on-failure`.
- [ ] **Step 3: Refactor `_h3_new_conn` and `_h3_del_conn`**
  - Edit [src/protocols/http3.c](file:///data/home/quintin/workspace/source/c/csilk/src/protocols/http3.c):
    - In `_h3_new_conn`, determine `worker_pool_t* wp = (worker_pool_t*)t->wp;`:
      - If `wp` exists, allocate client via `pool_get(wp)` and arena via `pool_get_arena(wp)` (with fallback to `calloc` / `csilk_arena_new` when standalone transport without server);
      - Set `cl->protocol = CSILK_PROTO_HTTP3; cl->owner_pool = wp; cl->server = (csilk_server_t*)t->server; cl->h3_conn = c; c->client = cl;`;
      - Set `csilk_conn_set_state(cl, CSILK_CONN_ACCEPTED);`;
      - If `wp` and `t->server`, call `client_list_add((csilk_server_t*)t->server, cl);`;
      - Arm `cl->timer` for idle timeout if `t->server` has `runtime_config.idle_timeout_ms > 0`.
    - In `_h3_del_conn`:
      - Set `csilk_conn_set_state(cl, CSILK_CONN_CLOSING);`;
      - Stop timers via `csilk_io_timer_stop(&cl->timer)` and `csilk_io_timer_stop(&cl->request_timer)`;
      - If `wp` and `t->server`, call `client_list_remove((csilk_server_t*)t->server, cl);`;
      - Release arena via `pool_put_arena(wp, cl->ctx.arena)` (or `csilk_arena_free`);
      - Return client to pool via `pool_put(wp, cl)` (or `free(cl)` if standalone);
      - Nullify `c->client = NULL;`.
  - Edit [src/protocols/h3_session.c](file:///data/home/quintin/workspace/source/c/csilk/src/protocols/h3_session.c):
    - In `csilk_h3_dispatch_stream`, update `csilk_conn_set_state(client, CSILK_CONN_PROCESSING);`.
- [ ] **Step 4: Run tests and ensure they pass (Green)**
  - Run `ctest --test-dir build -R test_h3_worker --output-on-failure`.
  - Run `ctest --test-dir build -R test_h3 --output-on-failure`.
- [ ] **Step 5: Format and commit**
  - Run `cmake --build build --target format && cmake --build build --target check-format`.
  - Commit: `git commit -am "feat(h3): 🎯 integrate HTTP/3 connections into worker lock-free pool and generation tracking"`.

---

### Task 3: Multi-Worker UDP Binding with SO_REUSEPORT, Datagram Event Dispatch & Graceful Drain

**Deliverable:** Multi-Worker 线程独立绑定 UDP `SO_REUSEPORT`，事件循环非阻塞驱动 UDP 接收并分流，优雅停机安全清理 H3 连接与句柄。

- [ ] **Step 1: Write integration test for Multi-Worker HTTP/3 binding and graceful shutdown**
  - Add test in `tests/protocols/test_h3_worker.c` simulating multi-worker server startup with HTTP/3 enabled, sending UDP packets, verifying worker dispatch, and triggering graceful stop.
- [ ] **Step 2: Run test to make sure it fails (Red)**
  - Run `ctest --test-dir build -R test_h3_worker --output-on-failure`.
- [ ] **Step 3: Implement multi-worker UDP binding and shutdown handling**
  - Edit [src/core/server/server_worker.c](file:///data/home/quintin/workspace/source/c/csilk/src/core/server/server_worker.c):
    - In `worker_thread`, check if `server->h3_transport` or HTTP/3 is enabled on server:
      - Initialize UDP handle: `csilk_io_udp_init(loop_ptr, &wp->h3_udp_handle)`;
      - Bind with `SO_REUSEPORT` to `0.0.0.0:h3_port`;
      - Clone or instantiate worker-local transport `csilk_h3_transport_create_worker(server, wp, ...)` attached to `wp->h3_transport`;
      - Start receiving with `csilk_io_udp_recv_start(&wp->h3_udp_handle, on_worker_h3_datagram_recv)`;
      - Mark `wp->h3_udp_active = true`.
    - In worker teardown:
      - Stop UDP receiving with `csilk_io_udp_recv_stop(&wp->h3_udp_handle)`;
      - Close UDP handle cleanly with `csilk_io_udp_close(&wp->h3_udp_handle, NULL)`.
  - Edit [src/core/server/server_shutdown.c](file:///data/home/quintin/workspace/source/c/csilk/src/core/server/server_shutdown.c):
    - In `close_active_clients`, check `client->protocol`:
      - If `client->protocol == CSILK_PROTO_HTTP3`:
        - Do NOT call `csilk_io_close((csilk_io_handle_t*)&client->handle)` (handle is unused TCP);
        - Call `csilk_h3_transport_close_conn(wp->h3_transport, client->h3_conn)` to trigger QUIC graceful teardown and pool recycling.
- [ ] **Step 4: Run all protocol and server tests (Green)**
  - Run `ctest --test-dir build -R test_h3 --output-on-failure`.
  - Run `ctest --test-dir build -R test_server --output-on-failure`.
  - Run `ctest --test-dir build -R test_connection --output-on-failure`.
  - Run `ctest --test-dir build -R test_multi_worker --output-on-failure`.
- [ ] **Step 5: Format and commit**
  - Run `cmake --build build --target format && cmake --build build --target check-format`.
  - Commit: `git commit -am "feat(server): 🎯 bind multi-worker HTTP/3 UDP SO_REUSEPORT and coordinate graceful shutdown"`.

---

### Task 4: Full Local Regression, ABI Gate Verification & Remote CI Push/Monitoring

**Deliverable:** 285+ 本地测试 100% 通过，ABI 门禁 100% 合规，代码格式化零报警，推送到远程仓库并监控 GitHub Actions CI 全绿。

- [ ] **Step 1: Run full test suite locally**
  - Run `ctest --test-dir build -E test_integration --timeout 30 --output-on-failure`.
  - Run `ctest --test-dir build -R test_integration --output-on-failure`.
- [ ] **Step 2: Run ABI Gate verification**
  - Run `./scripts/check_abi_compatibility.sh`.
- [ ] **Step 3: Run formatting verification**
  - Run `cmake --build build --target check-format`.
- [ ] **Step 4: Push to origin/master**
  - Run `git push origin master`.
- [ ] **Step 5: Monitor GitHub Actions CI to completion**
  - Run `gh run watch <run-id> --exit-status` to monitor all CI matrix jobs until green.
