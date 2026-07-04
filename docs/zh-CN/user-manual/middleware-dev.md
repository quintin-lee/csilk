# 自定义中间件开发

csilk 中的中间件是一个具有签名 `void handler(csilk_ctx_t* c)` 的函数。中间件可以在实际业务处理器执行前后拦截请求。中间件 **MUST** 调用 `csilk_next(c)` 来传递控制权给下一个处理器 — 省略此调用 **MUST** 是有意为之（短路）。中间件 **MUST NOT** 阻塞 — 任何阻塞的 I/O **MUST** 使用 libuv 线程池。中间件 **SHOULD NOT** 在热路径上分配内存；应优先使用 Arena 分配。

## 中间件模式

```mermaid
%%{init: {'theme': 'base', 'themeVariables': {'background': '#2E3440','primaryColor':'#81A1C1','primaryBorderColor':'#4C566A','primaryTextColor':'#ECEFF4','secondaryColor':'#3B4252','secondaryBorderColor':'#434C5E','secondaryTextColor':'#D8DEE9','lineColor':'#81A1C1','textColor':'#ECEFF4','mainBkg':'#3B4252','nodeBorder':'#4C566A','clusterBkg':'#2E3440','clusterBorder':'#4C566A','titleColor':'#ECEFF4','edgeLabelBackground':'#3B4252','nodeTextColor':'#ECEFF4'}, 'flowchart': {'htmlLabels': true, 'curve': 'basis'}}}%%
flowchart TB
    START["fa:fa-play 请求进入中间件"]
    PRE["fa:fa-cog 预处理逻辑<br/>(例如，验证令牌，检查速率限制)"]

    DEC{"fa:fa-question-circle 条件满足?"}

    NEXT["fa:fa-arrow-right csilk_next(ctx)<br/>-&gt; 委托给下一个处理器"]

    POST["fa:fa-refresh 后处理逻辑<br/>(例如，记录延迟，添加头)"]

    ABORT_VALID["fa:fa-times-circle 优雅失败<br/>csilk_json_error()<br/>csilk_abort()"]
    ABORT_PANIC["fa:fa-exclamation-triangle 严重失败<br/>csilk_panic(ctx)<br/>-&gt; 被 Recovery 捕获"]

    DONE["fa:fa-check-circle 完成"]

    START --> PRE
    PRE --> DEC
    DEC -->|是| NEXT
    DEC -->|可恢复| ABORT_VALID
    DEC -->|不可恢复| ABORT_PANIC
    NEXT --> POST
    POST --> DONE
    ABORT_VALID --> DONE
    ABORT_PANIC --> DONE
```

## 基本中间件结构

```c
void my_middleware(csilk_ctx_t* c) {
    // === 预处理 ===
    // 访问请求数据
    const char* token = csilk_get_header(c, "Authorization");
    const char* ip = csilk_get_client_ip(c);

    // 验证 / 检查条件
    if (!token) {
        csilk_json_error(c, 401, "Missing token");
        return;  // 中止链（不调用 csilk_next）
    }

    // 存储供后续中间件使用的数据
    csilk_set(c, "user_id", (void*)42);

    // === 委托 ===
    csilk_next(c);

    // === 后处理 ===
    // 仅在 csilk_next 正常返回时运行
    // (即，没有中止且没有 panic)
    CSILK_LOG_I("请求完成");
}
```

## 常见中间件配方

### 1. 认证中间件

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
    // 后处理: 审计日志记录
}
```

### 2. 计时/日志中间件

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

### 3. 响应头注入

```c
void security_headers_middleware(csilk_ctx_t* c) {
    csilk_next(c);

    // 添加安全头
    csilk_set_header(c, "X-Content-Type-Options", "nosniff");
    csilk_set_header(c, "X-Frame-Options", "DENY");
    csilk_set_header(c, "X-XSS-Protection", "1; mode=block");
}
```

### 4. JWT 授权示例

```c
void protected_resource_handler(csilk_ctx_t* c) {
    // JWT 中间件已将解码后的载荷存储到上下文中
    cJSON* payload = (cJSON*)csilk_get(c, "jwt_payload");
    if (payload) {
        const char* user = cJSON_GetObjectItem(payload, "sub")->valuestring;
        const char* role = cJSON_GetObjectItem(payload, "role")->valuestring;
        CSILK_LOG_I("认证用户: %s (角色: %s)", user, role);
    }
    csilk_string(c, 200, "Protected data");
}

// 在 main 中:
// csilk_group_use(api, (csilk_handler_t)csilk_jwt_middleware, "secret");
```

### 5. 请求追踪与请求 ID

```c
void trace_middleware(csilk_ctx_t* c) {
    const char* req_id = csilk_get_request_id(c);
    
    // 通过上下文感知日志记录添加请求 ID
    CSILK_LOG_I("[%s] 开始请求", req_id);

    csilk_next(c);
}
```

## 注册方法

```mermaid
%%{init: {'theme': 'base', 'themeVariables': {'background': '#2E3440','primaryColor':'#81A1C1','primaryBorderColor':'#4C566A','primaryTextColor':'#ECEFF4','secondaryColor':'#3B4252','secondaryBorderColor':'#434C5E','secondaryTextColor':'#D8DEE9','lineColor':'#81A1C1','textColor':'#ECEFF4','mainBkg':'#3B4252','nodeBorder':'#4C566A','clusterBkg':'#2E3440','clusterBorder':'#4C566A','titleColor':'#ECEFF4','edgeLabelBackground':'#3B4252','nodeTextColor':'#ECEFF4'}, 'flowchart': {'htmlLabels': true, 'curve': 'basis'}}}%%
flowchart TB
    subgraph Global_MW["fa:fa-globe 全局中间件"]
        G["csilk_server_use(server, handler)<br/>-&gt; 应用于所有路由<br/>-&gt; 最多 32 个全局中间件"]
    end

    subgraph Group_MW["fa:fa-folder 分组中间件"]
        GRP["csilk_group_use(group, handler)<br/>-&gt; 应用于组内路由<br/>-&gt; 继承父组中间件"]
    end

    subgraph Route_H["fa:fa-code 路由处理器"]
        R["csilk_router_add(r, method, path, [h1, h2, NULL], 2)<br/>-&gt; 特定于单个路由的处理器"]
    end

    subgraph Exec_Order["fa:fa-sort-amount-asc 执行顺序"]
        G --> GRP
        GRP --> R
        Note["组合链:<br/>global[0] -&gt; global[1] -&gt; group[0] -&gt; route[0] -&gt; route[1]"]
    end
```

## 中间件链组装

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
    G->>G: api->middlewares[0] = auth
    G->>G: api->middleware_count = 1

    Note over R: csilk_group_add_route(api, GET, "/ping", handler)

    Note over C: 在 on_message_complete() 中:
    C->>R: csilk_router_match_ctx() -> route_handlers = [auth, handler]
    C->>S: 前置全局中间件 -> [recovery, logger, auth, handler]
    C->>C: csilk_next() 从索引 0 开始
```

## 最佳实践

1. **始终调用 `csilk_next(c)`** 在希望继续链的中间件中。跳过它会终止链并发送响应。

2. **使用 `csilk_abort(c)`** 早期终止链而不设置响应体（与 `csilk_redirect` 结合使用时有用）。

3. **使用 `csilk_set(c, key, value)` 存储数据** 在中间件和处理器之间传递数据。数据存储在上下文的链表存储中。

4. **不要在后处理逻辑中调用 `csilk_next()`** — 仅调用一次您的中间件。调用堆栈自动处理返回路径。

5. **使用 Recovery 中间件作为第一个全局中间件** 以确保下游处理器中的任何 `csilk_panic()` 调用都被捕获。

6. **使用 Arena 分配的内存** 在处理器内的临时数据。分配在 Arena 上的内存会在请求周期结束时自动释放。

---

## 进一步阅读

有关中间件系统及相关组件的深入架构细节，请参见：

| 主题 | 模块设计文档 |
|-------|----------------------|
| 中间件洋葱模型 & 链组装 | [中间件](../module-design/middleware.md) |
| JWT / CSRF / CORS / WAF / 速率限制器 | [安全](../module-design/security.md) |
| 上下文生命周期 & Arena 分配器 | [上下文](../module-design/context.md) |
| 服务器钩子 | [钩子](../module-design/hooks.md) |