# Middleware Design

Middleware in csilk forms a bidirectional chain following the **onion model**. Each middleware can execute logic both on the way "in" (before `csilk_next()`) and on the way "out" (after `csilk_next()` returns).

## Onion Model

```mermaid
flowchart LR
    REQ["HTTP Request"]

    subgraph Onion["Middleware Onion"]
        direction LR
        L1["Layer 1\nRecovery"] --> L2["Layer 2\nLogger\n(request start)"]
        L2 --> L3["Layer 3\nAuth\n(token check)"]
        L3 --> L4["Layer 4\nRate Limit\n(counter check)"]
        L4 --> CORE["Core Handler\n(Business Logic)"]
        CORE --> L4P["Rate Limit\n(... no post logic)"]
        L4P --> L3P["Auth\n(... no post logic)"]
        L3P --> L2P["Logger\n(log latency)"]
        L2P --> L1P["Recovery\n(... no post logic)"]
    end

    L1P --> RES["HTTP Response"]
```

## Handler Chain Assembly

When a route matches, the full handler chain is assembled dynamically:

```mermaid
sequenceDiagram
    participant Server
    participant Router
    participant Arena
    participant CTX as csilk_ctx_t

    Note over Server: on_message_complete()

    Server->>Router: csilk_router_match_ctx(router, ctx)
    Router-->>Server: ctx->handlers = [group_mw, handler, NULL]

    alt Global middlewares exist
        Server->>Server: Count route handlers
        Server->>Server: total = global_count + route_count
        Server->>Arena: csilk_arena_alloc((total+1) * sizeof(handler))
        Server->>CTX: Prepend global_middlewares[0..N] to arena_handlers
        Server->>CTX: Append route handlers after global handlers
        Server->>CTX: arena_handlers[total] = NULL
        Server->>CTX: ctx->handlers = arena_handlers
    end

    Server->>CTX: csilk_next(ctx)
    Note over CTX: Chain starts at index 0
```

## Panic Recovery via setjmp/longjmp

```mermaid
sequenceDiagram
    participant C as csilk_ctx_t
    participant REC as Recovery Middleware
    participant H as Handler (may crash)

    REC->>REC: Initialize jmp_buf
    REC->>REC: jump_point = setjmp(ctx->jump_buffer)

    alt First pass (return 0)
        Note over REC: setjmp returns 0
        REC->>REC: ctx->has_jump_buffer = 1

        alt Chain continues normally
            REC->>C: csilk_next(ctx)
            H->>H: Normal execution
            H->>H: csilk_string(ctx, 200, "OK")
        else Handler panics
            H->>H: csilk_panic(ctx)
            Note over H: This triggers...
            H-->>REC: longjmp(ctx->jump_buffer, 1)
            Note over REC: setjmp returns 1
            REC->>C: csilk_string(ctx, 500, "Internal Server Error")
            REC->>C: csilk_abort(ctx)
        end

    else Second pass (return 1, after longjmp)
        Note over REC: Recovery triggered!
        REC->>REC: Log crash
        REC->>C: csilk_abort(ctx)
    end
```

## Built-in Middleware Catalog

| Middleware | File | Purpose |
|-----------|------|---------|
| **Recovery** | `src/middleware/recovery.c` | Catch crashes via setjmp/longjmp, return 500 |
| **Logger** | `src/middleware/logger.c` | Log method, path, status, latency |
| **Auth** | `src/middleware/auth.c` | Token-based authentication via validator callback |
| **CORS** | `src/middleware/cors.c` | CORS headers and OPTIONS preflight |
| **Rate Limit** | `src/middleware/ratelimit.c` | IP-based sliding window rate limiting |
| **CSRF** | `src/middleware/csrf.c` | Stateless CSRF token generation and validation |
| **Static Files** | `src/middleware/static.c` | Async static file serving via thread pool |
| **Gzip** | `src/middleware/gzip.c` | Response compression via zlib + thread pool |
| **SSE** | `src/middleware/sse.c` | Server-Sent Events initialization/send/close |
| **Multipart** | `src/middleware/multipart.c` | Multipart/form-data parsing with part callback |

## Middleware Execution Order

```mermaid
flowchart TB
    subgraph "Global Middleware (server-level)"
        G1["1. Recovery (setjmp)"]
        G2["2. Logger (start timer)"]
    end

    subgraph "Group Middleware (prefix-level)"
        GM1["3. Auth (validate token)"]
        GM2["4. Rate limit (check quota)"]
    end

    subgraph "Route Handlers"
        H1["5. CORS (set headers)"]
        H2["6. Business Logic Handler"]
    end

    G1 --> G2
    G2 --> GM1
    GM1 --> GM2
    GM2 --> H1
    H1 --> H2

    subgraph "Post-Execution (reverse order)"
        H2 --> H1r["6a. CORS (no post)"]
        H1r --> GM2r["5a. Rate limit (no post)"]
        GM2r --> GM1r["4a. Auth (no post)"]
        GM1r --> G2r["3a. Logger (log latency)"]
        G2r --> G1r["2a. Recovery (cleanup)"]
    end
```
