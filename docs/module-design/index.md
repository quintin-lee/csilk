# Module Design Documents

Deep-dive architectural documentation for each core subsystem of csilk.

## Core Engine

| Document | Focus |
|----------|-------|
| [Server Core](server.md) | libuv (default) / io_uring (optional, Linux-only) event loop, TLS/ALPN, HTTP/2, worker pool, graceful shutdown |
| [Router](router.md) | Radix tree (Patricia trie), SIMD-accelerated matching, param extraction |
| [Context](context.md) | Request/response lifecycle, opaque API, deferred cleanup |
| [Arena](arena.md) | Bump allocator, zero-copy headers, SIMD-accelerated memcpy |
| [Hooks](hooks.md) | Server/Connection/Request lifecycle hook system |
| [Reflection](reflection.md) | Runtime type introspection, JSON binding |

## App & Middleware

| Document | Focus |
|----------|-------|
| [App Layer](app.md) | `csilk_app_t` facade, bootstrap sequence, route group matching, admin dashboard, workflow engine |
| [Middleware](middleware.md) | Onion model, chain assembly, 16 built-in middleware modules |

## Protocols & Messaging

| Document | Focus |
|----------|-------|
| [Protocols](protocols.md) | WebSocket, SSE, Swagger UI, WebSocket Rooms |
| [Messaging](messaging.md) | Event bus, pub/sub, `uv_async_t` dispatch, WAL persistence |

## Data & AI

| Document | Focus |
|----------|-------|
| [Data](data.md) | DB abstraction, pluggable drivers, connection pool, cJSON results |
| [AI Engine](ai.md) | Unified chat/embeddings, tool calls, streaming |
| [Workflow](workflow.md) | DAG scheduler, hot reload, WAL resume, interactive nodes |
| [Drivers](drivers.md) | AI/Cipher/DB/Perm/Vector DB pluggable driver lifecycle |

## Security & Cryptography

| Document | Focus |
|----------|-------|
| [Security](security.md) | RBAC, JWT, CSRF, CORS, WAF, rate limiter |
| [Crypto](crypto.md) | SHA-256, HMAC, UUID, random number generation |

## Observability

| Document | Focus |
|----------|-------|
| [Metrics](metrics.md) | Prometheus exposition, lock-free counters, latency histograms |

---

## How to Read

1. Start with **Server Core** and **Context** for the foundation.
2. Read **Router** and **Middleware** to understand how requests flow.
3. Choose deeper docs based on your focus area (data, AI, security, protocols).

## Relationship Overview

```mermaid
%%{init: {'theme': 'base', 'themeVariables': {'background': '#2E3440','primaryColor':'#81A1C1','primaryBorderColor':'#4C566A','primaryTextColor':'#ECEFF4','secondaryColor':'#3B4252','secondaryBorderColor':'#434C5E','secondaryTextColor':'#D8DEE9','lineColor':'#81A1C1','textColor':'#ECEFF4','mainBkg':'#3B4252','nodeBorder':'#4C566A','clusterBkg':'#2E3440','clusterBorder':'#4C566A','titleColor':'#ECEFF4','edgeLabelBackground':'#3B4252','nodeTextColor':'#ECEFF4'}, 'flowchart': {'htmlLabels': true, 'curve': 'basis'}}}%%
graph TB
    APP["fa:fa-cube App Layer"] --> SRV["fa:fa-server Server Core"]
    APP --> RTR["fa:fa-sitemap Router (Radix)"]
    APP --> MW["fa:fa-shield Middleware Chain"]

    SRV --> TLS["fa:fa-lock TLS/ALPN"]
    SRV --> H2["fa:fa-code-fork HTTP/2 (h2)"]
    SRV --> MQ["fa:fa-envelope MQ (Bus)"]

    MW --> SEC["fa:fa-shield Security (RBAC/JWT)"]
    MW --> PROT["fa:fa-plug Protocols (WS/SSE)"]

    SRV --> DATA["fa:fa-database Data (DB)"]
    SRV --> AI_MOD["fa:fa-robot AI (Engine)"]
    SRV --> CRYPTO["fa:fa-key Crypto (Cipher)"]
    SRV --> METRICS["fa:fa-dashboard Metrics (Prom)"]
```

See also: [Architecture Overview](../architecture.md), [Getting Started](../getting-started.md).
