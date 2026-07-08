# ABI 兼容性评估报告

> **更新日期**: 2026-05-31 | 评估 `csilk_ctx_s` 不透明类型转换（docs/meta/PLAN.md P2-1）

## 概述

**状态: 已完成** — 不透明类型转换已在 v0.3.0 中完整实现。

内部结构体定义（`csilk_ctx_s`、`csilk_client_t`、`csilk_server_s`）已从 `include/csilk/core/` 移至 `src/core/`（`ctx_types.h`、`srv_impl.h`、`srv_types.h`）。所有非框架代码均通过公共访问器 API 访问上下文状态。

---

## 当前状态

### 公共 API — 仅不透明前向声明
```c
typedef struct csilk_ctx_s csilk_ctx_t;  // include/csilk/types.h — 不透明句柄
```

### 内部定义（对公共 include/ 隐藏）
```
src/core/ctx_types.h    — csilk_ctx_s 布局（30+ 字段）
src/core/srv_types.h    — csilk_server_s 布局
src/core/internal/srv_impl.h     — 内部服务器实现细节
```

### 访问器 API（全覆盖）

| 访问器 | 提供功能 |
|---|---|
| `csilk_get_method` / `csilk_get_path` / `csilk_get_body` | 请求元数据 |
| `csilk_get_header` / `csilk_get_query` / `csilk_get_cookie` | 请求头/参数 |
| `csilk_get_param` / `csilk_get_params_count` | URL 路径参数 |
| `csilk_get_status` / `csilk_set_header` / `csilk_add_header` | 响应控制 |
| `csilk_get_arena` / `csilk_set` / `csilk_get` | Arena + 键值存储 |
| `csilk_is_websocket` / `csilk_is_sse` / `csilk_is_aborted` | 协议模式标志 |
| `csilk_is_async` / `csilk_ctx_set_async` | 异步响应模式 |
| `csilk_get_handler_index` / `csilk_get_work_req` | 处理器链状态 |
| `csilk_get_file_fd` / `csilk_set_file_response` | 零拷贝文件 I/O |
| `csilk_get_response_body` / `csilk_set_response_body` | 响应体操作 |
| `csilk_ctx_get_server` / `csilk_ctx_get_mq` | 框架对象访问 |
| `csilk_ctx_defer` / `csilk_ctx_defer_free` | Panic 安全资源清理 |
| `csilk_for_each_header` / `csilk_for_each_query` / `csilk_for_each_form_field` | 迭代 API |
| `csilk_bind_json` / `csilk_bind_reflect` / `csilk_parse_form_urlencoded` | 请求体解析 |

### 仅使用不透明类型的公共头文件
```
include/csilk/types.h       — 仅前向声明
include/csilk/context.h     — 所有访问器，无结构体解引用
include/csilk/response.h    — 状态/头/体/重定向/流式 API
include/csilk/router.h      — 路由注册与匹配
include/csilk/server.h      — 服务器生命周期
include/csilk/middleware.h  — 15 个内置中间件
include/csilk/hooks.h       — 生命周期钩子
include/csilk/websocket.h   — WebSocket API
include/csilk/sse.h         — SSE API
include/csilk/mq.h          — 消息队列 API
include/csilk/workflow.h    — 工作流引擎 API
include/csilk/admin.h       — 管理后台 API
include/csilk/group.h       — 路由组
include/csilk/config.h      — 配置类型
include/csilk/errors.h      — 错误码
include/csilk/version.h     — 版本信息（由 version.h.in 生成）
```

### 已完成迁移项目：
- [x] 15 个内置中间件 — 全部仅使用访问器 API
- [x] 30+ 测试文件 — 移除 `#include "ctx_types.h"`，使用公共 API
- [x] 11 个示例程序 — 迁移至公共访问器 API
- [x] 内部头文件已物理移至 `src/core/`（提交: 830daca）
- [x] 迭代 API（`csilk_for_each_*`）已添加，支持头/查询/表单遍历
- [x] 延迟清理 API 保护跨 `longjmp` 的资源泄漏

---

## 未来规划（v1.0+）

不透明类型转换在 v0.3.0 中功能上已完成。对于未来的 v1.0 ABI 冻结：
1. 考虑将 `csilk_router_t` 和 `csilk_server_t` 同样设为不透明。
2. 在 `csilk_server_new` 中添加带版本号的 ABI 兼容性检查。
3. 为驱动虚函数表布局添加预留填充字段，以备未来扩展。
