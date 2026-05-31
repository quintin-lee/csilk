# Context Design

The `csilk_ctx_t` (request context) is the central object in csilk. It carries the request state, response buffer, handler chain, WebSocket callbacks, and an Arena allocator across the entire request lifecycle.

## Context Structure

```mermaid
graph TB
    subgraph "csilk_ctx_t"
        HW["handler_index + handlers[]\n(Middleware chain state)"]
        AB["aborted (chain termination flag)"]
        JB["jump_buffer\n(setjmp/longjmp seat)"]

        subgraph Request["csilk_request_t"]
            RM["method (GET/POST/etc)"]
            RP["path (/api/v1/users)"]
            RB["body + body_len"]
            RH["headers (hash map, 64 buckets)"]
            RQ["query_params (hash map)"]
        end

        subgraph Response["csilk_response_t"]
            RS["status (HTTP code)"]
            RBODY["body + body_len"]
            RH2["headers (hash map)"]
        end

        subgraph Params["Path Params"]
            P1["params[0..19]"]
        end

        subgraph Storage["User KV Storage"]
            ST["storage_head (linked list)"]
        end

        AR["arena (bump allocator)"]
        WS["is_websocket + on_ws_message"]
        SE["is_sse"]
        AS["is_async + response_started"]
    end
```

## Context Lifecycle

```mermaid
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
graph TB
    subgraph "Header Hash Map (16 buckets)"
        B0["bucket[0]"] --> H1["Key: host\nValue: localhost:8080"]
        H1 --> H2["Key: content-type\nValue: application/json"]
        B1["bucket[1]"]
        B2["bucket[2]"]
        B3["bucket[3]"] --> H3["Key: authorization\nValue: Bearer xyz"]
        B4["bucket[4..15]"]
    end

    subgraph "Lookup 'Content-Type'"
        IN["csilk_get_header(ctx, 'Content-Type')"]
        IN --> HASH["djb2_hash('content-type') % 16 → bucket 0"]
        HASH --> SCAN["strcasecmp walk list"]
        SCAN --> MATCH["Found: 'application/json'"]
    end
```

## Response Generation Flow

```mermaid
flowchart TB
    subgraph "Sync Response"
        SYNC_H["Handler calls csilk_string/json/redirect"] --> SYNC_SET["ctx->response.status = 200\nctx->response.body = 'OK'"]
        SYNC_SET --> SYNC_RET["Handler returns without calling csilk_next()"]
        SYNC_RET --> SYNC_SEND["_csilk_send_response(ctx)\nSerializes HTTP + headers + body\nuv_write() to socket"]
    end

    subgraph "Async / Streaming Response"
        ASYNC_H["Handler sets ctx->is_async = 1"] --> ASYNC_RET["Returns control to libuv"]
        ASYNC_RET --> ASYNC_WAIT["Later: csilk_response_write()"]
        ASYNC_WAIT --> ASYNC_SEND["send_chunked_headers() (first call)\nwrite_chunk_frame() (per chunk)\ncsilk_response_end() (terminal chunk)"]
    end

    subgraph "WebSocket Response"
        WS_H["Handler calls csilk_ws_handshake()"] --> WS_101["Status 101 Switching Protocols\nctx->is_websocket = 1"]
        WS_101 --> WS_READ["uv_read_start() continues\nBut parser switches to csilk_ws_parse_frame()"]
    end
```

## Context Cleanup

### Regular Cleanup

Between requests (keep-alive), `csilk_ctx_cleanup()` efficiently resets state:

1. `csilk_arena_reset()` - O(1) pointer reset; all per-request allocations freed
2. `free()` path parameters (keys/values)
3. `free()` request body
4. `free()` request path
5. `memset()` header/query/response maps to zero
6. Reset all flags: `aborted`, `is_websocket`, `is_sse`, `is_async`, `response_started`
7. Reset `handler_index = -1`, `storage_head = NULL`

### Deferred Cleanup (Panic-Safe)

The deferred cleanup API (`csilk_ctx_defer` / `csilk_ctx_defer_free`) protects against resource leaks across `setjmp`/`longjmp` boundaries. When a handler panics via `csilk_panic`, stack unwinding is skipped via `longjmp`, so heap allocations, open file descriptors, and mutex locks held by the handler would normally leak. The deferred cleanup list is processed in LIFO order before arena reset, ensuring all registered cleanup callbacks are invoked even on panic paths:

```c
char* buf = malloc(1024);
csilk_ctx_defer(c, free, buf);       // free(buf) called on cleanup or panic
csilk_ctx_defer(c, close, &fd);      // close(fd) called on cleanup or panic
csilk_ctx_defer(c, uv_mutex_unlock, &mutex);  // unlock called on cleanup or panic
```

Items are arena-allocated and auto-freed on arena reset. Callbacks are invoked automatically by `csilk_ctx_cleanup` and by the panic recovery path.
