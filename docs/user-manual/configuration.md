# Configuration

The server supports YAML configuration for defining server settings, logging, and middleware parameters. Configuration is loaded at startup via libyaml.

## Configuration System

```mermaid
flowchart TB
    subgraph Input
        YAML["config.yaml"]
    end

    subgraph Loader
        P["csilk_load_config(path, &config)"]
        V["csilk_config_validate(&config, err_msg)"]
    end

    subgraph "csilk_config_t"
        S["server:\n  idle_timeout_ms: 5000\n  max_body_size: 1048576\n  max_header_size: 65536\n  max_connections: 10000\n  listen_backlog: 128\n  tcp_nodelay: true\n  tcp_keepalive: 60\n  worker_threads: 4"]

        L["log:\n  level: info\n  file: /var/log/csilk.log\n  json: true\n  rotate_size_mb: 100"]

        MW["middleware:\n  cors: { ... }\n  rate_limit: { ... }\n  static_files: { ... }"]
    end

    subgraph Apply
        SRV["csilk_server_set_config()"]
        LOG["csilk_log_init()"]
        USE["csilk_server_use() for middleware"]
    end

    YAML --> P
    P --> S
    P --> L
    P --> MW
    S --> V
    V --> SRV
    L --> LOG
    MW --> USE
```

## Configuration Lifecycle

```mermaid
sequenceDiagram
    participant Main
    participant App as csilk_app_t
    participant Config
    participant Server
    participant Log

    Main->>App: csilk_app_new("config_multi.yaml")
    App->>Config: csilk_load_config(path, &config)
    Config->>Config: Open YAML file
    Config->>Config: Parse: libyaml state machine
    Config->>Config: Fill csilk_config_t struct

    Config->>Config: csilk_config_validate(&config, err)
    Config->>Config: Check required fields
    Config->>Config: Check value ranges
    Config-->>App: 0 (success) or -1 (error)

    App->>Server: csilk_server_new(router)
    App->>Log: csilk_log_init(config.log)

    User->>App: csilk_app_apply_config(app)
    App->>Server: csilk_server_set_config(server, config.server)
    App->>Server: csilk_server_set_max_connections(server, config.max_connections)
    App->>App: For each middleware config, apply to server

    User->>App: csilk_app_run(app, 8080)
```

## Full Configuration Schema

```yaml
# Server settings
server:
  idle_timeout_ms: 5000       # Connection idle timeout (ms)
  max_body_size: 1048576      # Max request body (1MB default)
  max_header_size: 65536      # Max request headers (64KB default)
  max_connections: 10000      # Max concurrent connections (0 = unlimited)
  listen_backlog: 128         # TCP listen backlog
  tcp_nodelay: true           # Disable Nagle's algorithm
  tcp_keepalive: 60           # TCP keepalive interval (seconds)
  worker_threads: 4           # Number of worker threads (0 = 1)

# Logging settings
log:
  level: info                 # trace, debug, info, warn, error, fatal
  file: "/var/log/csilk.log"  # Log file path (NULL = stdout)
  json: true                  # JSON format output
  rotate_size_mb: 100         # Max log file size before rotation

# CORS middleware
middleware:
  cors:
    enabled: true
    allow_origins: ["http://localhost:3000"]
    allow_methods: ["GET", "POST", "PUT", "DELETE"]
    allow_headers: ["Content-Type", "Authorization"]
    max_age: 3600

  # Rate limit middleware
  rate_limit:
    enabled: true
    requests_per_second: 100
    burst: 200

  # Static file middleware
  static_files:
    enabled: true
    root_dir: "./public"
    cache_time: 3600
```

## Configuration Data Flow

```mermaid
flowchart LR
    subgraph "YAML Parser (libyaml)"
        S1["yaml_parser_t"]
        S2["Event Stream:\nSTREAM_START\nDOCUMENT_START\nMAPPING_START\n  SCALAR: 'server'\n  MAPPING_START\n    SCALAR: 'port'\n    SCALAR: '8080'\n  MAPPING_END\nMAPPING_END\nDOCUMENT_END\nSTREAM_END"]
        S3["csilk_config_t struct"]
    end

    YML["config.yaml\n{server: {port: 8080}}"] --> S1
    S1 --> S2
    S2 --> S3
    S3 --> VAL["config_validate()\nCheck: port > 0 && port < 65536\nCheck: timeout > 0"]
    VAL --> APPLY["Applied to csilk_server_s"]
```
