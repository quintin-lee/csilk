# Event-Loop-Driven Non-Blocking Asynchronous Database Driver Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Transform PostgreSQL and MySQL drivers into true event-loop-driven asynchronous state machines attached to `csilk_io_loop_t`, with async connection lease independence, FIFO queueing, and query timeouts.

**Architecture:** Introduce `csilk_io_poll_t` socket polling abstraction in `sys_io.h`. Refactor `csilk_db_pool_t` in `db.c` to support concurrent async leases on a single thread and FIFO wait queueing. Replace synchronous `poll()` in `postgres.c` and `mysql.c` with event-driven poll callbacks, query timers, and automatic poisoned connection sanitization.

**Tech Stack:** C23, libuv, io_uring, libpq (PostgreSQL), libmariadb / libmysqlclient, ctest, ASAN, TSAN.

**Spec:** [`docs/superpowers/specs/2026-11-24-async-db-event-loop-convergence-design.md`](file:///data/home/quintin/workspace/source/c/csilk/docs/superpowers/specs/2026-11-24-async-db-event-loop-convergence-design.md)

## Global Constraints

- Language Standard: C23 (`CMAKE_C_STANDARD 23`), built-in `nullptr`, `bool`, `constexpr`.
- Code Formatting: Strict LLVM 18 clang-format (`cmake --build build --target check-format`).
- ABI Compatibility: SOVERSION 1 backward compatibility (`./scripts/check_abi_compatibility.sh`).
- Concurrency & Thread-Safety: Zero data races under TSAN; strictly no raw `uv_*` or `pthread_*` in driver code.
- Worker Confinement: All asynchronous callbacks must fire strictly on the loop owning the operation.

## Review Focus

1. Concurrent async queries on single worker thread acquiring distinct connection instances without colliding.
2. Connection pool exhaustion queueing requests in FIFO order and dispatching upon connection release without leaking or deadlocking.
3. Socket disconnect / error during async query destroying corrupt connection rather than returning it to idle pool.
4. 30-second query timeout firing, cancelling in-flight query (`PQcancel`), and reporting `CSILK_DB_ERR_TIMEOUT`.
5. Parameterized SQL queries on MySQL escaping special characters (`'`, `"`, `\0`, `\n`) safely without SQL injection vulnerabilities.

---

### Task 1: Cross-Backend Socket Polling Abstraction (`csilk_io_poll_t`)

**Files:**
- Modify: `include/csilk/core/sys_io.h`
- Create: `tests/core/test_io_poll.c`
- Modify: `cmake/tests.cmake`

**Interfaces:**
- Produces:
  - `typedef ... csilk_io_poll_t;`
  - `typedef void (*csilk_io_poll_cb)(csilk_io_poll_t* handle, int status, int events);`
  - `#define CSILK_IO_READABLE ...`, `#define CSILK_IO_WRITABLE ...`, `#define CSILK_IO_DISCONNECT ...`
  - `int csilk_io_poll_init(csilk_io_loop_t* loop, csilk_io_poll_t* handle, int fd);`
  - `int csilk_io_poll_init_socket(csilk_io_loop_t* loop, csilk_io_poll_t* handle, csilk_io_os_sock_t socket);`
  - `int csilk_io_poll_start(csilk_io_poll_t* handle, int events, csilk_io_poll_cb cb);`
  - `int csilk_io_poll_stop(csilk_io_poll_t* handle);`

- [ ] **Step 1: Write the failing unit test for `csilk_io_poll_t`**

Create `tests/core/test_io_poll.c`:
```c
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include "csilk/csilk.h"
#include "csilk/core/sys_io.h"

static int s_poll_cb_called = 0;

static void on_poll_event(csilk_io_poll_t* handle, int status, int events) {
    (void)handle;
    (void)status;
    if (events & CSILK_IO_READABLE) {
        s_poll_cb_called++;
        char c;
        read(handle->fd, &c, 1);
        csilk_io_poll_stop(handle);
    }
}

int main(void) {
    csilk_io_loop_t* loop = csilk_io_default_loop();
    int fds[2];
    assert(pipe(fds) == 0);

    csilk_io_poll_t poll_handle;
    int rc = csilk_io_poll_init(loop, &poll_handle, fds[0]);
    assert(rc == 0);

    rc = csilk_io_poll_start(&poll_handle, CSILK_IO_READABLE, on_poll_event);
    assert(rc == 0);

    assert(write(fds[1], "x", 1) == 1);
    csilk_io_run(loop, CSILK_IO_RUN_ONCE);

    assert(s_poll_cb_called == 1);
    close(fds[0]);
    close(fds[1]);
    printf("test_io_poll: PASS\n");
    return 0;
}
```

- [ ] **Step 2: Add test to `cmake/tests.cmake` and verify compilation fails**

Register `test_io_poll` in `cmake/tests.cmake`.
Run: `cmake --build build --target test_io_poll`
Expected: FAIL due to missing `csilk_io_poll_*` declarations.

- [ ] **Step 3: Implement `csilk_io_poll_t` in `include/csilk/core/sys_io.h`**

Define `csilk_io_poll_t` mapping to `uv_poll_t` in libuv mode and poll SQE handling in io_uring mode.

- [ ] **Step 4: Build and run `test_io_poll`**

Run: `ctest --test-dir build -R test_io_poll --output-on-failure`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add include/csilk/core/sys_io.h tests/core/test_io_poll.c cmake/tests.cmake
git commit -m "feat(io): 🎯 add cross-backend csilk_io_poll_t socket polling abstraction"
```

---

### Task 2: Connection Pool Async Leases & FIFO Pending Queue

**Files:**
- Modify: `src/drivers/db/db_internal.h`
- Modify: `src/drivers/db/db.c`
- Modify: `tests/drivers/db/test_db_nonblocking.c`

**Interfaces:**
- Consumes: `csilk_db_pool_t`, `csilk_thread_self()`
- Produces:
  - `int csilk_db_pool_acquire_async(csilk_db_pool_t* pool, void** conn, int* lease_id);`
  - `void csilk_db_pool_put_async(csilk_db_pool_t* pool, void* conn, int lease_id, bool is_healthy);`
  - FIFO wait queue inside `csilk_db_pool_t`: `async_wait_head`, `async_wait_tail`.

- [ ] **Step 1: Write failing tests in `tests/drivers/db/test_db_nonblocking.c`**

Add tests:
- `test_concurrent_async_leases_same_thread()`: 10 concurrent async acquires on thread 0 return 10 distinct connection pointers.
- `test_async_pool_fifo_queue()`: When pool capacity is 2 and 5 async queries arrive, 3 queue up and drain as connections return.
- `test_poisoned_connection_eviction()`: Returning a connection flagged unhealthy destroys it and spawns a clean replacement.

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --test-dir build -R test_db_nonblocking --output-on-failure`
Expected: FAIL on multi-lease assertion.

- [ ] **Step 3: Implement async lease tracking and FIFO wait queue in `src/drivers/db/db.c`**

In `csilk_db_pool_t`:
- Add `uint8_t conn_is_async[CSILK_DB_MAX_POOL_SIZE]` and `int conn_lease_counter`.
- Implement `csilk_db_pool_acquire_async()` and `csilk_db_pool_put_async()`.
- Add FIFO wait queue linked list (`db_async_waiter_t`) to hold pending queries when pool is full, with 5000ms acquire timer.

- [ ] **Step 4: Run test to verify it passes**

Run: `ctest --test-dir build -R test_db_nonblocking --output-on-failure`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add src/drivers/db/db_internal.h src/drivers/db/db.c tests/drivers/db/test_db_nonblocking.c
git commit -m "feat(db): 🎯 implement async connection lease independence and FIFO pending queue"
```

---

### Task 3: Event-Driven PostgreSQL Async State Machine (`postgres.c`)

**Files:**
- Modify: `src/drivers/db/postgres.c`
- Modify: `tests/drivers/db/test_db_postgres.c`

**Interfaces:**
- Consumes: `csilk_io_poll_t`, `csilk_io_timer_t`, `csilk_db_pool_acquire_async`, `csilk_db_pool_put_async`
- Produces:
  - Event-driven `postgres_query_async()` and `postgres_exec_async()` without `poll()` busy loops.
  - Automatic 30s timeout and dirty socket eviction.

- [ ] **Step 1: Write failing test in `tests/drivers/db/test_db_postgres.c`**

Add `test_postgres_async_100_queries()`:
- Configure pool with 10 connections.
- Launch 100 concurrent `csilk_db_query_async()` calls on the same event loop.
- Run `csilk_io_run(loop, CSILK_IO_RUN_DEFAULT)` until all 100 complete.
- Verify all 100 results match `SELECT 1 AS num`.

- [ ] **Step 2: Run test to verify failure or synchronous blocking**

Run: `ctest --test-dir build -R test_db_postgres --output-on-failure`
Expected: Verify current blocking behavior / missing concurrency.

- [ ] **Step 3: Implement asynchronous poll state machine in `src/drivers/db/postgres.c`**

Replace `(void)loop;` and synchronous `poll(&pfd, 1, 100)` with:
- Allocate `pg_async_op_ctx_t`.
- `PQsetnonblocking(conn->db, 1)` and `PQsendQueryParams` / `PQsendQuery`.
- `csilk_io_timer_start(&ctx->timer, on_pg_timeout, 30000, 0)`.
- If `PQflush() > 0`, poll for `CSILK_IO_WRITABLE`; if `PQflush() == 0`, poll for `CSILK_IO_READABLE`.
- In `on_pg_poll_event()`:
  - Flush state: continue `PQflush()`. When 0, switch to `CSILK_IO_READABLE`.
  - Read state: call `PQconsumeInput()`. If disconnected, mark poisoned, close, and fail. If `!PQisBusy()`, drain `PQgetResult()`, convert to JSON, stop timer, close poll, return connection, and dispatch `ctx->query_cb`.

- [ ] **Step 4: Run test to verify it passes**

Run: `ctest --test-dir build -R test_db_postgres --output-on-failure`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add src/drivers/db/postgres.c tests/drivers/db/test_db_postgres.c
git commit -m "feat(postgres): 🎯 implement event-loop-driven non-blocking async query state machine"
```

---

### Task 4: Event-Driven MySQL Async State Machine & Parameter Interpolation (`mysql.c`)

**Files:**
- Modify: `src/drivers/db/mysql.c`
- Modify: `tests/drivers/db/test_db_mysql.c`

**Interfaces:**
- Consumes: `csilk_io_poll_t`, `csilk_io_timer_t`, `csilk_db_pool_acquire_async`, `csilk_db_pool_put_async`
- Produces:
  - Client-side SQL parameter escaping with `mysql_real_escape_string_quote`.
  - MariaDB non-blocking `mysql_real_query_start` / `cont` driven via `csilk_io_poll_t`.
  - Oracle MySQL fallback via `csilk_io_queue_work`.

- [ ] **Step 1: Write failing test in `tests/drivers/db/test_db_mysql.c`**

Add tests:
- `test_mysql_async_parameterized_query()`: query with parameters (`?`) containing quotes and special chars.
- `test_mysql_async_concurrency()`: 50 concurrent async queries.

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --test-dir build -R test_db_mysql --output-on-failure`
Expected: FAIL due to ignored parameters (`(void)params;`).

- [ ] **Step 3: Implement parameter escaping and poll state machine in `src/drivers/db/mysql.c`**

- For parameterized queries (`params != NULL`), interpolate `?` placeholders using `mysql_real_escape_string_quote` into an allocated SQL query.
- For MariaDB: register `csilk_io_poll_t` for `MYSQL_WAIT_READ` / `MYSQL_WAIT_WRITE`. On poll events, invoke `mysql_real_query_cont` and `mysql_store_result_cont`.
- For Oracle MySQL: gracefully offload to `csilk_io_queue_work` so `csilk_db_set_nonblocking(pool, 1)` succeeds (`return 0`).

- [ ] **Step 4: Run test to verify it passes**

Run: `ctest --test-dir build -R test_db_mysql --output-on-failure`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add src/drivers/db/mysql.c tests/drivers/db/test_db_mysql.c
git commit -m "feat(mysql): 🎯 implement async parameter interpolation and event-driven socket polling"
```

---

### Task 5: Context Loop Accessor & Full Integration Verification

**Files:**
- Modify: `include/csilk/core/ctx/context.h`
- Modify: `src/core/ctx/context.c`
- Modify: `src/core/server/server_worker.c`
- Modify: `tests/drivers/db/test_db_nonblocking.c`

**Interfaces:**
- Produces:
  - `csilk_io_loop_t* csilk_ctx_get_loop(const csilk_ctx_t* ctx);`
  - Automatic resolution in `csilk_db_query_async` when `loop == NULL` via worker TLS.

- [ ] **Step 1: Write integration test**

In `test_db_nonblocking.c`:
- Simulate HTTP handler calling `csilk_ctx_get_loop(ctx)` and passing it to `csilk_db_query_async()`.
- Verify `csilk_ctx_get_loop()` returns the worker loop.
- Verify when `loop == NULL` is passed, the query resolves to the current worker's loop via TLS.

- [ ] **Step 2: Implement `csilk_ctx_get_loop` and TLS loop fallback**

In `context.c`:
```c
csilk_io_loop_t* csilk_ctx_get_loop(const csilk_ctx_t* ctx) {
    if (!ctx || !ctx->_internal_client) return csilk_io_default_loop();
    csilk_client_t* cl = (csilk_client_t*)ctx->_internal_client;
    if (cl->owner_pool) return &cl->owner_pool->loop;
    if (cl->server && cl->server->loop) return cl->server->loop;
    return csilk_io_default_loop();
}
```
In `db.c`: when `loop == NULL`, check `_csilk_get_current_worker_pool()`.

- [ ] **Step 3: Run full test suite and quality gates**

```bash
# Format
cmake --build build --target format
cmake --build build --target check-format

# Run all unit tests
ctest --test-dir build -E test_integration --output-on-failure

# ABI check
./scripts/check_abi_compatibility.sh
```

- [ ] **Step 4: Commit**

```bash
git add include/csilk/core/ctx/context.h src/core/ctx/context.c src/drivers/db/db.c tests/drivers/db/test_db_nonblocking.c
git commit -m "feat(ctx): 🎯 add csilk_ctx_get_loop and auto-resolve worker loop for async DB queries"
```
