# 配置指南

csilk 支持通过 YAML 文件或直接的 C 结构体操作进行灵活配置。配置涵盖服务器、TLS、日志记录器、CORS、速率限制、静态文件、AI、数据库和管理仪表板设置。配置文件 **MUST** 是有效的 YAML 1.2 格式。服务器端口 **MUST** 在范围 [1, 65535] 内。工作线程数 **SHOULD** 等于 CPU 核心数。TLS 证书和密钥文件 **MUST** 为 PEM 编码格式。所有超时值 **MUST** 以秒为单位指定（整数）。

## YAML 配置架构

服务器可以自动或手动加载 `config.yaml` 文件。

```yaml
server:
  port: 8080
  listen_backlog: 128
  max_connections: 0      # 0 = 无限制
  worker_threads: 4       # 多核 SO_REUSEPORT (线程安全客户端池)
  tcp_nodelay: true
  idle_timeout_ms: 30000
  read_timeout_ms: 5000
  write_timeout_ms: 5000
  request_timeout_ms: 10000
  max_header_size: 8192
  max_body_size: 1048576  # 1MB
  max_url_size: 8192      # 最大 URL 长度 (0 = 无限制)
  max_headers_count: 100  # 最大请求头数量 (0 = 无限制)
  
  # TLS/HTTPS 设置 (需要 OpenSSL)
  enable_tls: true
  tls_cert_file: "certs/server.crt"
  tls_key_file: "certs/server.key"
  tls_ca_file: "certs/ca.crt"
  tls_verify_peer: false

logger:
  level: "INFO"           # DEBUG, INFO, WARN, ERROR
  file_path: "logs/app.log"
  max_file_size: 10485760 # 10MB
  max_backup_files: 5

cors:
  allow_origin: "*"
  allow_methods: "GET,POST,PUT,DELETE,OPTIONS"
  allow_headers: "Content-Type,Authorization"

rate_limit:
  requests_per_second: 100
  burst: 50

static_files:
  root_dir: "./public"
  enable_gzip: true

ai:
  default_provider: "openai"        # "openai" 或 "ollama"
  openai_base_url: "https://api.openai.com/v1"
  ollama_base_url: "http://localhost:11434"

database:
  default_driver: "sqlite"          # "sqlite", "mysql", "postgres", "mongodb"
  sqlite_path: "data/app.db"
  mysql_dsn: "user:password@tcp(localhost:3306)/dbname"
  postgres_dsn: "host=localhost port=5432 dbname=app user=user password=pass"
  mongodb_uri: "mongodb://localhost:27017"
  max_pool_size: 10

admin:
  enabled: true
  path: "/admin"

vector_db:
  default_driver: "qdrant"          # "qdrant" 或 "milvus"
  qdrant_endpoint: "http://localhost:6333"
  milvus_endpoint: "http://localhost:19530"

perm:
  driver: "simple"                  # "simple" (内存 RBAC)
  config_path: "perms.yaml"         # 可选的角色/权限种子文件

cipher:
  key_file: "keys/cipher.key"       # AES-256-GCM 密钥文件
  enable_aes: true
  enable_rsa: true
  rsa_public_key: "keys/rsa.pub"
  rsa_private_key: "keys/rsa.pem"
```

## 编程配置 (C API)

### 1. 高级应用配置

应用层在创建时接受 YAML 配置文件路径：

```c
csilk_app_t* app = csilk_app_new("config.yaml");
// 传递 nullptr 使用默认值 (端口 8080, 标准错误输出日志记录)
csilk_app_t* app_default = csilk_app_new(nullptr);
```

创建后可以调整各个设置：

```c
csilk_app_log_level(app, CSILK_LOG_DEBUG);
csilk_app_log_file(app, "logs/app.log", 10485760);
csilk_app_enable_openapi(app, 1);
```

### 2. 低级服务器配置

```c
csilk_server_config_t cfg = {0};
cfg.enable_tls = 1;
cfg.tls_cert_file = "cert.pem";
cfg.tls_key_file = "key.pem";

csilk_server_set_config(server, &cfg);
```

## 从文件加载

```c
csilk_config_t* cfg = csilk_config_load("config.yaml");
if (cfg) {
    csilk_server_apply_config(server, cfg);
    csilk_config_free(cfg);
}

---

## 进一步阅读

有关各可配置子系统的深入架构细节，请参见：

| 子系统 | 模块设计文档 |
|-----------|----------------------|
| 服务器 / TLS / HTTP/2 | [服务器核心](../module-design/server.md) |
| 应用层 & 静态文件 | [应用层](../module-design/app.md) |
| 路由器 & 参数提取 | [路由器](../module-design/router.md) |
| 上下文 & Arena | [上下文](../module-design/context.md) |
| 中间件链 | [中间件](../module-design/middleware.md) |
| 数据库驱动 | [数据层](../module-design/data.md) |
| AI 提供者 | [AI 引擎](../module-design/ai.md) |
| 驱动生命周期 | [驱动](../module-design/drivers.md) |
| RBAC / JWT / CSRF / WAF | [安全](../module-design/security.md) |
| WebSocket / SSE | [协议](../module-design/protocols.md) |
| 消息队列 | [消息传递](../module-design/messaging.md) |
| Prometheus 指标 | [指标](../module-design/metrics.md) |
| 加密 & 密钥 | [加密](../module-design/crypto.md) |
| 生命周期钩子 | [钩子](../module-design/hooks.md) |