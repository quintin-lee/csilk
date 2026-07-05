# 部署运维指南

> **Version**: 0.3.0 | **Last updated**: 2026-06-29

本指南描述如何将 csilk 应用部署到生产环境，涵盖 Docker 容器化、systemd 服务管理、多 worker 调优、TLS 配置、Prometheus 监控、健康检查及日志管理。

---

## 1. Docker 部署

### 1.1 使用官方 Dockerfile

项目根目录已提供 `Dockerfile`：

```dockerfile
FROM ubuntu:22.04 AS builder
# ... 完整构建流程参见 Dockerfile

FROM ubuntu:22.04
COPY --from=builder /app/build/example_server /app/
COPY --from=builder /app/config.yaml /app/
EXPOSE 8080
CMD ["/app/example_server"]
```

构建与运行：

```bash
docker build -t csilk-app .
docker run -d \
  --name my-csilk \
  -p 8080:8080 \
  -v /host/config.yaml:/app/config.yaml \
  -v /host/certs:/app/certs \
  csilk-app
```

### 1.2 多阶段构建优化

```dockerfile
# --- 构建阶段 ---
FROM ubuntu:22.04 AS build
RUN apt-get update && apt-get install -y \
    cmake gcc-13 libyaml-dev libssl-dev zlib1g-dev libcurl4-openssl-dev

WORKDIR /src
COPY . .
RUN mkdir build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release \
             -DCMAKE_C_COMPILER=gcc-13 && \
    make -j$(nproc) example_server

# --- 运行阶段 ---
FROM ubuntu:22.04
RUN apt-get update && apt-get install -y --no-install-recommends \
    libyaml-0-2 libssl3 zlib1g libcurl4 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/example_server /app/server
COPY config.yaml /app/config.yaml

EXPOSE 8080
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
  CMD curl -f http://localhost:8080/healthz || exit 1

CMD ["/app/server"]
```

> **注意**：运行阶段仅包含运行时库（`libyaml-0-2`、`libssl3`、`zlib1g`、`libcurl4`），不包含构建工具，镜像体积可控制在 80MB 以内。

### 1.3 Docker Compose

```yaml
version: "3.8"
services:
  app:
    build: .
    ports:
      - "443:8080"
    volumes:
      - ./config.yaml:/app/config.yaml
      - ./certs:/app/certs
      - ./data:/app/data
    environment:
      - OPENAI_API_KEY=${OPENAI_API_KEY}
    restart: always
    healthcheck:
      test: ["CMD", "curl", "-f", "http://localhost:8080/healthz"]
      interval: 30s
      timeout: 3s
      retries: 3
```

---

## 2. systemd 服务管理

### 2.1 Service 单元文件

创建 `/etc/systemd/system/csilk-app.service`：

```ini
[Unit]
Description=csilk HTTP Service
Documentation=https://github.com/username/csilk
After=network.target

[Service]
Type=simple
User=www-data
Group=www-data
WorkingDirectory=/opt/csilk-app

# 可执行文件与配置
ExecStart=/opt/csilk-app/build/example_server
ExecReload=/bin/kill -HUP $MAINPID

# 资源限制
LimitNOFILE=1048576
LimitNPROC=65536

# 安全硬化
ProtectSystem=full
ProtectHome=true
NoNewPrivileges=true
PrivateTmp=true

# 日志
StandardOutput=journal
StandardError=journal

# 重启策略
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

### 2.2 管理命令

```bash
# 启用并启动服务
sudo systemctl enable csilk-app
sudo systemctl start csilk-app

# 查看状态
sudo systemctl status csilk-app

# 查看实时日志
sudo journalctl -u csilk-app -f

# 重新加载配置后重启
sudo systemctl reload csilk-app
# 或完全重启
sudo systemctl restart csilk-app
```

---

## 3. 多 Worker 调优

### 3.1 配置 Worker 数量

```yaml
server:
  worker_threads: 0      # 0 = 自动设置为 CPU 核心数
  listen_backlog: 65535
  tcp_nodelay: true
  max_connections: 0     # 0 = 无限制
```

```c
// 或通过 C API 配置
csilk_server_config_t config = {
    .worker_threads = 16,        // 建议 = 物理 CPU 核心数
    .listen_backlog = 65535,
    .tcp_nodelay = 1,
    .max_connections = 100000,
};
csilk_server_set_config(server, &config);
```

### 3.2 Worker 调优建议

| 场景 | 建议 |
|:-----|:------|
| 4 核 CPU | `worker_threads = 4`，单 Worker 约 50K QPS |
| 16 核 CPU | `worker_threads = 16`，线性扩至 ~200K QPS |
| AI 密集型 | 保留 1-2 核心给 libuv 线程池 |
| 高并发连接 | 增大 `listen_backlog` 和 `max_connections` |
| CPU 亲和性 | 设置 `enable_cpu_affinity = 1`（需平台支持） |

---

## 4. TLS/HTTPS 配置

### 4.1 YAML 配置

```yaml
server:
  port: 443
  enable_tls: true
  tls_cert_file: "/etc/ssl/certs/server.crt"
  tls_key_file: "/etc/ssl/private/server.key"
  tls_ca_file: "/etc/ssl/certs/ca.crt"
  tls_verify_peer: false     # 客户端证书验证（可选）
```

### 4.2 C API 配置

```c
csilk_server_config_t config = {
    .enable_tls = 1,
    .tls_cert_file = "certs/server.crt",
    .tls_key_file = "certs/server.key",
    .tls_min_version = TLS_VERSION_1_3,  // MUST: TLS 1.3
};
csilk_server_set_config(server, &config);
```

### 4.3 使用 Let's Encrypt 自动续签

```bash
# 使用 certbot 获取证书
sudo certbot certonly --standalone -d example.com

# 配置 csilk 使用证书
# tls_cert_file: /etc/letsencrypt/live/example.com/fullchain.pem
# tls_key_file:  /etc/letsencrypt/live/example.com/privkey.pem

# 续签后重启服务
sudo certbot renew --deploy-hook "systemctl reload csilk-app"
```

---

## 5. Prometheus 指标采集

csilk 内置 Prometheus 指标端点，集成 Admin Dashboard 即可启用：

```c
#include "csilk/app/admin.h"

int main() {
    csilk_app_t* app = csilk_app_new("config.yaml");
    csilk_admin_serve(app, "/admin");
    csilk_app_run(app, 8080);
    csilk_app_free(app);
    return 0;
}
```

### 5.1 Prometheus 抓取配置

```yaml
# prometheus.yml
scrape_configs:
  - job_name: 'csilk'
    scrape_interval: 10s
    metrics_path: '/admin/stats'
    static_configs:
      - targets: ['localhost:8080']
```

### 5.2 公开指标

启用 Admin Dashboard 后，`GET /admin/stats` 返回 JSON 格式指标，包含：

| 指标前缀 | 说明 |
|:---------|:------|
| `http_requests_total` | 总请求数 |
| `http_request_duration_ms` | 请求延迟直方图 |
| `http_requests_active` | 活跃连接数 |
| `http_requests_2xx/4xx/5xx` | 状态码分布 |
| `mq_published_total` | MQ 发布消息数 |
| `mq_queue_depth` | MQ 队列深度 |
| `db_queries_total` | 数据库查询数 |
| `ai_requests_total` | AI 请求数 |
| `ai_tokens_total` | AI token 消耗 |

---

## 6. 健康检查

csilk 内置两个健康检查端点：

```c
#include "csilk/csilk.h"

int main() {
    csilk_router_t* r = csilk_router_new();

    // 浅健康检查 — 返回 {"status":"up"}
    csilk_router_add(r, "GET", "/healthz",
        (csilk_handler_t[]){csilk_health_check_handler, NULL}, 1);

    // 深度健康检查 — 检查 MQ 和连接池状态，返回 200 或 503
    csilk_router_add(r, "GET", "/readyz",
        (csilk_handler_t[]){csilk_ready_check_handler, NULL}, 1);

    csilk_server_t* s = csilk_server_new(r);
    csilk_server_run(s, 8080);
    csilk_server_free(s);
    return 0;
}
```

- `/healthz` — 轻量存活检查，仅返回 JSON 状态
- `/readyz` — 就绪检查，验证 MQ、数据库连接池等子系统是否可用

使用 `curl` 验证：

```bash
curl http://localhost:8080/healthz
# {"status":"up"}

curl http://localhost:8080/readyz
# {"status":"ready"} 或 503 {"status":"not ready"}
```

---

## 7. 日志管理

### 7.1 配置日志

```yaml
logger:
  level: "INFO"            # DEBUG, INFO, WARN, ERROR
  file_path: "logs/app.log"
  max_file_size: 10485760  # 10MB 自动轮转
  max_backup_files: 5
```

### 7.2 结构日志（JSON 格式）

```c
csilk_app_log_level(app, CSILK_LOG_INFO);
csilk_app_log_file(app, "logs/app.log", 10 * 1024 * 1024);
csilk_app_log_json(app, 1);  // 启用 JSON 格式输出
```

JSON 日志格式便于与 ELK/Loki 等日志系统集成：

```json
{"time":"2026-06-29T10:30:00Z","level":"INFO","msg":"Server started on port 8080"}
{"time":"2026-06-29T10:30:01Z","level":"INFO","msg":"Request: GET /api/users 200","req_id":"abc123"}
```

---

## 8. 完整部署示例

结合 Docker、TLS、健康检查和 Prometheus：

```c
#include "csilk/csilk.h"
#include "csilk/app/admin.h"

void on_start(csilk_server_t* s) {
    CSILK_LOG_I("csilk server ready on port %d", csilk_server_get_port(s));
}

int main() {
    // 初始化应用
    csilk_app_t* app = csilk_app_new("/etc/csilk/config.yaml");

    // 注册钩子
    csilk_server_add_hook(csilk_get_server_from_app(app),
        CSILK_HOOK_SERVER_START, on_start);

    // 中间件
    csilk_app_use(app, csilk_recovery_handler);
    csilk_app_use(app, csilk_logger_handler);
    csilk_app_use(app, csilk_request_id_middleware);

    // 健康检查
    csilk_app_get(app, "/healthz", csilk_health_check_handler);
    csilk_app_get(app, "/readyz",  csilk_ready_check_handler);

    // Admin Dashboard（含 Prometheus 指标）
    csilk_admin_serve(app, "/admin");

    // 业务路由
    csilk_app_get(app, "/api/users", list_users);

    // 启动（端口和 TLS 从 config.yaml 读取）
    csilk_app_run(app, 0);  // 0 = 使用配置中的端口
    csilk_app_free(app);
    return 0;
}
```

`config.yaml`：

```yaml
server:
  port: 443
  enable_tls: true
  tls_cert_file: "/etc/letsencrypt/live/example.com/fullchain.pem"
  tls_key_file: "/etc/letsencrypt/live/example.com/privkey.pem"
  worker_threads: 0
  tcp_nodelay: true

logger:
  level: "INFO"
  file_path: "/var/log/csilk/app.log"
  max_file_size: 10485760
  max_backup_files: 5
```

---

## 9. 内核参数调优

### 9.1 通用网络调优

```bash
# /etc/sysctl.d/99-csilk.conf
# TCP Fast Open
net.ipv4.tcp_fastopen = 3

# 增大 backlog
net.core.somaxconn = 65535
net.ipv4.tcp_max_syn_backlog = 65535

# 启用 TIME_WAIT 复用
net.ipv4.tcp_tw_reuse = 1

# 关闭慢启动重启
net.ipv4.tcp_slow_start_after_idle = 0

# 增大 socket 缓冲区
net.core.rmem_max = 134217728
net.core.wmem_max = 134217728
net.ipv4.tcp_rmem = 4096 87380 134217728
net.ipv4.tcp_wmem = 4096 65536 134217728

# 文件描述符限制
fs.file-max = 2097152
```

```bash
# /etc/security/limits.d/99-csilk.conf
* soft nofile 1048576
* hard nofile 1048576
```

### 9.2 io_uring 内核要求

如果使用 io_uring 后端（`-DCSILK_USE_URING=ON`），需要以下内核支持：

| 条件 | 最低版本 | 推荐版本 | 说明 |
|:-----|:--------:|:--------:|------|
| Linux Kernel | 5.1 | **6.1+** | 5.1 引入基础 io_uring；5.6+ 支持轮询模式；6.1+ 为生产稳定版本 |
| io_uring 启用 | — | — | 内核默认启用（`/proc/sys/kernel/io_uring_disabled = 0`） |
| liburing | — | v2.5+ | CMake FetchContent 自动获取 |

验证内核支持：
```bash
uname -r                     # 应输出 ≥ 5.1
cat /proc/sys/kernel/io_uring_disabled  # 应为 0
```

---

## 10. 最佳实践

| 实践 | 说明 |
|:-----|:------|
| **MUST** 生产启用 TLS 1.3 | 设置 `enable_tls: true`，`tls_min_version = TLS_VERSION_1_3` |
| **SHOULD** worker 数 = CPU 核心数 | 设置 `worker_threads: 0` 自动检测 |
| **MUST** 配置健康检查 | 集成 `/healthz` 和 `/readyz` 用于 K8s 或负载均衡 |
| **SHOULD** 启用日志轮转 | 设置 `max_file_size` 和 `max_backup_files` |
| **SHOULD** 使用多阶段 Docker 构建 | 运行镜像仅包含运行时依赖，控制体积 |
| **SHOULD** 配置系统 fd 限制 | `LimitNOFILE=1048576` 支持高并发 |
| **MAY** 启用 Admin Dashboard | 实时查看 HTTP/MQ/DB/AI 指标 |
| **MAY** 集成 Prometheus | 通过 `/admin/stats` 端点采集 |
| **MUST NOT** 在生产使用 `-Ofast` | 浮点语义差异可能导致非预期行为 |
| **MUST** 生产构建使用 Release 模式 | `-DCMAKE_BUILD_TYPE=Release` |
| **MAY** 使用 io_uring 后端提升性能 | Linux 5.1+ 可用；构建时加 `-DCSILK_USE_URING=ON`；详见 [构建指南](../contributing/how-to-build.md) |

---

## 延伸阅读

| 文档 | 内容 |
|:-----|:------|
| [配置指南](./configuration.md) | YAML 配置完整参考 |
| [性能调优指南](../performance-tuning.md) | 编译器优化、内核参数、PGO 全攻略 |
| [模块设计 — Server Core](../../docs/module-design/server.md) | 多 Worker 实现细节 |
| [故障排查手册](../troubleshooting.md) | 常见问题与调试技巧 |
| [Dockerfile](../../docker/Dockerfile) | 官方 Docker 构建文件 |
