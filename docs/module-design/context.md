# Context Design

The `csilk_ctx_t` (request context) is the central object in csilk. It carries the request state, response buffer, handler chain, WebSocket callbacks, and an Arena allocator across the entire request lifecycle. All HTTP headers use **zero-copy** `csilk_str_view_t` references into raw recv buffers — no `malloc` per header. Context is reset between keep-alive requests in O(1) via arena pointer bump. Users **MUST NOT** hold `csilk_str_view_t` pointers across requests; data **SHOULD** be persisted via `csilk_arena_strdup()` if needed beyond the current request lifecycle.

## Context Structure

```mermaid
%%{init: {
  'theme': 'base',
  'themeVariables': {
    'background': '#2E3440',
    'primaryColor': '#81A1C1',
    'primaryBorderColor': '#4C566A',
    'primaryTextColor': '#ECEFF4',
    'secondaryColor': '#3B4252',
    'secondaryBorderColor': '#434C5E',
    'secondaryTextColor': '#D8DEE9',
    'lineColor': '#81A1C1',
    'textColor': '#ECEFF4',
    'mainBkg': '#3B4252',
    'nodeBorder': '#4C566A',
    'clusterBkg': '#2E3440',
    'clusterBorder': '#4C566A',
    'titleColor': '#ECEFF4',
    'edgeLabelBackground': '#3B4252',
    'nodeTextColor': '#ECEFF4'
  },
  'flowchart': {'htmlLabels': true, 'curve': 'basis'}
}}%%
graph TB
    subgraph ctx_t["fa:fa-exchange-alt csilk_ctx_t"]
        HW["fa:fa-list handler_index + handlers[]<br/>(Middleware chain state)"]
        AB["fa:fa-ban aborted (chain termination flag)"]
        JB["fa:fa-bookmark jump_buffer<br/>(setjmp/longjmp seat)"]

        subgraph Request["fa:fa-arrow-right csilk_request_t"]
            RM["fa:fa-tag method (GET/POST/etc)"]
            RP["fa:fa-link path (/api/v1/users)"]
            RB["fa:fa-file body + body_len"]
            RH["fa:fa-table headers (hash map, 64 buckets)"]
            RQ["fa:fa-search query_params (hash map)"]
        end

        subgraph Response["fa:fa-reply csilk_response_t"]
            RS["fa:fa-hashtag status (HTTP code)"]
            RBODY["fa:fa-file body + body_len"]
            RH2["fa:fa-table headers (hash map)"]
        end

        subgraph Params["fa:fa-tags Path Params"]
            P1["fa:fa-list params[0..19]"]
        end

        subgraph Storage["fa:fa-database User KV Storage"]
            ST["fa:fa-link storage_head (linked list)"]
        end

        AR["fa:fa-memory arena (bump allocator)"]
        WS["fa:fa-plug is_websocket + on_ws_message"]
        SE["fa:fa-broadcast-tower is_sse"]
        AS["fa:fa-bolt is_async + response_started"]
    end
```

## Context Lifecycle

```mermaid
%%{init: {
  'theme': 'base',
  'themeVariables': {
    'background': '#2E3440',
    'primaryColor': '#81A1C1',
    'primaryBorderColor': '#4C566A',
    'primaryTextColor': '#ECEFF4',
    'secondaryColor': '#3B4252',
    'secondaryBorderColor': '#434C5E',
    'secondaryTextColor': '#D8DEE9',
    'lineColor': '#81A1C1',
    'textColor': '#ECEFF4',
    'mainBkg': '#3B4252',
    'nodeBorder': '#4C566A',
    'clusterBkg': '#2E3440',
    'clusterBorder': '#4C566A',
    'titleColor': '#ECEFF4',
    'edgeLabelBackground': '#3B4252',
    'nodeTextColor': '#ECEFF4'
  }
}}%%
stateDiagram-v2
    [*] --> Pooled: pool_get() acquires client

    state Pooled {
        [*] --> Created: uv_tcp_init() + uv_accept()
    }

    state Created {
        [*] --> Init: csilk_arena_new()
        [*] --> Init: parser.data = client
        Init --> Parsing: uv_read_start()

        state Parsing {
            [*] --> Headers: llhttp callbacks
            Headers --> Body: on_body() called
            Body --> Complete: on_message_complete()
        }
    }

    Parsing --> Matched: csilk_router_match_ctx()
    Matched --> Chaining: Global middleware prepended
    Chaining --> Executing: csilk_next(ctx)

    state Executing {
        [*] --> handler1: Handler 1 (e.g. Recovery)
        handler1 --> handler2: csilk_next()
        handler2 --> handler3: csilk_next()
        handler3 --> handlerN: csilk_next()
        handlerN --> [*]
        --
        [*] --> Panic: longjmp (csilk_panic)
        Panic --> [*]: Returns to Recovery
    }

    Executing --> Sending: Response body set OR aborted
    Sending --> Cleanup: _csilk_send_response()

    state Cleanup {
        [*] --> Reset: csilk_arena_reset()
        Reset --> ClearParams: Free params
        ClearParams --> ClearBody: Free body
        ClearBody --> ResetFlags: Reset flags
        ResetFlags --> [*]
    }

    Cleanup --> KeepAlive: Connection: keep-alive
    KeepAlive --> Parsing: Wait for next request

    Cleanup --> Closing: Connection: close
    Closing --> Freed: csilk_arena_free() + pool_put(client)
    Freed --> [*]
```

### Multi-Thread Safety

In multi-worker mode (configured via `worker_threads > 1`), csilk uses a **per-worker lock-free connection object pool** (`pool_get`/`pool_put`) that avoids mutex contention. Each worker thread manages its own pool, eliminating the data race previously present in the shared-mutex pool design.

## Handler Chain Execution

The handler chain uses a simple index-based iterator pattern:

```mermaid
%%{init: {
  'theme': 'base',
  'themeVariables': {
    'background': '#2E3440',
    'primaryColor': '#81A1C1',
    'primaryBorderColor': '#4C566A',
    'primaryTextColor': '#ECEFF4',
    'secondaryColor': '#3B4252',
    'secondaryBorderColor': '#434C5E',
    'secondaryTextColor': '#D8DEE9',
    'lineColor': '#81A1C1',
    'textColor': '#ECEFF4',
    'mainBkg': '#3B4252',
    'nodeBorder': '#4C566A',
    'clusterBkg': '#2E3440',
    'clusterBorder': '#4C566A',
    'titleColor': '#ECEFF4',
    'edgeLabelBackground': '#3B4252',
    'nodeTextColor': '#ECEFF4'
  }
}}%%
sequenceDiagram
    participant S as csilk_next()
    participant C as csilk_ctx_t
    participant H0 as Handler[0] (Recovery)
    participant H1 as Handler[1] (Auth)
    participant H2 as Handler[2] (Business)

    Note over C: handler_index = -1

    S->>C: handler_index++ → 0
    S->>H0: handlers[0](ctx)

    H0->>H0: setjmp(jump_buffer)
    H0->>S: csilk_next(ctx)

    S->>C: handler_index++ → 1
    S->>H1: handlers[1](ctx)

    H1->>H1: Check auth token
    H1->>S: csilk_next(ctx)

    S->>C: handler_index++ → 2
    S->>H2: handlers[2](ctx)

    H2->>H2: Process business logic
    H2->>C: csilk_string(ctx, 200, "OK")

    Note over C: Response set, no more csilk_next()

    Note over S: Return path: H2 → H1 → H0
    Note over H1: Run post-auth logic (e.g., audit)
    Note over H0: Run post-recovery logic (e.g., timing)
```

## Header Hash Map

Headers use a case-insensitive DJB2 hash map with 16 fixed buckets and chaining:

```mermaid
%%{init: {
  'theme': 'base',
  'themeVariables': {
    'background': '#2E3440',
    'primaryColor': '#81A1C1',
    'primaryBorderColor': '#4C566A',
    'primaryTextColor': '#ECEFF4',
    'secondaryColor': '#3B4252',
    'secondaryBorderColor': '#434C5E',
    'secondaryTextColor': '#D8DEE9',
    'lineColor': '#81A1C1',
    'textColor': '#ECEFF4',
    'mainBkg': '#3B4252',
    'nodeBorder': '#4C566A',
    'clusterBkg': '#2E3440',
    'clusterBorder': '#4C566A',
    'titleColor': '#ECEFF4',
    'edgeLabelBackground': '#3B4252',
    'nodeTextColor': '#ECEFF4'
  },
  'flowchart': {'htmlLabels': true, 'curve': 'basis'}
}}%%
graph TB
    subgraph header_map["fa:fa-table Header Hash Map (16 buckets)"]
        B0["fa:fa-folder bucket[0]"] --> H1["fa:fa-tag Key: host<br/>Value: localhost:8080"]
        H1 --> H2["fa:fa-tag Key: content-type<br/>Value: application/json"]
        B1["fa:fa-folder bucket[1]"]
        B2["fa:fa-folder bucket[2]"]
        B3["fa:fa-folder bucket[3]"] --> H3["fa:fa-tag Key: authorization<br/>Value: Bearer xyz"]
        B4["fa:fa-ellipsis-h bucket[4..15]"]
    end

    subgraph lookup["fa:fa-search Lookup 'Content-Type'"]
        IN["fa:fa-play csilk_get_header(ctx, 'Content-Type')"]
        IN --> HASH["fa:fa-cog djb2_hash('content-type') % 16 → bucket 0"]
        HASH --> SCAN["fa:fa-list strcasecmp walk list"]
        SCAN --> MATCH["fa:fa-check Found: 'application/json'"]
    end
```

## Zero-Copy String Views vs Owned Accessors

csilk provides dual accessors for maximum performance and explicit ownership semantics:

```c
typedef struct {
    const char* data; /**< Pointer to buffer (borrowed from parser/network buffer). */
    size_t      len;  /**< Byte length (not guaranteed to be NUL-terminated). */
} csilk_view_t;
```

| Scope | Zero-Copy Borrowed View | Owned / NUL-Terminated C-String |
|-------|-------------------------|--------------------------------|
| **Query Param** | `csilk_get_query_view(c, "key")` | `csilk_get_query(c, "key")` |
| **Path Param**  | `csilk_get_param_view(c, "id")`  | `csilk_get_param(c, "id")`  |
| **Header**      | `csilk_get_header_view(c, "Host")` | `csilk_get_header(c, "Host")` |
| **Body**        | `csilk_get_body_view(c)` | `csilk_get_body(c)` |

> [!IMPORTANT]
> `csilk_view_t` directly references raw network receive buffers with zero copying and zero allocations. It is valid only for the duration of the current request handler. If you require a NUL-terminated string or persistent copy, use the standard `csilk_get_*()` functions or duplicate via `csilk_arena_strndup()`.

## Custom Storage Destructors (RAII)

For heap-allocated resources stored in `csilk_ctx_t` (such as cJSON objects, database handles, or third-party structures), `csilk_set_ex()` registers an automatic cleanup callback invoked during request cleanup:

```c
typedef void (*csilk_destructor_t)(void* value);

// Store heap object with automatic destructor
cJSON* payload = jwt_verify_internal(...);
csilk_set_ex(c, "jwt_payload", payload, (csilk_destructor_t)csilk_json_free);
```

## Outbound Streaming & Backpressure Flow Control

Streaming writes (`csilk_response_write()`, `csilk_sse_send()`, `csilk_ws_send()`) enforce connection-level outbound queue watermarks:

```c
// Configure connection watermarks (defaults: 64KB high, 16KB low, 16MB max)
csilk_set_write_watermarks(c, 128 * 1024, 32 * 1024, 32 * 1024 * 1024);

// Write data with backpressure detection
int status = csilk_response_write(c, data, len);
if (status == 0) {
    // High watermark exceeded — pause producer output
    csilk_on_drain(c, on_stream_drain, producer_state);
} else if (status < 0) {
    // Buffer overflow (exceeded max_write_buffer_size) or socket error
}
```

## Response Generation Flow

```mermaid
%%{init: {
  'theme': 'base',
  'themeVariables': {
    'background': '#2E3440',
    'primaryColor': '#81A1C1',
    'primaryBorderColor': '#4C566A',
    'primaryTextColor': '#ECEFF4',
    'secondaryColor': '#3B4252',
    'secondaryBorderColor': '#434C5E',
    'secondaryTextColor': '#D8DEE9',
    'lineColor': '#81A1C1',
    'textColor': '#ECEFF4',
    'mainBkg': '#3B4252',
    'nodeBorder': '#4C566A',
    'clusterBkg': '#2E3440',
    'clusterBorder': '#4C566A',
    'titleColor': '#ECEFF4',
    'edgeLabelBackground': '#3B4252',
    'nodeTextColor': '#ECEFF4'
  },
  'flowchart': {'htmlLabels': true, 'curve': 'basis'}
}}%%
flowchart TB
    subgraph sync_resp["fa:fa-bolt Sync Response"]
        SYNC_H["fa:fa-play Handler calls csilk_string/json/redirect"] --> SYNC_SET["fa:fa-cog ctx->response.status = 200\nctx->response.body = 'OK'"]
        SYNC_SET --> SYNC_RET["fa:fa-undo Handler returns without calling csilk_next()"]
        SYNC_RET --> SYNC_SEND["fa:fa-reply _csilk_send_response(ctx)\nSerializes HTTP + headers + body\ncsilk_io_write() to socket"]
    end

    subgraph async_resp["fa:fa-cloud Async / Streaming Response"]
        ASYNC_H["fa:fa-play Handler sets ctx->is_async = 1"] --> ASYNC_RET["fa:fa-undo Returns control to event loop"]
        ASYNC_RET --> ASYNC_WAIT["fa:fa-hourglass Later: csilk_response_write()"]
        ASYNC_WAIT --> ASYNC_SEND["fa:fa-file send_chunked_headers() (first call)\nwrite_chunk_frame() (per chunk)\ncsilk_response_end() (terminal chunk)"]
    end

    subgraph ws_resp["fa:fa-plug WebSocket Response"]
        WS_H["fa:fa-play Handler calls csilk_ws_handshake()"] --> WS_101["fa:fa-random Status 101 Switching Protocols\nctx->is_websocket = 1"]
        WS_101 --> WS_READ["fa:fa-sync-alt csilk_io_read_start() continues\nBut parser switches to csilk_ws_parse_frame()"]
    end
```

## Context Cleanup

### Regular Cleanup

Between requests (keep-alive), `csilk_ctx_cleanup()` efficiently resets state:

1. **Invoke Custom Destructors** - Run custom `csilk_destructor_t` on stored objects (`csilk_set_ex`) and deferred cleanup items (`csilk_ctx_defer`).
2. **Free registered read buffers** - All dynamic raw network read buffers accumulated during request parsing are freed.
3. `csilk_arena_reset()` - O(1) pointer reset; all per-request allocations are recycled.
4. `free()` path parameters (keys/values).
5. `free()` request body (if it was copied/allocated, otherwise it was zero-copy referenced and freed in step 2).
6. `free()` request path.
7. `memset()` header/query/response maps to zero.
8. Reset all flags: `aborted`, `is_websocket`, `is_sse`, `is_async`, `response_started`.
9. Reset `handler_index = -1`, `storage_head = NULL`, and `read_buffers_count = 0`.

### Deferred Cleanup (Panic-Safe)

The deferred cleanup API (`csilk_ctx_defer` / `csilk_ctx_defer_free`) protects against resource leaks across `setjmp`/`longjmp` boundaries. When a handler panics via `csilk_panic`, stack unwinding is skipped via `longjmp`, so heap allocations, open file descriptors, and mutex locks held by the handler would normally leak. The deferred cleanup list is processed in LIFO order before arena reset, ensuring all registered cleanup callbacks are invoked even on panic paths:

```c
char* buf = malloc(1024);
csilk_ctx_defer(c, free, buf);                 // free(buf) called on cleanup or panic
csilk_ctx_defer(c, (void(*)(void*))close, &fd);// close(fd) called on cleanup or panic
csilk_ctx_defer(c, (void(*)(void*))csilk_mutex_unlock, &mutex); // unlock called on cleanup or panic
```

