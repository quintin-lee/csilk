# Admin Dashboard 使用指南

> **Version**: 0.3.0 | **Last updated**: 2026-06-29

csilk 的 Admin Dashboard 是一个统一的 Web 管理界面，提供对 HTTP 指标、AI 引擎、消息队列（MQ）和数据库运行状态的实时监控。

---

## 1. 快速启用

### 1.1 基本用法

```c
#include "csilk/app/admin.h"

int main() {
    csilk_app_t* app = csilk_app_new("config.yaml");
    csilk_admin_serve(app, "/admin");  // 仪表板挂载到 /admin
    csilk_app_run(app, 8080);
    csilk_app_free(app);
    return 0;
}
```

访问 `http://localhost:8080/admin/` 即可看到仪表板。

### 1.2 受保护的 Admin

```c
csilk_admin_serve_secure(app, "/admin", (csilk_handler_t)csilk_jwt_middleware, "secret-key");
```

所有 Admin 路由（UI、Stats API、WebSocket）都被中间件保护。

### 1.3 YAML 配置

```yaml
admin:
  enabled: true
  path: "/admin"
```

然后通过 `csilk_app_apply_config(app)` 自动启用。

---

## 2. 仪表板功能

Dashboard 包含以下实时监控面板：

### HTTP 指标面板

| 指标 | 说明 |
|:-----|:------|
| QPS | 每秒请求数（实时） |
| 延迟直方图 | P50/P90/P99 延迟分布 |
| 状态码分布 | 2xx / 4xx / 5xx 比例 |
| 活跃连接数 | 当前 Keep-Alive 连接数 |

### MQ 面板

| 指标 | 说明 |
|:-----|:------|
| 消息吞吐量 | 发布/投递速率 |
| 队列深度 | 积压消息数 |
| 失败率 | 处理失败的消息比例 |
| 主题列表 | 已注册主题统计 |

### AI 面板

| 指标 | 说明 |
|:-----|:------|
| 请求数 | 总 AI 调用次数 |
| Token 消耗 | Prompt / Completion Token 量 |
| 错误率 | AI 调用失败率 |
| 延迟 | 平均每次调用耗时 |

### 数据库面板

| 指标 | 说明 |
|:-----|:------|
| 查询数 | SELECT 统计 |
| 写入数 | INSERT/UPDATE/DELETE 统计 |
| 错误率 | 操作失败率 |
| 连接池 | 池中连接状态 |

### 进程面板

| 指标 | 说明 |
|:-----|:------|
| RSS 内存 | 常驻内存大小 |
| CPU 使用率 | 进程 CPU 占比 |
| 运行时间 | 服务器启动后的运行时长 |

---

## 3. API 端点

Admin Dashboard 注册了以下端点：

| 端点 | 方法 | 说明 |
|:-----|:------|:------|
| `GET /admin/` | GET | HTML 仪表板页面 |
| `GET /admin/stats` | GET | JSON 格式全量指标快照 |
| `GET /admin/ws` | WebSocket | 实时事件流推送 |

### 3.1 Stats API

`GET /admin/stats` 返回 JSON 格式的完整系统状态，兼容 Prometheus 采集：

```bash
curl http://localhost:8080/admin/stats
```

输出示例：

```json
{
  "http": {
    "requests_total": 15234,
    "requests_2xx": 14800,
    "requests_4xx": 380,
    "requests_5xx": 54,
    "requests_active": 128,
    "request_duration_p50_us": 120,
    "request_duration_p90_us": 450,
    "request_duration_p99_us": 1200
  },
  "mq": {
    "published_total": 8921,
    "delivered_total": 8890,
    "failed_total": 31,
    "queue_depth": 15
  },
  "ai": {
    "requests_total": 567,
    "tokens_total": 125000,
    "errors_total": 3
  },
  "db": {
    "queries_total": 45000,
    "execs_total": 12000,
    "errors_total": 0
  },
  "process": {
    "rss_bytes": 25165824,
    "uptime_sec": 86400
  }
}
```

### 3.2 WebSocket 实时流

`GET /admin/ws` 建立 WebSocket 连接，服务器每秒推送一次增量指标：

```javascript
// 浏览器端示例
const ws = new WebSocket('ws://localhost:8080/admin/ws');
ws.onmessage = (event) => {
    const stats = JSON.parse(event.data);
    updateDashboard(stats);
};
```

---

## 4. 自定义管理页面

### 4.1 添加自定义端点

```c
void custom_admin_stats(csilk_ctx_t* c) {
    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "custom_metric", "hello");
    csilk_json(c, 200, json);
    cJSON_Delete(json);
}

void setup_custom_admin(csilk_app_t* app) {
    csilk_admin_serve(app, "/admin");

    // 注册自定义管理端点
    csilk_app_get(app, "/admin/custom", custom_admin_stats);
}
```

### 4.2 扩展浏览器端

Admin Dashboard 的 HTML 页面加载在 `share/csilk/admin/`（或嵌入的 HTML 字符串）中，可以通过添加自定义 JavaScript 事件监听来扩展。

---

## 5. 保护 Admin Dashboard

### 5.1 JWT 保护

```c
// 所有 Admin 路由需要 JWT 认证
csilk_admin_serve_secure(app, "/admin",
    (csilk_handler_t)csilk_jwt_middleware, "your-secret-key");
```

### 5.2 自定义认证中间件

```c
void custom_admin_auth(csilk_ctx_t* c) {
    const char* api_key = csilk_get_header(c, "X-Admin-Key");
    if (!api_key || strcmp(api_key, getenv("ADMIN_API_KEY")) != 0) {
        csilk_json_error(c, 401, "Unauthorized");
        return;
    }
    csilk_next(c);
}

csilk_admin_serve_secure(app, "/admin", custom_admin_auth);
```

### 5.3 环境区分

生产环境建议仅在内部网络暴露：

```c
if (getenv("ENABLE_ADMIN")) {
    csilk_admin_serve(app, "/admin");
}
```

---

## 6. 完整示例

```c
#include "csilk/csilk.h"
#include "csilk/app/admin.h"
#include "csilk/drivers/db.h"

static csilk_db_pool_t* g_db;

void admin_login(csilk_ctx_t* c) {
    // 简单的管理登录端点
    cJSON* body = csilk_get_json(c);
    const char* user = cJSON_GetObjectItem(body, "user")->valuestring;
    const char* pass = cJSON_GetObjectItem(body, "pass")->valuestring;

    if (strcmp(user, "admin") == 0 && strcmp(pass, getenv("ADMIN_PASS")) == 0) {
        csilk_json_string(c, 200, "{\"token\":\"admin-token\"}");
    } else {
        csilk_json_error(c, 401, "Invalid credentials");
    }
    cJSON_Delete(body);
}

// 自定义统计数据端点
void custom_health(csilk_ctx_t* c) {
    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "version", "0.3.0");
    cJSON_AddNumberToObject(json, "uptime_sec", uv_now(uv_default_loop()) / 1000);
    csilk_json(c, 200, json);
    cJSON_Delete(json);
}

int main() {
    csilk_app_t* app = csilk_app_new("config.yaml");

    // 健康检查和公开路由
    csilk_app_get(app, "/healthz", csilk_health_check_handler);

    // 管理登录
    csilk_app_post(app, "/admin/login", admin_login);

    // Admin Dashboard（JWT 保护）
    csilk_admin_serve_secure(app, "/admin",
        (csilk_handler_t)csilk_jwt_middleware, getenv("JWT_SECRET"));

    // 自定义管理端点
    csilk_app_get(app, "/admin/health", custom_health);

    csilk_app_run(app, 8080);
    csilk_app_free(app);
    return 0;
}
```

---

## 7. 最佳实践

| 实践 | 说明 |
|:-----|:------|
| **SHOULD** 生产环境启用认证 | 使用 `csilk_admin_serve_secure` 保护 Admin 面板 |
| **SHOULD** 限制 Admin 访问源 | 防火墙规则、反向代理 IP 白名单 |
| **MAY** 通过 `ENABLE_ADMIN` 环境变量控制 | 开发环境开启，生产按需 |
| **SHOULD** 配合 Prometheus 使用 | `GET /admin/stats` 可直接配置为 Prometheus 抓取端点 |
| **MAY** 添加自定义管理端点 | 在 `/admin` 下注册 GET 路由扩展功能 |
| **MUST NOT** 默认暴露到公网 | Admin Dashboard 应置于内网或 VPN 内 |

---

## 延伸阅读

| 文档 | 内容 |
|:-----|:------|
| [模块设计 — App 层](../../docs/module-design/app.md) | Admin Dashboard 内部实现 |
| [部署指南](./deployment.md) | Docker 部署、Prometheus 采集配置 |
| [安全指南](./security.md) | JWT 认证、RBAC 权限配置 |
