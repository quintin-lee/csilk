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
```

## Programmatic Configuration (C API)

### 1. High-Level App Config

```c
csilk_app_config_t config = {
    .port = 8443,
    .log_level = CSILK_LOG_DEBUG,
    .worker_threads = 2
};
csilk_app_t* app = csilk_app_new(&config);
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
```
