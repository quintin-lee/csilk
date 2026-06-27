# Configuration Guide

csilk supports flexible configuration via YAML files or direct C structure manipulation. The configuration covers server, TLS, logger, CORS, rate limiting, static files, AI, database, and admin dashboard settings.

## YAML Configuration Schema

The server can load a `config.yaml` file automatically or manually.

```yaml
server:
  port: 8080
  listen_backlog: 128
  max_connections: 0      # 0 = unlimited
  worker_threads: 4       # Multi-core SO_REUSEPORT (thread-safe client pool)
  tcp_nodelay: true
  idle_timeout_ms: 30000
  read_timeout_ms: 5000
  write_timeout_ms: 5000
  request_timeout_ms: 10000
  max_header_size: 8192
  max_body_size: 1048576  # 1MB
  max_url_size: 8192      # Maximum URL length (0 = unlimited)
  max_headers_count: 100  # Maximum number of request headers (0 = unlimited)
  
  # TLS/HTTPS settings (OpenSSL required)
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
  default_provider: "openai"        # "openai" or "ollama"
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
  default_driver: "qdrant"          # "qdrant" or "milvus"
  qdrant_endpoint: "http://localhost:6333"
  milvus_endpoint: "http://localhost:19530"

perm:
  driver: "simple"                  # "simple" (in-memory RBAC)
  config_path: "perms.yaml"         # Optional seed file for roles/permissions

cipher:
  key_file: "keys/cipher.key"       # AES-256-GCM key file
  enable_aes: true
  enable_rsa: true
  rsa_public_key: "keys/rsa.pub"
  rsa_private_key: "keys/rsa.pem"
```

## Programmatic Configuration (C API)

### 1. High-Level App Config

The app layer accepts a YAML config path at creation time:

```c
csilk_app_t* app = csilk_app_new("config.yaml");
// Pass nullptr for defaults (port 8080, stderr logging)
csilk_app_t* app_default = csilk_app_new(nullptr);
```

Individual settings can be tuned after creation:

```c
csilk_app_log_level(app, CSILK_LOG_DEBUG);
csilk_app_log_file(app, "logs/app.log", 10485760);
csilk_app_enable_openapi(app, 1);
```

### 2. Low-Level Server Config

```c
csilk_server_config_t cfg = {0};
cfg.enable_tls = 1;
cfg.tls_cert_file = "cert.pem";
cfg.tls_key_file = "key.pem";

csilk_server_set_config(server, &cfg);
```

## Loading from File

```c
csilk_config_t* cfg = csilk_config_load("config.yaml");
if (cfg) {
    csilk_server_apply_config(server, cfg);
    csilk_config_free(cfg);
}

---

## Further Reading

For deep-dive architectural details of each configurable subsystem, see:

| Subsystem | Module Design Document |
|-----------|----------------------|
| Server / TLS / HTTP/2 | [Server Core](../module-design/server.md) |
| App Layer & Static Files | [App Layer](../module-design/app.md) |
| Router & Param Extraction | [Router](../module-design/router.md) |
| Context & Arena | [Context & Arena](../module-design/context.md) |
| Middleware Chain | [Middleware](../module-design/middleware.md) |
| Database Drivers | [Data Layer](../module-design/data.md) |
| AI Providers | [AI Engine](../module-design/ai.md) |
| Driver Lifecycle | [Drivers](../module-design/drivers.md) |
| RBAC / JWT / CSRF / WAF | [Security](../module-design/security.md) |
| WebSocket / SSE | [Protocols](../module-design/protocols.md) |
| Message Queue | [Messaging](../module-design/messaging.md) |
| Prometheus Metrics | [Metrics](../module-design/metrics.md) |
| Crypto & Cipher | [Crypto](../module-design/crypto.md) |
| Lifecycle Hooks | [Hooks](../module-design/hooks.md) |
```
