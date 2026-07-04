# ABI 稳定性路线图 — v1.0 上下文不透明类型转换

> **状态**: 已完成 | **完成于**: v0.3.0 | **基于**: ABI_REPORT.md
>
> **ABI 规则**: 所有公共 API 函数 **必须** 接受/返回 `csilk_ctx_t*`（不透明）— 直接结构体访问 **不得** 暴露在公共头文件中。访问器函数调用开销在内联时 **应该** 为零（单指针解引用，≤ 1 CPU 周期）。

## 当前状态

`csilk_ctx_s` 内部布局隐藏在 `src/core/ctx_types.h` 中。所有公共 API 通过访问器函数使用不透明的 `csilk_ctx_t*` 句柄。

以下所有阶段已在 v0.3.0 开发周期中完成：

## 已完成阶段

### 阶段 A：访问器 API 扩展（v0.3.0）✅

在 `include/csilk/context.h` 中实现了访问器/修改器 API：

```c
const char* csilk_get_method(csilk_ctx_t* c);       // （原计划中为 csilk_ctx_get_method）
const char* csilk_get_path(csilk_ctx_t* c);          // （原计划中为 csilk_ctx_get_path）
int         csilk_get_status(csilk_ctx_t* c);        // （原计划中为 csilk_ctx_get_status）
int         csilk_is_websocket(csilk_ctx_t* c);      // （原计划中为 csilk_ctx_is_websocket）
csilk_arena_t* csilk_get_arena(csilk_ctx_t* c);      // （原计划中为 csilk_ctx_get_arena）
const char* csilk_get_header(csilk_ctx_t* c, const char* key);
void        csilk_set_header(csilk_ctx_t* c, const char* key, const char* value);
void        csilk_set_response_body(csilk_ctx_t* c, const char* data, size_t len, int managed);
// ... 以及 30+ 更多访问器已完整实现
```

### 阶段 B-E：迁移完成（v0.3.0）✅

- 更新所有框架源代码（`src/`）使用访问器而非直接结构体字段访问
- 迁移 15 个中间件模块到访问器 API
- 将 `ctx_types.h` 标记为 `@deprecated` 发布

### 阶段 C：测试与示例迁移（v0.7.0）
- 更新 30+ 测试文件使用访问器
- 更新 3 个示例
- 设置 `CSILK_DEPRECATE_CTX_TYPES` 编译标志作为迁移辅助

### 阶段 D：最终不透明化（v1.0）
- 将 `ctx_types.h` 从 `include/` 移至 `src/core/`
- 从公共头文件中移除 `csilk_request_t` 和 `csilk_response_t`（已前瞻声明为 `csilk_ctx_t`）
- 提升 SOVERSION 到 1

## 风险

| 风险 | 缓解措施 |
|------|---------|
| 破坏 30+ 测试文件 | 分阶段迁移，带弃用警告 |
| 访问器调用导致的性能回归 | 在头文件中内联访问器函数 |
| SSE/WebSocket 回调需要结构体访问 | 添加专用的 SSE/WS 访问器子集 |
