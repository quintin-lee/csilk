# ctx/router 架构解耦实施计划

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 router 从 ctx 内部字段中解耦，统一参数内存管理，简化 cleanup 路径。

**Architecture:** 引入独立的 `csilk_route_result_t` 结构体，路由匹配结果不再直接写入 ctx。Router 变为无状态函数，ctx 填充由框架层统一处理。Cleanup 路径消除 param 分支逻辑。

**Spec:** `docs/superpowers/specs/2026-08-18-ctx-router-refactor-design.md`

---

## Chunk 1: Router-Ctx 解耦核心变更

### Task 1.1: 定义新类型和 API 声明

**Files:**
- Modify: `include/csilk/core/router.h`
- Modify: `src/core/primitives/router_internal.h`

- [ ] **Step 1: 在 router.h 末尾添加新类型定义**

在 `include/csilk/core/router.h` 文件末尾（`csilk_serve_swagger_ui` 声明之后）添加：

```c
/**
 * @brief Path parameter captured during route matching.
 */
typedef struct {
    const char* key;   /**< Parameter name (e.g., "id"). Trie-borrowed, read-only. */
    const char* value; /**< Parameter value segment reference (not copied). */
} csilk_route_param_t;

/**
 * @brief Result of a route match — independent of csilk_ctx_t.
 *
 * Stack-allocated temporary. Its params[].key may reference trie-owned
 * segment strings (lifetime = router lifetime). Caller must NOT hold
 * the result across route mutations.
 */
typedef struct {
    csilk_handler_t*         handlers;        /**< NULL-terminated handler chain. */
    size_t                   handler_count;   /**< Number of handlers. */
    csilk_method_handler_t*  mh;              /**< Matched method handler (for metadata). */
    csilk_route_param_t      params[CSILK_MAX_PARAMS];
    int                      params_count;
    int                      matched;
} csilk_route_result_t;

/**
 * @brief Match a method+path against the router trie, returning a result struct.
 */
csilk_route_result_t csilk_router_match_result(const csilk_router_t* r,
                                                const char* method,
                                                const char* path);

/**
 * @brief Apply a route match result to a request context.
 */
void csilk_ctx_apply_route_result(csilk_ctx_t* c, const csilk_route_result_t* result);
```

- [ ] **Step 2: 更新 router_internal.h 中 match_node 声明**

将 `src/core/primitives/router_internal.h` 中的 `match_node` 声明从：
```c
csilk_handler_t* match_node(csilk_router_node_t*     node,
                            const char*              method,
                            const char*              path,
                            csilk_ctx_t*             ctx,
                            csilk_method_handler_t** out_mh);
```
改为：
```c
csilk_handler_t* match_node(csilk_router_node_t*       node,
                            const char*                method,
                            const char*                path,
                            csilk_route_result_t*      result);
```

- [ ] **Step 3: 编译验证**

```bash
cmake --build build -j$(nproc) 2>&1 | head -30
```
Expected: 编译错误（因为实现还未更新），确认声明变更生效。

- [ ] **Step 4: Commit**

```bash
git add include/csilk/core/router.h src/core/primitives/router_internal.h
git commit -m "feat(router): ✨ add csilk_route_result_t and new API declarations"
```

---

### Task 1.2: 重构 router_trie.c — 移除 ctx 依赖

**Files:**
- Modify: `src/core/primitives/router_trie.c`

- [ ] **Step 1: 重写 match_node 签名和终端匹配逻辑**

将 `match_node()` 函数签名更新，终端匹配时设置 `result->mh`、`result->handlers`、`result->handler_count`、`result->matched`：

```c
csilk_handler_t*
match_node(csilk_router_node_t*     node,
           const char*              method,
           const char*              path,
           csilk_route_result_t*    result)
{
    int use_simd = 0;  /* No ctx available — SIMD config comes from server config */

    if (!path || *path == '\0' || (path[0] == '/' && path[1] == '\0')) {
        csilk_method_handler_t* mh = node->handlers;
        while (mh) {
            if (strcmp(mh->method, method) == 0) {
                result->mh = mh;
                result->handlers = mh->handlers;
                result->handler_count = mh->handler_count;
                result->matched = 1;
                return mh->handlers;
            }
            mh = mh->next;
        }
        return NULL;
    }

    /* ... rest of function, updated below ... */
}
```

- [ ] **Step 2: 更新 try_match_static — 移除 ctx 参数，使用 result**

```c
static csilk_handler_t*
try_match_static(csilk_router_node_t*     child,
                 const char*              method,
                 const char*              seg,
                 size_t                   len,
                 const char*              p,
                 csilk_route_result_t*    result,
                 int                      use_simd)
{
    if (child->segment_len == len && child->segment[0] == seg[0]) {
        int match = 0;
        if (use_simd) {
            match = csilk_memcmp_fast(child->segment, seg, len);
        } else {
            match = (strncmp(child->segment, seg, len) == 0);
        }
        if (match) {
            csilk_handler_t* r = match_node(child, method, p, result);
            if (r) return r;
        }
    }
    return NULL;
}
```

- [ ] **Step 3: 更新 try_match_param — 使用临时 result 进行 backtrack**

```c
static csilk_handler_t*
try_match_param(csilk_router_node_t*     child,
                const char*              method,
                const char*              seg,
                size_t                   len,
                const char*              p,
                csilk_route_result_t*    result)
{
    /* Capture param into temp — no arena allocation here */
    if (result->params_count < CSILK_MAX_PARAMS) {
        result->params[result->params_count].key = child->segment;
        result->params[result->params_count].value = seg;
        result->params_count++;

        csilk_handler_t* r = match_node(child, method, p, result);

        if (!r) {
            result->params_count--;  /* backtrack: discard param */
        }
        return r;
    }
    return NULL;
}
```

- [ ] **Step 4: 更新 try_match_wildcard — 同样使用 result**

```c
static csilk_handler_t*
try_match_wildcard(csilk_router_node_t*     child,
                   const char*              method,
                   const char*              path,
                   csilk_route_result_t*    result)
{
    if (result->params_count < CSILK_MAX_PARAMS) {
        const char* val_start = path;
        while (*val_start == '/') val_start++;

        result->params[result->params_count].key = child->segment;
        result->params[result->params_count].value = val_start;
        result->params_count++;

        /* Wildcard always terminates — find matching method handler */
        csilk_method_handler_t* mh = child->handlers;
        while (mh) {
            if (strcmp(mh->method, method) == 0) {
                result->mh = mh;
                result->handlers = mh->handlers;
                result->handler_count = mh->handler_count;
                result->matched = 1;
                return mh->handlers;
            }
            mh = mh->next;
        }

        result->params_count--;  /* backtrack: method mismatch */
    }
    return NULL;
}
```

- [ ] **Step 5: 更新 match_node 的循环部分**

```c
    csilk_handler_t* result_handler = NULL;
    csilk_route_result_t saved_result;
    memcpy(&saved_result, result, sizeof(csilk_route_result_t));

    for (int i = 0; i < node->children_count; i++) {
        csilk_router_node_t* child = node->children[i];
        if (child->type == CSILK_NODE_STATIC) {
            result_handler = try_match_static(child, method, seg, len, p, result, use_simd);
        } else if (child->type == CSILK_NODE_PARAM) {
            result_handler = try_match_param(child, method, seg, len, p, result);
        } else if (child->type == CSILK_NODE_WILDCARD) {
            result_handler = try_match_wildcard(child, method, path, result);
        }
        if (result_handler) {
            return result_handler;
        }
        /* Backtrack: restore result to state before this child */
        memcpy(result, &saved_result, sizeof(csilk_route_result_t));
    }
    return NULL;
```

> **注意**: `use_simd` 默认 0（无 ctx 时无法获取 server config）。SIMD 加速通过 `csilk_router_match_result()` 传入 ctx 配置的方式在未来优化中处理。当前先保证功能正确。

- [ ] **Step 6: 编译验证 — 应该还有未更新的调用点报错**

```bash
cmake --build build -j$(nproc) 2>&1 | grep "error:" | head -20
```
Expected: 多个 error，指向 router.c 中的 `match_node` 调用点。

- [ ] **Step 7: Commit**

```bash
git add src/core/primitives/router_trie.c
git commit -m "refactor(router): ♻️ decouple match_node from ctx, use csilk_route_result_t"
```

---

### Task 1.3: 更新 router.c — 新增 API + 薄包装

**Files:**
- Modify: `src/core/primitives/router.c`

- [ ] **Step 1: 实现 csilk_router_match_result()**

在 `csilk_router_match()` 函数之前添加：

```c
csilk_route_result_t
csilk_router_match_result(const csilk_router_t* r, const char* method, const char* path)
{
    csilk_route_result_t result;
    memset(&result, 0, sizeof(result));
    if (r && r->root && method && path) {
        match_node(r->root, method, path, &result);
    }
    return result;
}
```

- [ ] **Step 2: 更新 csilk_router_match() 为薄包装**

```c
csilk_handler_t*
csilk_router_match(const csilk_router_t* r, const char* method, const char* path)
{
    if (!r || !r->root || !method || !path) {
        return NULL;
    }
    csilk_route_result_t tmp = csilk_router_match_result(r, method, path);
    return tmp.handlers;
}
```

- [ ] **Step 3: 实现 csilk_ctx_apply_route_result()**

```c
void
csilk_ctx_apply_route_result(csilk_ctx_t* c, const csilk_route_result_t* result)
{
    if (!c || !result) return;

    c->params_count = 0;
    for (int i = 0; i < result->params_count && i < CSILK_MAX_PARAMS; i++) {
        if (!result->params[i].key || !result->params[i].value) continue;

        if (c->arena) {
            c->params[i].key = csilk_arena_strdup(c->arena, result->params[i].key);
            c->params[i].value = csilk_arena_strndup(c->arena, result->params[i].value,
                                                      strlen(result->params[i].value));
        } else {
            c->params[i].key = strdup(result->params[i].key);
            c->params[i].value = strdup(result->params[i].value);
        }

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

- [ ] **Step 4: 更新 csilk_router_match_ctx() 为薄包装**

```c
int
csilk_router_match_ctx(csilk_router_t* r, csilk_ctx_t* c)
{
    if (!r || !c || !r->root || !c->request.method || !c->request.path) {
        CSILK_LOG_W("Invalid match parameters: router=%p, ctx=%p", (void*)r, (void*)c);
        return 0;
    }
    c->params_count = 0;

    csilk_route_result_t result =
        csilk_router_match_result(r, c->request.method, c->request.path);
    if (result.matched) {
        csilk_ctx_apply_route_result(c, &result);
        CSILK_LOG_D("Route successfully matched: %s %s (pattern: %s)",
                    c->request.method, c->request.path,
                    (result.mh && result.mh->path) ? result.mh->path : "unknown");
        return 1;
    }
    CSILK_LOG_D("Route not matched: %s %s", c->request.method, c->request.path);
    return 0;
}
```

- [ ] **Step 5: 编译验证**

```bash
cmake --build build -j$(nproc) 2>&1 | tail -5
```
Expected: 编译成功，无 error。

- [ ] **Step 6: 运行现有测试**

```bash
ctest --test-dir build -R "test_router|test_radix|test_params_limit|test_get_param|test_middleware_chain" --timeout 10 --output-on-failure
```
Expected: 全部 PASS（现有行为应完全一致）。

- [ ] **Step 7: Commit**

```bash
git add src/core/primitives/router.c
git commit -m "feat(router): ✨ add csilk_router_match_result and csilk_ctx_apply_route_result"
```

---

### Task 1.4: 新增测试 — match_result 无 ctx 调用

**Files:**
- Create: `tests/core/test_router_match_result.c`
- Modify: `cmake/tests.cmake`

- [ ] **Step 1: 创建测试文件**

```c
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/core/internal.h"

static void
mock_handler(csilk_ctx_t* c)
{
    (void)c;
}

static void
test_match_result_static()
{
    csilk_router_t* r = csilk_router_new();
    csilk_handler_t h[] = {mock_handler};
    csilk_router_add(r, "GET", "/api/users", h, 1);

    csilk_route_result_t result = csilk_router_match_result(r, "GET", "/api/users");
    assert(result.matched == 1);
    assert(result.handlers != NULL);
    assert(result.handlers[0] == mock_handler);
    assert(result.params_count == 0);
    assert(result.mh != NULL);

    csilk_router_free(r);
    printf("test_match_result_static: PASS\n");
}

static void
test_match_result_param()
{
    csilk_router_t* r = csilk_router_new();
    csilk_handler_t h[] = {mock_handler};
    csilk_router_add(r, "GET", "/api/users/:id", h, 1);

    csilk_route_result_t result = csilk_router_match_result(r, "GET", "/api/users/123");
    assert(result.matched == 1);
    assert(result.params_count == 1);
    assert(strcmp(result.params[0].key, "id") == 0);
    assert(strcmp(result.params[0].value, "123") == 0);

    csilk_router_free(r);
    printf("test_match_result_param: PASS\n");
}

static void
test_match_result_wildcard()
{
    csilk_router_t* r = csilk_router_new();
    csilk_handler_t h[] = {mock_handler};
    csilk_router_add(r, "GET", "/static/*filepath", h, 1);

    csilk_route_result_t result =
        csilk_router_match_result(r, "GET", "/static/css/app.css");
    assert(result.matched == 1);
    assert(result.params_count == 1);
    assert(strcmp(result.params[0].key, "*filepath") == 0);
    assert(strcmp(result.params[0].value, "css/app.css") == 0);

    csilk_router_free(r);
    printf("test_match_result_wildcard: PASS\n");
}

static void
test_match_result_no_match()
{
    csilk_router_t* r = csilk_router_new();
    csilk_handler_t h[] = {mock_handler};
    csilk_router_add(r, "GET", "/api/users", h, 1);

    csilk_route_result_t result = csilk_router_match_result(r, "POST", "/api/users");
    assert(result.matched == 0);
    assert(result.handlers == NULL);
    assert(result.params_count == 0);

    result = csilk_router_match_result(r, "GET", "/api/undefined");
    assert(result.matched == 0);

    csilk_router_free(r);
    printf("test_match_result_no_match: PASS\n");
}

static void
test_match_result_backtrack_no_leak()
{
    /* Param branch fails deeper — params should not leak */
    csilk_router_t* r = csilk_router_new();
    csilk_handler_t h[] = {mock_handler};
    csilk_router_add(r, "GET", "/api/users/:id", h, 1);
    csilk_router_add(r, "GET", "/api/posts/:pid/comments", h, 1);

    /* Match /api/posts/42 — the :id param on users branch should backtrack */
    csilk_route_result_t result = csilk_router_match_result(r, "GET", "/api/posts/42");
    assert(result.matched == 0);
    assert(result.params_count == 0);

    /* Match /api/posts/42/comments — should capture :pid */
    result = csilk_router_match_result(r, "GET", "/api/posts/42/comments");
    assert(result.matched == 1);
    assert(result.params_count == 1);
    assert(strcmp(result.params[0].key, "pid") == 0);
    assert(strcmp(result.params[0].value, "42") == 0);

    csilk_router_free(r);
    printf("test_match_result_backtrack_no_leak: PASS\n");
}

static void
test_apply_result_to_ctx()
{
    csilk_router_t* r = csilk_router_new();
    csilk_handler_t h[] = {mock_handler};
    csilk_router_add(r, "GET", "/api/users/:id/profile", h, 1);

    csilk_ctx_t* c = csilk_test_ctx_new();
    csilk_test_ctx_set_request(c, "GET", "/api/users/42/profile");

    csilk_route_result_t result =
        csilk_router_match_result(r, c->request.method, c->request.path);
    csilk_ctx_apply_route_result(c, &result);

    assert(c->handlers != NULL);
    assert(c->handlers[0] == mock_handler);
    assert(c->handler_count == 1);
    assert(c->handler_index == -1);
    assert(c->params_count == 2);
    assert(strcmp(csilk_get_param(c, "id"), "42") == 0);
    assert(strcmp(csilk_get_param(c, "profile"), "") == 0 ||  /* profile is static, no param */
           csilk_get_param(c, "profile") == NULL);

    /* Re-match should reset params */
    result = csilk_router_match_result(r, "GET", "/api/users/99/settings");
    csilk_ctx_apply_route_result(c, &result);
    assert(c->params_count == 1);
    assert(strcmp(csilk_get_param(c, "id"), "99") == 0);

    csilk_test_ctx_free(c);
    csilk_router_free(r);
    printf("test_apply_result_to_ctx: PASS\n");
}

int
main()
{
    test_match_result_static();
    test_match_result_param();
    test_match_result_wildcard();
    test_match_result_no_match();
    test_match_result_backtrack_no_leak();
    test_apply_result_to_ctx();
    printf("All router_match_result tests passed!\n");
    return 0;
}
```

- [ ] **Step 2: 注册到 cmake**

在 `cmake/tests.cmake` 的 `CSILK_CORE_TESTS` 列表中添加 `test_router_match_result`（在 `test_router` 后面），并在 `CSILK_CORE_TEST_DIRS` 对应位置添加 `core`。

- [ ] **Step 3: 编译并运行新测试**

```bash
cmake --build build -j$(nproc) --target test_router_match_result
./build/test_router_match_result
```
Expected: 所有测试 PASS。

- [ ] **Step 4: Commit**

```bash
git add tests/core/test_router_match_result.c cmake/tests.cmake
git commit -m "test(router): ✅ add tests for csilk_router_match_result and csilk_ctx_apply_route_result"
```

---

### Task 1.5: 清理 csilk_ctx_cleanup — 移除 no-arena params 分支

**Files:**
- Modify: `src/core/ctx/context.c`

- [ ] **Step 1: 更新 csilk_ctx_cleanup() 中的 arena reset 部分**

将当前的 params 清理代码（约第 363-370 行）：
```c
    if (c->arena) {
        csilk_arena_reset(c->arena);
    } else {
        for (int i = 0; i < c->params_count; i++) {
            free(c->params[i].key);
            free(c->params[i].value);
        }
    }
    c->params_count = 0;
```
替换为（保留 no-arena fallback 但加注释说明用途）：
```c
    /* 8. Arena reset — O(1) reclaim all request-scoped allocations.
     * Params are now always populated via csilk_ctx_apply_route_result().
     * In arena mode they are reclaimed by arena_reset(); in no-arena mode
     * (test fixtures) they were malloc'd and must be freed individually. */
    if (c->arena) {
        csilk_arena_reset(c->arena);
    } else {
        for (int i = 0; i < c->params_count; i++) {
            free(c->params[i].key);
            free(c->params[i].value);
            c->params[i].key = NULL;
            c->params[i].value = NULL;
        }
    }
    c->params_count = 0;
```

- [ ] **Step 2: 运行所有 core 测试**

```bash
ctest --test-dir build -E test_integration --timeout 10 --output-on-failure
```
Expected: 全部 PASS。

- [ ] **Step 3: Commit**

```bash
git add src/core/ctx/context.c
git commit -m "refactor(ctx): ♻️ simplify params cleanup in csilk_ctx_cleanup"
```

---

## Chunk 2: Cleanup 路径完善与验证

### Task 2.1: 完善 csilk_ctx_cleanup() 步骤

**Files:**
- Modify: `src/core/ctx/context.c`

- [ ] **Step 1: 整理 cleanup 步骤顺序和注释**

参考 spec 中第三节的完整实现，更新 `csilk_ctx_cleanup()`：
1. Deferred callbacks (LIFO)
2. Storage destructors + driver clear
3. Zero-copy file response — close fd + reset offset/size
4. Request body — check `body_is_managed && body_ownership in {HEAP, TRANSFER}`
5. Response body — same check + reset status
6. Path — always free
7. Read buffers — pool return or free, then reset pointers to embedded
8. Arena reset (or no-arena param free)
9. Header maps — clear ONLY used maps
10. Handler chain state
11. Flow control state (including `on_ws_send = NULL`)
12. Request ID

- [ ] **Step 2: 运行全量测试**

```bash
ctest --test-dir build -E test_integration --timeout 10 --output-on-failure
```
Expected: 全部 PASS。

- [ ] **Step 3: Commit**

```bash
git add src/core/ctx/context.c
git commit -m "refactor(ctx): ♻️ reorder and document csilk_ctx_cleanup steps"
```

---

### Task 2.2: ASAN/TSAN 验证

- [ ] **Step 1: ASAN 构建并运行**

```bash
cmake -B build_asan -S . -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang \
  -DUSE_ASAN=ON -DENABLE_OOM_TEST=ON
cmake --build build_asan -j$(nproc)
ctest --test-dir build_asan -E test_integration --timeout 30 --output-on-failure
```

- [ ] **Step 2: TSAN 构建并运行（可选，耗时长）**

```bash
cmake -B build_tsan -S . -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang \
  -DUSE_TSAN=ON -DENABLE_OOM_TEST=ON
cmake --build build_tsan -j$(nproc)
ctest --test-dir build_tsan -R "test_router|test_radix|test_params_limit|test_context|test_middleware_chain" --timeout 30 --output-on-failure
```

- [ ] **Step 3: 清理临时 build 目录**

```bash
rm -rf build_asan build_tsan
```

---

### Task 2.3: 集成测试回归

- [ ] **Step 1: 运行集成测试**

```bash
ctest --test-dir build -R test_integration --timeout 30 --output-on-failure
```
Expected: 全部 PASS。

- [ ] **Step 2: 运行所有测试**

```bash
ctest --test-dir build --timeout 60 --output-on-failure
```
Expected: 全部 PASS。

---

## 文件变更总览

| 文件 | 变更类型 | 说明 |
|------|----------|------|
| `include/csilk/core/router.h` | Modify | 新增 `csilk_route_result_t`、`csilk_route_param_t`、新 API 声明 |
| `src/core/primitives/router_internal.h` | Modify | 更新 `match_node()` 签名 |
| `src/core/primitives/router_trie.c` | Modify | 重构所有匹配函数，移除 ctx 参数，使用 result |
| `src/core/primitives/router.c` | Modify | 新增 `csilk_router_match_result()`、`csilk_ctx_apply_route_result()`；更新 `csilk_router_match()` 和 `csilk_router_match_ctx()` 为薄包装 |
| `src/core/ctx/context.c` | Modify | 简化 cleanup 中的 params 处理，完善步骤注释 |
| `tests/core/test_router_match_result.c` | Create | 新测试：无 ctx 匹配、backtrack 验证、apply_result 验证 |
| `cmake/tests.cmake` | Modify | 注册新测试目标 |
