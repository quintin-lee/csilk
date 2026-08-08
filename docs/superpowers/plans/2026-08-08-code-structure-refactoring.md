# Code Structure Refactoring Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split 6 large `.c` files (>800 lines) into smaller, single-responsibility units while maintaining ABI compatibility.

**Architecture:** Pure internal refactoring -- extract functions from monolithic `.c` files into new focused `.c` files. Create minimal internal headers where cross-file function declarations are needed. Update `cmake/sources.cmake` to include new files. Zero changes to public headers in `include/csilk/`.

**Tech Stack:** C23, CMake, clang-format, clang-tidy

**Spec:** `docs/superpowers/specs/2026-08-08-code-structure-refactoring-design.md`

---

## File Structure

### New Files to Create

| File | Responsibility |
|------|---------------|
| `src/core/ctx/ctx_accessors.c` | Context getter/setter accessors, iteration, WebSocket callbacks |
| `src/core/http/http1_parse.c` | HTTP/1.1 request parsing: llhttp callbacks, header processing, body, dispatch |
| `src/core/http/http1_response.c` | HTTP/1.1 response building: serialization, write pipeline, keep-alive |
| `src/app/app_internal.h` | Internal `struct csilk_app_s` definition for cross-file use |
| `src/app/app_routes.c` | Route registration and group management |
| `src/workflow/wf_ai_internal.h` | Internal declaration of `ai_node_handler` for cross-file use |
| `src/workflow/wf_ai_utils.c` | Memory helpers, JSON path, context memory, agent memory |
| `src/workflow/wf_ai_nodes.c` | Template engine, AI chat handler, vector search handler |
| `src/workflow/wf_ai_agents.c` | Agent node types: ReAct, Reflexion, HITL, multi-agent worker |
| `src/core/server/server_shutdown.c` | Connection drain and graceful shutdown callbacks |
| `src/core/server/server_workers.c` | Worker thread creation, CPU affinity, bind/listen, dispatch |
| `src/core/uring/uring_loop.c` | io_uring event loop, worker thread, bind/listen, shutdown |
| `src/core/uring/uring_dispatch.c` | Cross-thread request dispatch for io_uring backend |

### Files to Modify

| File | Change |
|------|--------|
| `cmake/sources.cmake` | Replace monolithic files with new split files |
| `src/core/internal/srv_impl.h` | Add shutdown/worker function declarations |

### Files to Delete (replaced by splits)

| File | Replaced By |
|------|-------------|
| `src/workflow/wf_ai.c` | `wf_ai_utils.c` + `wf_ai_nodes.c` + `wf_ai_agents.c` |
| `src/core/http/http1.c` | `http1_parse.c` + `http1_response.c` |

---

### Task 1: Verify Baseline

**Files:** None (verification only)

- [ ] **Step 1: Build the project**

```bash
cd build && cmake .. && make -j$(nproc) 2>&1 | tail -5
```

Expected: Build succeeds with no errors.

- [ ] **Step 2: Run all tests**

```bash
cd build && ctest --output-on-failure 2>&1 | tail -20
```

Expected: All tests pass. Record the count (e.g., "100% tests passed, 0 tests failed out of 148").

- [ ] **Step 3: Run clang-tidy**

```bash
cd build && make tidy 2>&1 | tail -5
```

Expected: No errors.

---

### Task 2: Split `context.c` -> `context.c` + `ctx_accessors.c`

**Files:**
- Create: `src/core/ctx/ctx_accessors.c`
- Modify: `src/core/ctx/context.c` (remove extracted functions)
- Modify: `cmake/sources.cmake:20` (add ctx_accessors.c)

This is the lowest-risk split: pure getter/setter extraction with no shared globals.

- [ ] **Step 1: Create `ctx_accessors.c` with extracted functions**

Create `src/core/ctx/ctx_accessors.c` containing these functions moved from `context.c`:

**Accessors (all trivial getters/setters):**
- `csilk_get_method` (line 326)
- `csilk_get_path` (line 341)
- `csilk_get_param` (line 96)
- `csilk_get_params_count` (line 114)
- `csilk_get_param_key` (line 125)
- `csilk_get_param_value` (line 139)
- `csilk_get_header` (line 158)
- `csilk_get_response_header` (line 174)
- `csilk_get_query` (line 191)
- `csilk_set_request_header` (line 205)
- `csilk_get_headers` (line 493)
- `csilk_get_request_id` (line 452)
- `csilk_set_request_id` (line 564)
- `csilk_get_arena` (line 468)
- `csilk_is_websocket` (line 379)
- `csilk_ctx_set_websocket` (line 389)
- `csilk_is_sse` (line 402)
- `csilk_ctx_set_sse` (line 412)
- `_csilk_get_internal_client` (line 424)
- `_csilk_set_internal_client` (line 434)
- `csilk_get_status` (line 479)
- `csilk_ctx_set_async` (line 510)
- `csilk_is_async` (line 544)
- `csilk_ctx_get_server` (line 522)
- `csilk_ctx_get_mq` (line 532)
- `csilk_get_handler_index` (line 554)
- `csilk_get_work_req` (line 576)
- `csilk_is_aborted` (line 693)
- `csilk_ctx_get_handler_path` (line 612)
- `csilk_ctx_get_handler_perm_required` (line 622)
- `csilk_ctx_get_handler_perm_resource` (line 632)

**Iteration:**
- `for_each_in_map` (static, line 700)
- `csilk_for_each_header` (line 717)
- `csilk_for_each_query` (line 725)
- `csilk_for_each_form_field` (line 733)

**WebSocket callbacks:**
- `csilk_set_on_ws_message` (line 753)
- `csilk_set_on_ws_send` (line 775)
- `csilk_get_on_ws_message` (line 851)

The file needs these includes (copy from context.c):
```c
#include "csilk/csilk.h"
#include "core/ctx/ctx_internal.h"
#include "core/internal/srv_internal.h"
```

Copy the exact function bodies from `context.c`, preserving all comments and formatting.

- [ ] **Step 2: Remove extracted functions from `context.c`**

Delete the function bodies listed above from `src/core/ctx/context.c`. Keep:
- `_csilk_ctx_init` (line 793)
- `csilk_ctx_cleanup` (line 230)
- `tls_large_body_pool_cleanup` (static, line 213)
- `tls_large_body_pool` (static _Thread_local, line 211)
- `csilk_next` (line 60)
- `csilk_abort` (line 80)
- `csilk_get_body` (line 354)
- `csilk_get_body_len` (line 368)
- `csilk_set_status` (line 485)
- `csilk_get_response_body` (line 645)
- `csilk_set_response_body` (line 673)
- `csilk_set_file_response` (line 588)
- `csilk_get_file_fd` (line 602)
- `csilk_ctx_set_storage_driver` (line 815)
- `csilk_ctx_set_crypto_driver` (line 827)
- `csilk_ctx_set_cipher_driver` (line 839)

- [ ] **Step 3: Update `cmake/sources.cmake`**

Add `ctx_accessors.c` after `context.c` in `CSILK_CORE_SOURCES`:

```cmake
    src/core/ctx/context.c
    src/core/ctx/ctx_accessors.c
    src/core/ctx/ctx_defer.c
```

- [ ] **Step 4: Build and verify**

```bash
cd build && cmake .. && make -j$(nproc) 2>&1 | grep -E "error|warning" | head -20
```

Expected: Zero errors, zero new warnings.

- [ ] **Step 5: Run tests**

```bash
cd build && ctest --output-on-failure 2>&1 | tail -5
```

Expected: All tests pass (same count as baseline).

- [ ] **Step 6: Format and commit**

```bash
cd /home/quintin/Data/source/c_cpp/server-c
make -C build format
git add src/core/ctx/ctx_accessors.c src/core/ctx/context.c cmake/sources.cmake
git commit -m "refactor(ctx): ♻️ extract accessor functions to ctx_accessors.c"
```

---

### Task 3: Split `http1.c` -> `http1_parse.c` + `http1_response.c`

**Files:**
- Create: `src/core/http/http1_parse.c`, `src/core/http/http1_response.c`
- Delete: `src/core/http/http1.c`
- Modify: `cmake/sources.cmake:24` (replace http1.c)

- [ ] **Step 1: Create `http1_parse.c`**

Move these functions from `http1.c`:
- `on_message_begin` (static, line 177)
- `on_url` (static, line 204)
- `on_header_field` (static, line 249)
- `on_header_value` (static, line 327)
- `on_headers_complete` (static, line 361)
- `on_body` (static, line 387)
- `on_message_complete` (static, line 961)
- `finalize_request` (static, line 912)
- `_csilk_persist_header` (line 37)
- `_csilk_dispatch_request` (line 585)
- `buf_grow` (static, line 295)

Include the necessary headers and any static helper types used by these functions.

- [ ] **Step 2: Create `http1_response.c`**

Move these functions from `http1.c`:
- `get_status_text` (line 436)
- `serialize_status_line` (static, line 700)
- `append_custom_headers` (static, line 746)
- `_csilk_send_response` (line 817)
- `on_sendfile_complete` (static, line 61)
- `on_write` (static, line 118)
- `_csilk_handle_post_response` (line 774)
- `csilk_client_write` (line 476)
- `_csilk_send_data` (line 520)
- `_csilk_send_data_owned` (line 537)

- [ ] **Step 3: Delete `http1.c`**

```bash
git rm src/core/http/http1.c
```

- [ ] **Step 4: Update `cmake/sources.cmake`**

Replace `src/core/http/http1.c` with:
```cmake
    src/core/http/http1_parse.c
    src/core/http/http1_response.c
```

- [ ] **Step 5: Build and verify**

```bash
cd build && cmake .. && make -j$(nproc) 2>&1 | grep -E "error|warning" | head -20
```

Expected: Zero errors. Fix any missing symbol declarations by adding them to `srv_impl.h`.

- [ ] **Step 6: Run tests**

```bash
cd build && ctest --output-on-failure 2>&1 | tail -5
```

Expected: All tests pass.

- [ ] **Step 7: Format and commit**

```bash
cd /home/quintin/Data/source/c_cpp/server-c
make -C build format
git add src/core/http/http1_parse.c src/core/http/http1_response.c cmake/sources.cmake
git commit -m "refactor(http): ♻️ split http1.c into http1_parse.c and http1_response.c"
```

---

### Task 4: Split `app.c` -> `app.c` + `app_routes.c`

**Files:**
- Create: `src/app/app_internal.h`, `src/app/app_routes.c`
- Modify: `src/app/app.c` (remove extracted functions, include app_internal.h)
- Modify: `cmake/sources.cmake:11` (add app_routes.c)

- [ ] **Step 1: Create `app_internal.h`**

```c
#ifndef CSILK_APP_INTERNAL_H
#define CSILK_APP_INTERNAL_H

#include "csilk/csilk.h"

#define CSILK_MAX_GROUPS 32

typedef struct {
    char           prefix[256];
    csilk_group_t *group;
} cached_group_t;

struct csilk_app_s {
    csilk_config_t   config;
    csilk_router_t  *router;
    csilk_server_t  *server;
    csilk_group_t   *root_group;
    cached_group_t   groups[CSILK_MAX_GROUPS];
    int              group_count;
};

#endif
```

- [ ] **Step 2: Create `app_routes.c`**

Move these functions from `app.c`:
- `find_or_create_group` (static, line 208)
- `find_matching_group_for_path` (static, line 618)
- `csilk_app_add_route` (line 653)
- `csilk_app_add_route_extended` (line 680)
- `csilk_app_add_route_extended_perm` (line 707)
- `csilk_app_add_route_perm` (line 752)
- `csilk_app_add_handlers` (line 779)
- `csilk_app_use` (line 548)
- `csilk_app_use_group` (line 567)
- `csilk_app_apply_config` (line 587)

Include `app_internal.h` and necessary csilk headers. The `cached_group_t` type and `CSILK_MAX_GROUPS` constant move to `app_internal.h` so both files can use them.

- [ ] **Step 3: Update `app.c`**

- Remove the `struct csilk_app_s` definition (lines 164-184) and `cached_group_t` typedef
- Add `#include "app_internal.h"`
- Remove the functions listed in Step 2
- Keep: `init_app_mutex`, `csilk_app_new`, `csilk_app_free`, `csilk_app_run`, logger functions, OpenAPI functions, static file functions, config functions, accessors, and static globals (`s_openapi_router`, `s_app_mutex`, `s_app_mutex_once`, `g_static[]`, `g_static_n`)

- [ ] **Step 4: Update `cmake/sources.cmake`**

```cmake
set(CSILK_APP_SOURCES
    src/app/app.c
    src/app/app_routes.c
    src/app/group.c
)
```

- [ ] **Step 5: Build and verify**

```bash
cd build && cmake .. && make -j$(nproc) 2>&1 | grep -E "error|warning" | head -20
```

Expected: Zero errors.

- [ ] **Step 6: Run tests**

```bash
cd build && ctest --output-on-failure 2>&1 | tail -5
```

Expected: All tests pass.

- [ ] **Step 7: Format and commit**

```bash
cd /home/quintin/Data/source/c_cpp/server-c
make -C build format
git add src/app/app_internal.h src/app/app_routes.c src/app/app.c cmake/sources.cmake
git commit -m "refactor(app): ♻️ extract route registration to app_routes.c"
```

---

### Task 5: Split `wf_ai.c` -> `wf_ai_utils.c` + `wf_ai_nodes.c` + `wf_ai_agents.c`

**Files:**
- Create: `src/workflow/wf_ai_internal.h`, `src/workflow/wf_ai_utils.c`, `src/workflow/wf_ai_nodes.c`, `src/workflow/wf_ai_agents.c`
- Delete: `src/workflow/wf_ai.c`
- Modify: `cmake/sources.cmake:136` (replace wf_ai.c)

- [ ] **Step 1: Create `wf_ai_internal.h`**

```c
#ifndef CSILK_WF_AI_INTERNAL_H
#define CSILK_WF_AI_INTERNAL_H

#include "csilk/csilk.h"
#include "csilk/app/workflow.h"

csilk_data_t* ai_node_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input,
                              void* user_data);

#endif
```

- [ ] **Step 2: Create `wf_ai_utils.c`**

Move these functions from `wf_ai.c`:
- `csilk_wf_alloc` (line 26)
- `csilk_wf_strdup` (line 48)
- `csilk_wf_data_new` (line 73)
- `_csilk_json_get_path` (static, line 101)
- `csilk_wf_ctx_set_memory` (line 847)
- `csilk_wf_ctx_get_memory` (line 873)
- `csilk_agent_memory_new` (line 1031)
- `csilk_agent_memory_store` (line 1052)
- `csilk_agent_memory_recall` (line 1087)
- `csilk_agent_memory_free` (line 1113)
- `struct csilk_agent_memory_s` definition (line 1023)

- [ ] **Step 3: Create `wf_ai_nodes.c`**

Move these functions from `wf_ai.c`:
- `apply_filter` (static, line 163)
- `resolve_templates` (static, line 227)
- `sub_tool_work_t` struct (line 400)
- `stream_ctx_t` struct (line 443)
- `sub_worker_cb` (static, line 413)
- `after_sub_worker_cb` (static, line 433)
- `on_ai_stream` (static, line 457)
- `ai_config_free` (static, line 472)
- `ai_node_handler` (line 512) -- **remove `static` keyword**
- `vector_search_config_free` (static, line 490)
- `vector_search_node_handler` (static, line 725)
- `csilk_wf_add_ai` (line 800)
- `csilk_wf_add_vector_search` (line 826)

Include `wf_ai_internal.h` is NOT needed here (this file defines `ai_node_handler`, it doesn't call it).

- [ ] **Step 4: Create `wf_ai_agents.c`**

Move these functions from `wf_ai.c`:
- `agent_react_config_free` (static, line 895)
- `agent_react_node_handler` (static, line 905)
- `csilk_wf_add_agent_react` (line 926)
- `agent_reflexion_config_free` (static, line 945)
- `agent_reflexion_node_handler` (static, line 954)
- `csilk_wf_add_agent_reflexion` (line 1002)
- `agent_hitl_config_free` (static, line 1180)
- `agent_hitl_node_handler` (static, line 1189)
- `csilk_wf_add_agent_hitl` (line 1223)
- `worker_config_t` struct (line 1134)
- `worker_config_free` (static, line 1141)
- `agent_worker_node_handler` (static, line 1149)
- `csilk_wf_add_agent_worker` (line 1159)
- `csilk_agent_publish_task` (line 1126)

Include `wf_ai_internal.h` for `ai_node_handler` declaration.

- [ ] **Step 5: Delete `wf_ai.c`**

```bash
git rm src/workflow/wf_ai.c
```

- [ ] **Step 6: Update `cmake/sources.cmake`**

Replace `src/workflow/wf_ai.c` with:
```cmake
    src/workflow/wf_ai_utils.c
    src/workflow/wf_ai_nodes.c
    src/workflow/wf_ai_agents.c
```

- [ ] **Step 7: Build and verify**

```bash
cd build && cmake .. && make -j$(nproc) 2>&1 | grep -E "error|warning" | head -20
```

Expected: Zero errors. If `ai_node_handler` signature mismatch, fix `wf_ai_internal.h`.

- [ ] **Step 8: Run tests**

```bash
cd build && ctest --output-on-failure 2>&1 | tail -5
```

Expected: All tests pass.

- [ ] **Step 9: Format and commit**

```bash
cd /home/quintin/Data/source/c_cpp/server-c
make -C build format
git add src/workflow/wf_ai_internal.h src/workflow/wf_ai_utils.c src/workflow/wf_ai_nodes.c src/workflow/wf_ai_agents.c cmake/sources.cmake
git commit -m "refactor(workflow): ♻️ split wf_ai.c into utils, nodes, and agents"
```

---

### Task 6: Split `server.c` -> `server.c` + `server_shutdown.c` + `server_workers.c`

**Files:**
- Create: `src/core/server/server_shutdown.c`, `src/core/server/server_workers.c`
- Modify: `src/core/server/server.c` (remove extracted functions)
- Modify: `src/core/internal/srv_impl.h` (add declarations)
- Modify: `cmake/sources.cmake:60` (add new files)

- [ ] **Step 1: Add declarations to `srv_impl.h`**

Add to `src/core/internal/srv_impl.h`:

```c
/* --- Server shutdown (server_shutdown.c) --- */
CSILK_INTERNAL void _csilk_server_on_signal(uv_signal_t* handle, int signum);
CSILK_INTERNAL int  _csilk_server_close_active_clients(csilk_server_t* server, csilk_io_loop_t* loop);
CSILK_INTERNAL void _csilk_server_on_stop_async(uv_async_t* handle);
CSILK_INTERNAL void _csilk_server_on_worker_stop_async(uv_async_t* handle);

/* --- Server workers (server_workers.c) --- */
CSILK_INTERNAL void _csilk_server_worker_thread(void* arg);
CSILK_INTERNAL void _csilk_server_pin_thread_to_core(int core_id);
CSILK_INTERNAL int  _csilk_server_bind_and_listen(csilk_io_loop_t* loop, uv_tcp_t* out_handle, int port, int backlog, int reuseport);
```

- [ ] **Step 2: Create `server_shutdown.c`**

Move these functions from `server.c`, renaming them to match the `srv_impl.h` declarations:
- `on_signal` (static, line 51) -> `_csilk_server_on_signal`
- `close_active_clients` (static, line 72) -> `_csilk_server_close_active_clients`
- `on_stop_async` (static, line 126) -> `_csilk_server_on_stop_async`
- `on_server_handle_close` (static, line 350) -> keep static or remove if unused elsewhere
- `on_worker_stop_async` (static, line 635) -> `_csilk_server_on_worker_stop_async`

Update all internal references to use the new names.

- [ ] **Step 3: Create `server_workers.c`**

Move these functions from `server.c`:
- `worker_thread` (static, line 762) -> `_csilk_server_worker_thread`
- `pin_thread_to_core` (static, line 741) -> `_csilk_server_pin_thread_to_core`
- `bind_and_listen` (static, line 840) -> `_csilk_server_bind_and_listen`
- `csilk_dispatch` (line 702)
- `on_dispatch_async` (static, line 675)
- `_csilk_worker_init_dispatch` (static, line 694)
- `worker_data_t` struct (line 612)
- `worker_stop_data_t` struct (line 618)

- [ ] **Step 4: Update `server.c`**

Remove the functions listed above. Update `csilk_server_run` to call `_csilk_server_on_stop_async` instead of `on_stop_async`. Update the async handle callback registration to use the new function names.

- [ ] **Step 5: Update `cmake/sources.cmake`**

In the `else()` branch (non-io_uring):
```cmake
    list(APPEND CSILK_CORE_SOURCES
        src/core/server/server.c
        src/core/server/server_shutdown.c
        src/core/server/server_workers.c
        src/core/server/connection.c
    )
```

- [ ] **Step 6: Build and verify**

```bash
cd build && cmake .. && make -j$(nproc) 2>&1 | grep -E "error|warning" | head -20
```

Expected: Zero errors. Fix any callback type mismatches.

- [ ] **Step 7: Run tests**

```bash
cd build && ctest --output-on-failure 2>&1 | tail -5
```

Expected: All tests pass.

- [ ] **Step 8: Format and commit**

```bash
cd /home/quintin/Data/source/c_cpp/server-c
make -C build format
git add src/core/server/server_shutdown.c src/core/server/server_workers.c src/core/server/server.c src/core/internal/srv_impl.h cmake/sources.cmake
git commit -m "refactor(server): ♻️ extract shutdown and worker logic from server.c"
```

---

### Task 7: Split `uring_server.c` -> `uring_server.c` + `uring_loop.c` + `uring_dispatch.c`

**Files:**
- Create: `src/core/uring/uring_loop.c`, `src/core/uring/uring_dispatch.c`
- Modify: `src/core/uring/uring_server.c` (remove extracted functions)
- Modify: `src/core/internal/srv_impl.h` (add uring declarations if needed)
- Modify: `cmake/sources.cmake:52` (add new files)

- [ ] **Step 1: Create `uring_dispatch.c`**

Move these functions from `uring_server.c`:
- `csilk_dispatch` (line 521)
- `on_dispatch_async` (static, line 494)
- `_csilk_worker_init_dispatch` (static, line 513)

- [ ] **Step 2: Create `uring_loop.c`**

Move these functions from `uring_server.c`:
- `worker_thread` (static, line 547)
- `bind_and_listen` (static, line 757)
- `on_stop_async` (static, line 111)
- `on_signal` (static, line 79)
- `on_worker_stop_async` (static, line 474)
- `openapi_json_handler` (static, line 811)
- `csilk_barrier_t` struct (line 40)
- `barrier_init` (static, line 48)
- `barrier_wait` (static, line 56)
- `barrier_destroy` (static, line 70)
- `worker_data_t` struct (line 460)
- `worker_stop_data_t` struct (line 466)

- [ ] **Step 3: Update `uring_server.c`**

Remove the functions listed above. Keep:
- `csilk_server_new` (line 150)
- `csilk_server_free` (line 294)
- `csilk_server_stop` (line 354)
- `csilk_server_run` (line 823)
- `csilk_server_set_config` (line 388)
- All setter/getter functions
- `spa_fallback_handler` (static, line 205)
- `close_active_clients` (static, line 87)

- [ ] **Step 4: Update `cmake/sources.cmake`**

In the `if(CSILK_USE_URING)` branch:
```cmake
    list(APPEND CSILK_CORE_SOURCES
        src/core/uring/uring_server.c
        src/core/uring/uring_loop.c
        src/core/uring/uring_dispatch.c
        src/core/uring/uring_connection.c
        src/core/uring/uring_thread_pool.c
        src/core/uring/uring_fs.c
        src/core/uring/uv_stubs.c
    )
```

- [ ] **Step 5: Build with io_uring and verify**

```bash
cd build && cmake .. -DCSILK_USE_URING=ON && make -j$(nproc) 2>&1 | grep -E "error|warning" | head -20
```

Expected: Zero errors.

- [ ] **Step 6: Run tests**

```bash
cd build && ctest --output-on-failure 2>&1 | tail -5
```

Expected: All tests pass.

- [ ] **Step 7: Rebuild with libuv (default) and verify**

```bash
cd build && cmake .. -DCSILK_USE_URING=OFF && make -j$(nproc) 2>&1 | grep -E "error|warning" | head -20
cd build && ctest --output-on-failure 2>&1 | tail -5
```

Expected: Both backends compile and pass tests.

- [ ] **Step 8: Format and commit**

```bash
cd /home/quintin/Data/source/c_cpp/server-c
make -C build format
git add src/core/uring/uring_loop.c src/core/uring/uring_dispatch.c src/core/uring/uring_server.c cmake/sources.cmake
git commit -m "refactor(uring): ♻️ extract event loop and dispatch from uring_server.c"
```

---

### Task 8: Final Verification

**Files:** None (verification only)

- [ ] **Step 1: Full rebuild from clean**

```bash
cd build && rm -rf * && cmake .. && make -j$(nproc) 2>&1 | tail -5
```

Expected: Clean build succeeds.

- [ ] **Step 2: Run all tests**

```bash
cd build && ctest --output-on-failure 2>&1 | tail -10
```

Expected: All tests pass. Same count as baseline.

- [ ] **Step 3: Run clang-format check**

```bash
cd build && make check-format 2>&1 | tail -5
```

Expected: All files pass format check.

- [ ] **Step 4: Run clang-tidy**

```bash
cd build && make tidy 2>&1 | tail -5
```

Expected: No new warnings.

- [ ] **Step 5: Verify io_uring backend**

```bash
cd build && cmake .. -DCSILK_USE_URING=ON && make -j$(nproc) 2>&1 | tail -5
cd build && ctest --output-on-failure 2>&1 | tail -5
```

Expected: io_uring backend compiles and passes all tests.

- [ ] **Step 6: Verify file sizes**

```bash
wc -l src/core/ctx/context.c src/core/ctx/ctx_accessors.c \
      src/core/http/http1_parse.c src/core/http/http1_response.c \
      src/app/app.c src/app/app_routes.c \
      src/workflow/wf_ai_utils.c src/workflow/wf_ai_nodes.c src/workflow/wf_ai_agents.c \
      src/core/server/server.c src/core/server/server_shutdown.c src/core/server/server_workers.c \
      src/core/uring/uring_server.c src/core/uring/uring_loop.c src/core/uring/uring_dispatch.c
```

Expected: No file exceeds 700 lines.

- [ ] **Step 7: Verify no public header changes**

```bash
git diff HEAD~6 -- include/
```

Expected: Empty diff. Zero changes to public headers.
