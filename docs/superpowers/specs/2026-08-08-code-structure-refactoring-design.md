# Spec: Code Structure Refactoring (Phase 1)

**Date**: 2026-08-08
**Scope**: Split 6 large `.c` files (>800 lines) into smaller, single-responsibility units
**Constraint**: ABI compatible -- zero changes to `include/csilk/` public headers

## Goals

- Reduce the largest files from 800-1239 lines to under 700 lines each
- Each new file has a clear, single responsibility
- All existing tests pass without modification
- Zero new warnings from compilation or clang-tidy

## Non-Goals

- Changing any public API function signatures or struct layouts
- Refactoring files under 800 lines
- Introducing new abstractions or design patterns
- Modifying test files

---

## 1. `wf_ai.c` (1239 lines) -> 3 files

The workflow AI engine mixes memory utilities, template resolution, AI node execution, and agent type implementations.

### Split

| New File | Responsibility | Functions | ~Lines |
|----------|---------------|-----------|--------|
| `wf_ai_utils.c` | Memory helpers, JSON path, context memory, agent memory | `csilk_wf_alloc`, `csilk_wf_strdup`, `csilk_wf_data_new`, `_csilk_json_get_path` (static), `csilk_wf_ctx_set_memory`, `csilk_wf_ctx_get_memory`, `csilk_agent_memory_new`, `csilk_agent_memory_store`, `csilk_agent_memory_recall`, `csilk_agent_memory_free` + `struct csilk_agent_memory_s` | ~290 |
| `wf_ai_nodes.c` | Template engine, AI chat handler, vector search handler | `apply_filter` (static), `resolve_templates` (static), `sub_worker_cb` (static), `after_sub_worker_cb` (static), `on_ai_stream` (static), `ai_config_free` (static), `ai_node_handler`, `vector_search_config_free` (static), `vector_search_node_handler` (static), `csilk_wf_add_ai`, `csilk_wf_add_vector_search` + `sub_tool_work_t`, `stream_ctx_t` | ~660 |
| `wf_ai_agents.c` | All agent node types (ReAct, Reflexion, HITL, worker) | `agent_react_config_free` (static), `agent_react_node_handler` (static), `csilk_wf_add_agent_react`, `agent_reflexion_config_free` (static), `agent_reflexion_node_handler` (static), `csilk_wf_add_agent_reflexion`, `agent_hitl_config_free` (static), `agent_hitl_node_handler` (static), `csilk_wf_add_agent_hitl`, `worker_config_free` (static), `agent_worker_node_handler` (static), `csilk_wf_add_agent_worker`, `csilk_agent_publish_task` + `worker_config_t` | ~270 |

### Internal Header Change

`ai_node_handler` is currently `static` but called from `wf_ai_agents.c` (by `agent_react_node_handler`, `agent_reflexion_node_handler`, `agent_hitl_node_handler`). It must become non-static and be declared in a new internal header `src/workflow/wf_ai_internal.h`:

```c
#ifndef CSILK_WF_AI_INTERNAL_H
#define CSILK_WF_AI_INTERNAL_H

#include "csilk/csilk.h"

int ai_node_handler(csilk_ctx_t *ctx, csilk_wf_node_t *node,
                    csilk_wf_exec_ctx_t *exec_ctx);

#endif
```

### Dependency Graph

```
wf_ai_utils.c  (standalone)
     ^
     |
wf_ai_nodes.c  (depends on utils: csilk_wf_alloc, csilk_wf_strdup, csilk_wf_data_new)
     ^
     |
wf_ai_agents.c (depends on nodes: ai_node_handler; depends on utils: csilk_wf_alloc, etc.)
```

---

## 2. `server.c` (1114 lines) -> 3 files

The libuv server core mixes lifecycle, connection/shutdown management, and worker thread infrastructure.

### Split

| New File | Responsibility | Functions | ~Lines |
|----------|---------------|-----------|--------|
| `server.c` (retained) | Server lifecycle + config + accessors | `csilk_server_new`, `csilk_server_set_config`, `csilk_server_run`, `csilk_server_stop`, `csilk_server_free`, `csilk_server_use`, `csilk_server_set_max_connections`, `csilk_server_check_backpressure`, `csilk_server_get_stats`, `csilk_server_set_spa_fallback`, `csilk_server_set_not_found_handler`, `csilk_server_set_storage_driver`, `csilk_server_set_crypto_driver`, `csilk_server_set_cipher_driver`, `csilk_server_set_quic_transport`, `csilk_server_get_mq`, `csilk_server_get_router`, `csilk_server_set_router`, `openapi_json_handler` (static) | ~600 |
| `server_shutdown.c` | Connection drain and graceful shutdown | `on_signal` (static), `close_active_clients` (static), `on_stop_async` (static), `on_server_handle_close` (static), `on_worker_stop_async` (static) | ~170 |
| `server_workers.c` | Worker thread creation, CPU affinity, bind/listen | `worker_thread` (static), `pin_thread_to_core` (static), `bind_and_listen` (static), `csilk_dispatch`, `on_dispatch_async` (static), `_csilk_worker_init_dispatch` (static) + `worker_data_t`, `worker_stop_data_t` | ~280 |

### Internal Header Change

The shutdown functions are called from `csilk_server_run` and `csilk_server_stop` in `server.c`. They must be declared in the existing internal header `src/core/internal/srv_impl.h`:

```c
void _csilk_server_on_signal(int signum);
void _csilk_server_close_active_clients(csilk_server_t *s);
void _csilk_server_on_stop_async(csilk_async_t *handle);
void _csilk_server_on_worker_stop_async(csilk_async_t *handle);
```

The dispatch functions (`csilk_dispatch`, `_csilk_worker_init_dispatch`) are already declared in internal headers since they are called from `server.c`.

### Key Detail

`csilk_server_run` references `on_stop_async` as a callback for the async handle. After the split, it will reference `_csilk_server_on_stop_async` via the internal header. Similarly, `worker_thread` in `server_workers.c` references `on_worker_stop_async`.

---

## 3. `uring_server.c` (1147 lines) -> 3 files

The io_uring backend mirrors `server.c` structure. The split follows the same boundaries.

### Split

| New File | Responsibility | Functions | ~Lines |
|----------|---------------|-----------|--------|
| `uring_server.c` (retained) | Server lifecycle + config + accessors | `csilk_server_new`, `csilk_server_free`, `csilk_server_stop`, `csilk_server_run`, `csilk_server_set_config`, `csilk_server_set_max_connections`, `csilk_server_set_not_found_handler`, `csilk_server_set_spa_fallback`, `csilk_server_use`, all driver setters, `csilk_server_get_stats`, `csilk_server_get_mq`, `csilk_server_get_router`, `csilk_server_set_router`, `spa_fallback_handler` (static), `close_active_clients` (static) | ~620 |
| `uring_loop.c` | io_uring event loop, worker thread, bind/listen, shutdown callbacks | `worker_thread` (static), `bind_and_listen` (static), `on_stop_async` (static), `on_signal` (static), `on_worker_stop_async` (static), `openapi_json_handler` (static) + `csilk_barrier_t`, `barrier_init/wait/destroy` (static) + `worker_data_t`, `worker_stop_data_t` | ~450 |
| `uring_dispatch.c` | Cross-thread request dispatch | `csilk_dispatch`, `on_dispatch_async` (static), `_csilk_worker_init_dispatch` (static) | ~80 |

### Internal Header Change

Same pattern as `server.c`. The `uring_loop.c` shutdown callbacks must be declared in the io_uring internal header. The barrier utility is only used by `uring_loop.c` and stays there.

### Note on CQE Duplication

`worker_thread` and `csilk_server_run` contain nearly identical CQE dispatch switch blocks. This refactoring does NOT deduplicate them -- that is a separate optimization beyond the scope of this spec. The split keeps them in separate files where future deduplication is easier.

---

## 4. `http1.c` (970 lines) -> 2 files

HTTP/1.1 processing splits cleanly into parsing (request side) and response (response side).

### Split

| New File | Responsibility | Functions | ~Lines |
|----------|---------------|-----------|--------|
| `http1_parse.c` | Request parsing: llhttp callbacks, header processing, body accumulation, dispatch | `on_message_begin` (static), `on_url` (static), `on_header_field` (static), `on_header_value` (static), `on_headers_complete` (static), `on_body` (static), `on_message_complete` (static), `finalize_request` (static), `_csilk_persist_header`, `_csilk_dispatch_request` + `buf_grow` (static) | ~500 |
| `http1_response.c` | Response building: status line, headers, serialization, write pipeline, keep-alive | `get_status_text`, `serialize_status_line` (static), `append_custom_headers` (static), `_csilk_send_response`, `on_sendfile_complete` (static), `on_write` (static), `_csilk_handle_post_response`, `csilk_client_write`, `_csilk_send_data`, `_csilk_send_data_owned` | ~470 |

### Internal Header Change

The llhttp parser callbacks in `http1_parse.c` are registered via `llhttp_settings_t` which is initialized in `server.c` / `uring_server.c`. The callback function pointers are already declared in internal headers. The response functions called from `http1_parse.c` (via `_csilk_dispatch_request`) must be declared in the existing internal header.

---

## 5. `app.c` (936 lines) -> 2 files

The app layer splits into lifecycle/bootstrap and route/middleware registration.

### Split

| New File | Responsibility | Functions | ~Lines |
|----------|---------------|-----------|--------|
| `app.c` (retained) | App lifecycle, config, logger, OpenAPI, static files, accessors | `init_app_mutex` (static), `csilk_app_new`, `csilk_app_free`, `csilk_app_run`, logger functions (`csilk_app_log_level`, `csilk_app_log_file`, `csilk_app_log_json`), OpenAPI functions (`get_openapi_router`, `set_openapi_router`, `openapi_handler`, `docs_handler`, `csilk_app_enable_openapi`), static file functions (`contains_path_traversal`, `static_serve`, `csilk_app_static`), config functions, accessors + `s_openapi_router`, `s_app_mutex`, `s_app_mutex_once`, `g_static[]`, `g_static_n` | ~500 |
| `app_routes.c` | Route registration and group management | `find_or_create_group` (static), `find_matching_group_for_path` (static), `csilk_app_add_route`, `csilk_app_add_route_extended`, `csilk_app_add_route_extended_perm`, `csilk_app_add_route_perm`, `csilk_app_add_handlers`, `csilk_app_use`, `csilk_app_use_group`, `csilk_app_apply_config` | ~300 |

### Internal Header Change

`app_routes.c` calls into `app.c` globals (`find_or_create_group` needs the `csilk_app_s` struct definition). The `csilk_app_s` struct is already defined in `app.c` and must be moved to an internal header `src/app/app_internal.h` so both files can access it:

```c
#ifndef CSILK_APP_INTERNAL_H
#define CSILK_APP_INTERNAL_H

#include "csilk/csilk.h"

#define CSILK_MAX_GROUPS  32

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

---

## 6. `context.c` (857 lines) -> 2 files

The request context splits into lifecycle/body handling and accessors/iteration.

### Split

| New File | Responsibility | Functions | ~Lines |
|----------|---------------|-----------|--------|
| `context.c` (retained) | Context lifecycle, body I/O, handler chain control, driver setup | `_csilk_ctx_init`, `csilk_ctx_cleanup`, `tls_large_body_pool_cleanup` (static), `tls_large_body_pool`, `csilk_next`, `csilk_abort`, `csilk_get_body`, `csilk_get_body_len`, `csilk_set_status`, `csilk_get_response_body`, `csilk_set_response_body`, `csilk_set_file_response`, `csilk_get_file_fd`, `csilk_ctx_set_storage_driver`, `csilk_ctx_set_crypto_driver`, `csilk_ctx_set_cipher_driver` | ~430 |
| `ctx_accessors.c` | All getter/setter accessors, iteration, WebSocket callbacks | `csilk_get_method`, `csilk_get_path`, `csilk_get_param`, `csilk_get_params_count`, `csilk_get_param_key`, `csilk_get_param_value`, `csilk_get_header`, `csilk_get_response_header`, `csilk_get_query`, `csilk_set_request_header`, `csilk_get_headers`, `csilk_get_request_id`, `csilk_set_request_id`, `csilk_get_arena`, `csilk_is_websocket`, `csilk_ctx_set_websocket`, `csilk_is_sse`, `csilk_ctx_set_sse`, `_csilk_get_internal_client`, `_csilk_set_internal_client`, `csilk_get_status`, `csilk_ctx_set_async`, `csilk_is_async`, `csilk_ctx_get_server`, `csilk_ctx_get_mq`, `csilk_get_handler_index`, `csilk_get_work_req`, `csilk_is_aborted`, `csilk_ctx_get_handler_path`, `csilk_ctx_get_handler_perm_required`, `csilk_ctx_get_handler_perm_resource`, `for_each_in_map` (static), `csilk_for_each_header`, `csilk_for_each_query`, `csilk_for_each_form_field`, `csilk_set_on_ws_message`, `csilk_set_on_ws_send`, `csilk_get_on_ws_message` | ~430 |

### Internal Header Change

No new internal header needed. All accessor functions are already declared in public headers (`include/csilk/core/context.h`). The `ctx_accessors.c` file only needs the `csilk_ctx_s` struct definition, which is available via the existing internal header.

---

## Build System Changes

### `cmake/sources.cmake`

Add new source files to their respective module variables:

```cmake
# Workflow module
list(APPEND CSILK_WORKFLOW_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/workflow/wf_ai_utils.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/workflow/wf_ai_nodes.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/workflow/wf_ai_agents.c
)
# Remove wf_ai.c from the list (replaced by the 3 new files)

# Core module
list(APPEND CSILK_CORE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/server/server_shutdown.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/server/server_workers.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/uring/uring_loop.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/uring/uring_dispatch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/http/http1_parse.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/http/http1_response.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/app_routes.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/ctx/ctx_accessors.c
)
# Remove the original monolithic files from their respective lists
```

### New Internal Headers

| Header | Location | Purpose |
|--------|----------|---------|
| `wf_ai_internal.h` | `src/workflow/` | Declares `ai_node_handler` for cross-file use |
| `app_internal.h` | `src/app/` | Defines `struct csilk_app_s` for cross-file use |

Existing internal header `src/core/internal/srv_impl.h` gains declarations for shutdown/worker functions.

---

## Verification

After each file split, run in order:

```bash
# 1. Compile -- zero warnings
cd build && cmake .. && make -j$(nproc) 2>&1 | grep -E "warning|error" | head -20

# 2. Format check
make check-format

# 3. All tests pass
make run_tests

# 4. Static analysis
make tidy
```

If any step fails, the split introduced a symbol visibility or linking issue that must be fixed before proceeding to the next file.

## Execution Order

Process files from lowest-risk to highest-risk:

1. `context.c` -- trivial getter/setter extraction, no shared globals
2. `http1.c` -- clean parse/response boundary, no shared globals
3. `app.c` -- requires `app_internal.h` for struct definition
4. `wf_ai.c` -- requires `wf_ai_internal.h` + making `ai_node_handler` non-static
5. `server.c` -- requires internal header updates for shutdown callbacks
6. `uring_server.c` -- same pattern as `server.c`, highest complexity due to io_uring event loop

## Summary

| Original File | Lines | New Files | Target Lines Each |
|---------------|-------|-----------|-------------------|
| `wf_ai.c` | 1239 | `wf_ai_utils.c`, `wf_ai_nodes.c`, `wf_ai_agents.c` | 290, 660, 270 |
| `server.c` | 1114 | `server.c`, `server_shutdown.c`, `server_workers.c` | 600, 170, 280 |
| `uring_server.c` | 1147 | `uring_server.c`, `uring_loop.c`, `uring_dispatch.c` | 620, 450, 80 |
| `http1.c` | 970 | `http1_parse.c`, `http1_response.c` | 500, 470 |
| `app.c` | 936 | `app.c`, `app_routes.c` | 500, 300 |
| `context.c` | 857 | `context.c`, `ctx_accessors.c` | 430, 430 |
| **Total** | **6263** | **15 files** (6 retained + 9 new) | -- |
