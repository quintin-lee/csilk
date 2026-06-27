# Custom Middleware Development

Middleware in csilk is a function with signature `void handler(csilk_ctx_t* c)`. Middleware can intercept requests before and after the actual business handler executes. Middleware **MUST** call `csilk_next(c)` to pass control to the next handler — omitting this **MUST** be intentional (short-circuit). Middleware **MUST NOT** block — any blocking I/O **MUST** use the libuv thread pool. Middleware **SHOULD NOT** allocate on the hot path; arena allocation **SHOULD** be preferred.

## Middleware Pattern

```mermaid
%%{init: {'theme': 'base', 'themeVariables': {'background': '#2E3440','primaryColor':'#81A1C1','primaryBorderColor':'#4C566A','primaryTextColor':'#ECEFF4','secondaryColor':'#3B4252','secondaryBorderColor':'#434C5E','secondaryTextColor':'#D8DEE9','lineColor':'#81A1C1','textColor':'#ECEFF4','mainBkg':'#3B4252','nodeBorder':'#4C566A','clusterBkg':'#2E3440','clusterBorder':'#4C566A','titleColor':'#ECEFF4','edgeLabelBackground':'#3B4252','nodeTextColor':'#ECEFF4'}, 'flowchart': {'htmlLabels': true, 'curve': 'basis'}}}%%
flowchart TB
    START["fa:fa-play Request enters middleware"]
    PRE["fa:fa-cog Pre-processing logic<br/>(e.g., validate token, check rate limit)"]

    DEC{"fa:fa-question-circle Condition met?"}

    NEXT["fa:fa-arrow-right csilk_next(ctx)<br/>-&gt; Delegate to next handler"]

    POST["fa:fa-refresh Post-processing logic<br/>(e.g., log latency, add headers)"]

    ABORT_VALID["fa:fa-times-circle Fail gracefully<br/>csilk_json_error()<br/>csilk_abort()"]
    ABORT_PANIC["fa:fa-exclamation-triangle Fail hard<br/>csilk_panic(ctx)<br/>-&gt; Caught by Recovery"]

    DONE["fa:fa-check-circle Done"]

    START --> PRE
    PRE --> DEC
    DEC -->|Yes| NEXT
    DEC -->|Recoverable| ABORT_VALID
    DEC -->|Unrecoverable| ABORT_PANIC
    NEXT --> POST
    POST --> DONE
    ABORT_VALID --> DONE
    ABORT_PANIC --> DONE
```

## Basic Middleware Structure

```c
void my_middleware(csilk_ctx_t* c) {
    // === PRE-PROCESSING ===
    // Access request data
    const char* token = csilk_get_header(c, "Authorization");
    const char* ip = csilk_get_client_ip(c);

    // Validate / check conditions
    if (!token) {
        csilk_json_error(c, 401, "Missing token");
        return;  // Abort chain (don't call csilk_next)
    }

    // Store data for later middleware to use
    csilk_set(c, "user_id", (void*)42);

    // === DELEGATE ===
    csilk_next(c);

    // === POST-PROCESSING ===
    // Only runs if csilk_next returns normally
    // (i.e., no abort and no panic)
    CSILK_LOG_I("Request completed");
}
```

## Common Middleware Recipes

### 1. Authentication Middleware

```c
void auth_middleware(csilk_ctx_t* c) {
    const char* auth = csilk_get_header(c, "Authorization");
    if (!auth || strncmp(auth, "Bearer ", 7) != 0) {
        csilk_json_error(c, 401, "Unauthorized");
        return;
    }

    const char* token = auth + 7;
    if (!validate_token(token)) {
        csilk_json_error(c, 403, "Invalid token");
        return;
    }

    csilk_next(c);
    // Post: audit logging
}
```

### 2. Timing/Logging Middleware

```c
void timing_middleware(csilk_ctx_t* c) {
    uint64_t start = uv_hrtime();

    csilk_next(c);

    uint64_t elapsed = (uv_hrtime() - start) / 1000000;
    CSILK_LOG_I("%s %s → %d (%llums)",
        csilk_get_method(c),
        csilk_get_path(c),
        csilk_get_status(c),
        elapsed);
}
```

### 3. Response Header Injection

```c
void security_headers_middleware(csilk_ctx_t* c) {
    csilk_next(c);

    // Add security headers after handler runs
    csilk_set_header(c, "X-Content-Type-Options", "nosniff");
    csilk_set_header(c, "X-Frame-Options", "DENY");
    csilk_set_header(c, "X-XSS-Protection", "1; mode=block");
}
```

### 4. JWT Authorization Example

```c
void protected_resource_handler(csilk_ctx_t* c) {
    // Built-in JWT middleware already verified the token and stored payload
    cJSON* payload = (cJSON*)csilk_get(c, "jwt_payload");
    if (payload) {
        const char* user = cJSON_GetObjectItem(payload, "sub")->valuestring;
        CSILK_LOG_I("Access by user: %s", user);
    }
    csilk_string(c, 200, "Protected data");
}

// In main:
// csilk_group_use(api, (csilk_handler_t)csilk_jwt_middleware, "secret");
```

### 5. Request Tracing with Request ID

```c
void trace_middleware(csilk_ctx_t* c) {
    const char* req_id = csilk_get_request_id(c);
    
    // Add request ID to all logs via context-aware logging (future feature)
    // Or just manually:
    CSILK_LOG_I("[%s] Started request", req_id);
    
    csilk_next(c);
}
```

## Registration Methods

```mermaid
%%{init: {'theme': 'base', 'themeVariables': {'background': '#2E3440','primaryColor':'#81A1C1','primaryBorderColor':'#4C566A','primaryTextColor':'#ECEFF4','secondaryColor':'#3B4252','secondaryBorderColor':'#434C5E','secondaryTextColor':'#D8DEE9','lineColor':'#81A1C1','textColor':'#ECEFF4','mainBkg':'#3B4252','nodeBorder':'#4C566A','clusterBkg':'#2E3440','clusterBorder':'#4C566A','titleColor':'#ECEFF4','edgeLabelBackground':'#3B4252','nodeTextColor':'#ECEFF4'}, 'flowchart': {'htmlLabels': true, 'curve': 'basis'}}}%%
flowchart TB
    subgraph Global_MW["fa:fa-globe Global Middleware"]
        G["csilk_server_use(server, handler)<br/>-&gt; Applied to ALL routes<br/>-&gt; Max 32 global middlewares"]
    end

    subgraph Group_MW["fa:fa-folder Group Middleware"]
        GRP["csilk_group_use(group, handler)<br/>-&gt; Applied to routes in group<br/>-&gt; Inherits parent group middlewares"]
    end

    subgraph Route_H["fa:fa-code Route Handlers"]
        R["csilk_router_add(r, method, path, [h1, h2, NULL], 2)<br/>-&gt; Handlers specific to one route"]
    end

    subgraph Exec_Order["fa:fa-sort-amount-asc Execution Order"]
        G --> GRP
        GRP --> R
        Note["Combined chain:<br/>global[0] -&gt; global[1] -&gt; group[0] -&gt; route[0] -&gt; route[1]"]
    end
```

## Middleware Chain Assembly

```mermaid
%%{init: {'theme': 'base', 'themeVariables': {'background': '#2E3440','primaryColor':'#81A1C1','primaryBorderColor':'#4C566A','primaryTextColor':'#ECEFF4','secondaryColor':'#3B4252','secondaryBorderColor':'#434C5E','secondaryTextColor':'#D8DEE9','lineColor':'#81A1C1','textColor':'#ECEFF4','mainBkg':'#3B4252','nodeBorder':'#4C566A','clusterBkg':'#2E3440','clusterBorder':'#4C566A','titleColor':'#ECEFF4','edgeLabelBackground':'#3B4252','nodeTextColor':'#ECEFF4','actorBorder':'#81A1C1','actorBkg':'#3B4252','actorTextColor':'#ECEFF4','signalColor':'#81A1C1','signalTextColor':'#D8DEE9','noteBkgColor':'#434C5E','noteTextColor':'#D8DEE9','loopTextColor':'#81A1C1','sequenceNumberColor':'#ECEFF4'}, 'sequence': {'actorFontSize': 14, 'noteFontSize': 12, 'messageFontSize': 12, 'mirrorActors': false}}}%%
sequenceDiagram
    participant S as fa:fa-server csilk_server_t
    participant G as fa:fa-folder csilk_group_t
    participant R as fa:fa-sitemap csilk_router_t
    participant C as fa:fa-cube csilk_ctx_t

    Note over S: csilk_server_use(server, recovery)
    S->>S: middlewares[0] = recovery
    S->>S: middleware_count = 1

    Note over S: csilk_server_use(server, logger)
    S->>S: middlewares[1] = logger
    S->>S: middleware_count = 2

    Note over G: csilk_group_use(api, auth)
    G->>G: api-&gt;middlewares[0] = auth
    G->>G: api-&gt;middleware_count = 1

    Note over R: csilk_group_add_route(api, GET, "/ping", handler)

    Note over C: At on_message_complete():
    C->>R: csilk_router_match_ctx() -&gt; route_handlers = [auth, handler]
    C->>S: Prepend global middlewares -&gt; [recovery, logger, auth, handler]
    C->>C: csilk_next() starts at index 0
```

## Best Practices

1. **Always call `csilk_next(c)`** in middleware that wants to continue the chain. Skipping it terminates the chain and sends the response.

2. **Use `csilk_abort(c)`** to terminate the chain early without setting a response body (useful with `csilk_redirect` which already set the response).

3. **Store data with `csilk_set(c, key, value)`** to pass data between middlewares and handlers. Data is stored on the context's linked list storage.

4. **Don't call `csilk_next()` in post-processing logic** -- only call it once in your middleware. The call stack handles the return path automatically.

5. **Use the Recovery middleware as the FIRST global middleware** to ensure any `csilk_panic()` calls in downstream handlers are caught.

6. **Use Arena-allocated memory** for temporary data within handlers. Memory allocated on the Arena is automatically freed at the end of the request cycle.

## Error Handling Flow

```mermaid
%%{init: {'theme': 'base', 'themeVariables': {'background': '#2E3440','primaryColor':'#81A1C1','primaryBorderColor':'#4C566A','primaryTextColor':'#ECEFF4','secondaryColor':'#3B4252','secondaryBorderColor':'#434C5E','secondaryTextColor':'#D8DEE9','lineColor':'#81A1C1','textColor':'#ECEFF4','mainBkg':'#3B4252','nodeBorder':'#4C566A','clusterBkg':'#2E3440','clusterBorder':'#4C566A','titleColor':'#ECEFF4','edgeLabelBackground':'#3B4252','nodeTextColor':'#ECEFF4'}, 'flowchart': {'htmlLabels': true, 'curve': 'basis'}}}%%
flowchart TB
    REQ["fa:fa-play Request"]

    REC["fa:fa-medkit Recovery Middleware<br/>(setjmp here)"]

    AUTH["fa:fa-key Auth Middleware"]

    HAND["fa:fa-code Business Handler"]

    REQ --> REC
    REC --> AUTH

    AUTH -- "Valid" --> HAND
    AUTH -- "No token" --> ERR1["fa:fa-times-circle csilk_json_error(c, 401)<br/>return (skip csilk_next)"]
    AUTH -- "Invalid token" --> ERR2["fa:fa-bomb csilk_panic(c, 'bad token')"]

    HAND -- "Normal" --> OK["fa:fa-check-circle csilk_string(c, 200, 'OK')"]
    HAND -- "Crash" --> ERR3["fa:fa-bomb csilk_panic(c, 'db error')"]

    ERR2 --> REC_L["fa:fa-arrow-right longjmp -&gt; Recovery"]
    ERR3 --> REC_L
    REC_L --> RECOVER["csilk_string(c, 500)\ncsilk_abort(c)"]
```

---

## Further Reading

For deep-dive architectural details of the middleware system and related components:

| Topic | Module Design Document |
|-------|----------------------|
| Middleware Onion Model & Chain Assembly | [Middleware](../module-design/middleware.md) |
| JWT / CSRF / CORS / WAF / Rate Limiter | [Security](../module-design/security.md) |
| Context Lifecycle & Arena Allocator | [Context](../module-design/context.md) |
| Server Hooks | [Hooks](../module-design/hooks.md) |
