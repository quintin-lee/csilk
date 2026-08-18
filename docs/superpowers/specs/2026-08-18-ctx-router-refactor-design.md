# ctx/router 架构解耦与优化设计

**日期：** 2026-08-18  
**状态：** 草案  
**范围：** `src/core/ctx/`、`src/core/primitives/router*.c`

---

## 问题陈述

csilk 的 ctx（请求上下文）和 router（路由匹配）是 HTTP 请求处理的核心组件。当前存在以下架构问题：

1. **Router-Ctx 紧耦合**：`router_trie.c` 直接读写 `ctx->params[]`、`ctx->arena` 等内部字段，导致路由模块无法独立测试，且无 ctx 场景（OpenAPI 生成、基准测试）只能走备用 API
2. **参数内存管理不一致**：有 arena 时 borrow trie segment，无 arena 时 malloc；backtrack 时需要手动 rollback；cleanup 路径需分支处理
3. **Cleanup 路径复杂**：`csilk_ctx_cleanup()` 有 10 个步骤，部分步骤对大多数请求是 no-op 但仍需执行
4. **Matching 性能有优化空间**：静态子节点线性扫描，平均 2-3 个子节点的场景仍可优化

---

## 设计方案

### 一、Router-Ctx 解耦

#### 核心思路

引入独立的 `csilk_route_result_t` 结构体，路由匹配结果不再直接写入 ctx，而是先返回给调用方。ctx 的填充由框架层通过 `csilk_ctx_apply_route_result()` 完成。

#### 新增类型

```c
/**
 * @brief Path parameter captured during route matching.
 */
typedef struct {
    const char* key;   /**< Parameter name (e.g., "id"). Arena-owned or trie-borrowed. */
    const char* value; /**< Parameter value. Arena-allocated copy of the matched segment. */
} csilk_route_param_t;

/**
 * @brief Result of a route match — independent of csilk_ctx_t.
 *
 * This struct encapsulates everything the router produces during matching:
 * the handler chain, the number of handlers, the matched method handler
 * (for metadata access), and captured path parameters.
 *
 * By returning this struct instead of writing directly into ctx, the router
 * becomes testable without a full context, and the ctx population becomes
 * a separate step with clear ownership semantics.
 */
typedef struct {
    csilk_handler_t*         handlers;        /**< NULL-terminated handler chain. */
    size_t                   handler_count;   /**< Number of handlers. */
    csilk_method_handler_t*  mh;              /**< Matched method handler (for metadata). */
    csilk_route_param_t      params[CSILK_MAX_PARAMS]; /**< Captured path params. */
    int                      params_count;    /**< Number of captured params. */
    int                      matched;         /**< 1 if a route matched, 0 otherwise. */
} csilk_route_result_t;
```

#### 新增 API

```c
/**
 * @brief Match a method+path against the router trie, returning a result struct.
 *
 * Unlike csilk_router_match_ctx(), this function does NOT modify any ctx state.
 * It is safe to call from any thread and does not require a csilk_ctx_t.
 *
 * @param r  Router instance.
 * @param method HTTP method string.
 * @param path Decoded URL path.
 * @return A csilk_route_result_t with matched=1 if a route was found.
 */
csilk_route_result_t csilk_router_match_result(const csilk_router_t* r,
                                                const char* method,
                                                const char* path);

/**
 * @brief Apply a route match result to a request context.
 *
 * Populates ctx->params[], ctx->handlers, ctx->handler_count, ctx->handler_index,
 * and ctx->current_handler from the result. All parameters are allocated from
 * the ctx's arena.
 *
 * @param c     Request context.
 * @param result The match result from csilk_router_match_result().
 */
void csilk_ctx_apply_route_result(csilk_ctx_t* c, const csilk_route_result_t* result);
```

#### 现有 API 兼容层

```c
/**
 * @brief Match a request context's method+path against the router.
 *
 * Thin wrapper around csilk_router_match_result() + csilk_ctx_apply_route_result().
 * Preserves the existing API contract.
 */
int csilk_router_match_ctx(csilk_router_t* r, csilk_ctx_t* c);
```

实现：

```c
int csilk_router_match_ctx(csilk_router_t* r, csilk_ctx_t* c)
{
    if (!r || !c || !r->root || !c->request.method || !c->request.path) {
        CSILK_LOG_W("Invalid match parameters: router=%p, ctx=%p", (void*)r, (void*)c);
        return 0;
    }
    c->params_count = 0;
    
    csilk_route_result_t result = csilk_router_match_result(r, c->request.method, c->request.path);
    if (result.matched) {
        csilk_ctx_apply_route_result(c, &result);
        CSILK_LOG_D("Route matched: %s %s (pattern: %s)",
                    c->request.method, c->request.path,
                    result.mh ? (result.mh->path ? result.mh->path : "?") : "?");
        return 1;
    }
    CSILK_LOG_D("Route not matched: %s %s", c->request.method, c->request.path);
    return 0;
}
```

#### 影响范围

| 文件 | 变更 |
|------|------|
| `include/csilk/core/router.h` | 新增 `csilk_route_result_t`、`csilk_router_match_result()`、`csilk_ctx_apply_route_result()` |
| `src/core/primitives/router_internal.h` | 更新 `match_node()` 签名，移除 ctx 参数 |
| `src/core/primitives/router_trie.c` | 重构 `match_node()` 返回 result 而非直接写 ctx |
| `src/core/primitives/router.c` | 新增 `csilk_router_match_result()`、`csilk_ctx_apply_route_result()`、更新 `csilk_router_match_ctx()` |
| `src/core/ctx/ctx_internal.h` | 移除 params 相关内部依赖（可选） |

---

### 二、参数内存管理统一

#### 当前问题

```c
// router_trie.c — 当前不一致的分配策略
if (ctx->arena) {
    ctx->params[ctx->params_count].key   = (char*)child->segment;  // borrow trie segment
    ctx->params[ctx->params_count].value = csilk_arena_strndup(ctx->arena, seg, len);
} else {
    ctx->params[ctx->params_count].key   = strdup(child->segment);
    ctx->params[ctx->params_count].value = malloc(len + 1);
}
// Backtrack 时需要 rollback：
ctx->params_count--;
free(ctx->params[ctx->params_count].key);
free(ctx->params[ctx->params_count].value);
```

#### 统一方案：始终通过 apply_result 分配

路由匹配阶段只保存 `(key, value)` 字符串引用，不分配内存。`csilk_ctx_apply_route_result()` 统一从 arena 分配：

```c
void csilk_ctx_apply_route_result(csilk_ctx_t* c, const csilk_route_result_t* result)
{
    if (!c || !result) return;
    
    c->params_count = 0;
    for (int i = 0; i < result->params_count && i < CSILK_MAX_PARAMS; i++) {
        if (!result->params[i].key || !result->params[i].value) continue;
        
        // Always arena-allocate — no malloc/free branches in cleanup
        c->params[i].key = csilk_arena_strdup(c->arena, result->params[i].key);
        c->params[i].value = csilk_arena_strndup(c->arena, result->params[i].value,
                                                  strlen(result->params[i].value));
        if (c->params[i].key && c->params[i].value) {
            c->params_count++;
        }
    }
    
    c->handlers = result->handlers;
    c->handler_count = result->handler_count;
    c->handler_index = -1;
    c->current_handler = result->mh;
}
```

#### Router 内部的临时参数

`csilk_route_result_t.params[].key` 可以是：
- **Trie segment 引用**（const char*，不需要 free）—— 当 key 来自静态注册的 segment 名
- **Arena 拷贝**（如果需要修改）

`csilk_route_result_t.params[].value` 始终是：
- **原始 segment 引用**（const char* + len）—— 在 match_node 中记录指针和长度
- 不在 router 内部分配，由 apply_result 统一从 ctx arena 复制

#### Backtrack 简化

由于参数现在存储在 `csilk_route_result_t` 中而非 ctx 中，backtrack 只需丢弃临时结果，无需手动 rollback：

```c
csilk_handler_t* match_node(csilk_router_node_t* node, const char* method,
                             const char* path, csilk_route_result_t* result)
{
    // Terminal check...
    
    // For param matching, use a temporary result to track potential captures
    csilk_route_result_t temp = *result;  // shallow copy
    temp.matched = 0;
    temp.params_count = 0;
    
    // Try param child...
    if (node->param_child) {
        temp.params[temp.params_count].key = child->segment;
        temp.params[temp.params_count].value = seg;
        temp.params_count++;
        
        csilk_handler_t* r = match_node(child, method, p, &temp);
        if (r) {
            *result = temp;  // commit on success
            return r;
        }
        // No need to rollback — temp goes out of scope
    }
    
    return NULL;
}
```

#### Cleanup 简化

参数现在全部来自 arena，无需在 cleanup 中单独处理：

```c
// 原 cleanup 步骤 4（params）可以移除
// csilk_arena_reset() 自动 reclaim 所有 arena 分配的参数
if (c->arena) {
    csilk_arena_reset(c->arena);
}
c->params_count = 0;  // 仅重置计数
```

---

### 三、Cleanup 路径优化

#### 优化点总结

1. **body_is_managed 检查前置**：只对真正分配的 body 执行 free，避免对零拷贝 body 误操作
2. **arena reset 顺序调整**：在 header map 清零之前执行，确保 arena 上的 storage items、defer items、query/form params 都能被正确 reclaim
3. **read_buffers 循环清理**：保持现有逻辑，但明确注释 pool return vs free 的分支条件

#### 重写后的 csilk_ctx_cleanup()

```c
void csilk_ctx_cleanup(csilk_ctx_t* c)
{
    if (!c) return;
    
    /* 1. Deferred callbacks (LIFO) — may release heap memory / fds. */
    csilk_ctx_defer_free(c);
    
    /* 2. Storage destructors + driver clear — BEFORE arena reset. */
    if (c->storage_head) {
        csilk_storage_item_t* item = c->storage_head;
        while (item) {
            if (item->free_fn && item->value) {
                item->free_fn(item->value);
                item->value = NULL;
            }
            item = item->next;
        }
        if (c->storage_driver && c->storage_driver->clear) {
            c->storage_driver->clear(c);
        }
        c->storage_head = NULL;
    }
    
    /* 3. Zero-copy file response — close fd if open. */
    if (c->file_fd >= 0) {
        csilk_io_fs_close(NULL, &close_req, c->file_fd, NULL);
        c->file_fd = -1;
    }
    
    /* 4. Request body — only free when managed (heap-allocated or H2-realloc'd). */
    if (c->request.body_is_managed && c->request.body) {
        if (c->request.body_capacity > 0) {
            csilk_body_free((void*)c->request.body, c->request.body_capacity);
        } else {
            free((void*)c->request.body);
        }
        c->request.body = NULL;
        c->request.body_len = 0;
        c->request.body_capacity = 0;
        c->request.body_is_managed = 0;
        c->request.body_ownership = CSILK_OWN_BORROWED;
    }
    
    /* 5. Response body — same logic. */
    if (c->response.body_is_managed && c->response.body) {
        if (c->response.body_capacity > 0) {
            csilk_body_free((void*)c->response.body, c->response.body_capacity);
        } else {
            free((void*)c->response.body);
        }
        c->response.body = NULL;
        c->response.body_len = 0;
        c->response.body_capacity = 0;
        c->response.body_is_managed = 0;
        c->response.body_ownership = CSILK_OWN_BORROWED;
        c->response.status = 0;
    }
    
    /* 6. Path — always freed (always malloc'd by split_url). */
    free(c->request.path);
    c->request.path = NULL;
    
    /* 7. Read buffers — return to worker pool or free. */
    for (int i = 0; i < c->read_buffers_count; i++) {
        char* b = c->read_buffers[i];
        if (!b) continue;
        size_t sz = c->read_buf_sizes[i];
        if (sz > 0 && c->server && c->server->worker_pools) {
            worker_pool_t* wp = ((csilk_client_t*)c->_internal_client)->owner_pool;
            pool_put_read_buf(wp, b, sz);
        } else {
            free(b);
        }
        c->read_buffers[i] = NULL;
    }
    c->read_buffers_count = 0;
    
    /* 8. Arena reset — O(1) reclaim all request-scoped allocations. */
    if (c->arena) {
        csilk_arena_reset(c->arena);
    }
    c->params_count = 0;
    
    /* 9. Header maps — clear ONLY used maps (via `used` flag). */
    if (c->request.headers.used)       memset(&c->request.headers,       0, sizeof(c->request.headers));
    if (c->request.query_params.used)  memset(&c->request.query_params,  0, sizeof(c->request.query_params));
    if (c->request.form_params.used)   memset(&c->request.form_params,   0, sizeof(c->request.form_params));
    if (c->response.headers.used)      memset(&c->response.headers,      0, sizeof(c->response.headers));
    
    /* 10. Handler chain state. */
    c->handler_index = -1;
    c->handlers = NULL;
    c->handler_count = 0;
    c->current_handler = NULL;
    
    /* 11. Flow control state. */
    c->aborted = 0;
    c->panicked = 0;
    c->is_websocket = 0;
    c->is_sse = 0;
    c->is_async = 0;
    c->response_started = 0;
    c->write_paused = 0;
    c->on_drain = NULL;
    c->on_drain_data = NULL;
    c->on_ws_message = NULL;
    
    /* 12. Request ID — clear only if set. */
    if (c->request_id[0]) {
        memset(c->request_id, 0, sizeof(c->request_id));
    }
}
```

---

### 四、Matching 性能（后续优化项）

#### 当前性能基线

- AVX2：~50ns per lookup (P99 ≤ 100ns, 100K routes)
- NEON：~80ns per lookup
- 典型 REST API 路径深度：3-7 层

#### 潜在优化方向

**方向 A：首字符快速索引**

为每个节点维护基于首字符的子节点索引，将 STATIC 子节点的线性扫描降为 O(1) 查找：

```c
struct csilk_router_node_s {
    // ... existing fields ...
    
    /* Fast first-char lookup for STATIC children.
     * sparse_children[seg[0]] points to the first STATIC child starting with that char.
     * Followed by children chain for same-char collisions. */
    csilk_router_node_t* static_first_child_by_char[256];
    
    csilk_router_node_t* param_child;     /**< At most one PARAM child per node. */
    csilk_router_node_t* wildcard_child;  /**< At most one WILDCARD child per node. */
};
```

**收益评估：**
- 预期改善 10-20% 的匹配时间
- 增加 route registration 复杂度（需要维护 char 索引）
- 对浅层 trie（depth < 5）收益有限

**建议：** 此优化作为后续性能调优项，不在本次重构范围内。

---

## 实施计划

### Phase 1：Router-Ctx 解耦（核心变更）

1. 在 `router.h` 中定义 `csilk_route_result_t`
2. 更新 `match_node()` 签名，移除 ctx 参数，返回 result
3. 实现 `csilk_router_match_result()` 和 `csilk_ctx_apply_route_result()`
4. 更新 `csilk_router_match_ctx()` 为薄包装
5. 更新所有测试用例

### Phase 2：参数内存统一

1. 修改 `match_node()` 使用临时 result 进行 backtrack
2. 实现 `csilk_ctx_apply_route_result()` 的统一 arena 分配逻辑
3. 简化 `csilk_ctx_cleanup()` 中的 params 处理

### Phase 3：Cleanup 优化

1. 整理 `csilk_ctx_cleanup()` 步骤顺序
2. 添加 `body_is_managed` 前置检查
3. 添加注释说明每步的清理语义

### Phase 4：验证

1. 运行所有单元测试
2. 运行集成测试
3. ASAN/TSAN 验证
4. 性能基准测试对比

---

## 风险评估

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| `csilk_router_match_ctx()` API 兼容 | 低 | 保持签名不变，内部转发到新 API |
| Backtrack 语义变化 | 中 | 充分测试 param/wildcard 边界情况 |
| Arena 分配失败 | 低 | apply_result 中检查 null，skip param |
| 性能回归 | 低 | result struct 是栈上分配，无额外 heap 开销 |

---

## 未覆盖范围

- **Header Map 优化**：当前 `used` 标志已足够，暂不重构
- **首字符索引匹配**：作为后续性能调优项
- **多线程 ctx 共享**：当前设计假设 ctx 单线程访问，保持
