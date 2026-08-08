# eBPF XDP Dynamic WAF & OpenTelemetry APM Dashboard Design Specification

## Overview

This specification defines the architecture, data structures, BPF-Map zero-downtime rule hot-reloading, OpenTelemetry tracing span collection, and embedded APM Web Dashboard UI for **eBPF XDP Dynamic WAF & OTLP Tracing Subsystem** in `csilk` (server-c).

The system enables kernel-level network packet filtering (`XDP_DROP`) with microsecond rule updates, alongside full-stack distributed execution tracing for Workflows, Raft RPCs, WASM sandboxes, and HTTP APIs with an embedded browser-based Gantt waterfall dashboard (`/admin/apm`).

---

## 1. System Architecture & Module Boundaries

### 1.1 Directory Structure

```
include/csilk/
  └── middleware/
      ├── xdp_waf.h            # eBPF XDP WAF public header
      └── otlp_trace.h         # OpenTelemetry tracing & APM UI header

src/
  └── middleware/
      ├── xdp_waf.c            # BPF-Map updates, rule reloading & userspace fallback
      └── otlp_trace.c         # Span ring buffer, telemetry JSON API & APM routes

share/
  └── csilk/
      └── apm_ui.html          # Embedded APM single-page Web Dashboard UI
```

### 1.2 Performance & Safety Guarantees

1. **Zero-Downtime Rule Reloading**: Rules are updated directly via `bpf_map_update_elem` syscalls on `BPF_MAP_TYPE_LPM_TRIE` and `BPF_MAP_TYPE_HASH` maps without un-attaching the XDP netlink driver hook.
2. **Low-Overhead Telemetry**: Monotonic nanosecond timestamp collection (`clock_gettime(CLOCK_MONOTONIC)`) into an in-memory 2048-span ring buffer with $< 10 \text{ ns}$ profiling overhead.
3. **Userspace Graceful Fallback**: Automatically degrades to a Radix-Tree userspace WAF match when running without Linux `CAP_BPF` / `CAP_NET_ADMIN` privileges.

---

## 2. eBPF XDP BPF-Map Layout (`xdp_waf.c`)

### 2.1 Rule Types & BPF-Map Structs

```c
typedef struct {
    uint32_t ip;
    uint32_t prefix_len;
    uint8_t  action; /* 0: PASS, 1: DROP, 2: RATELIMIT */
} csilk_xdp_ip_rule_t;

struct csilk_xdp_waf_s {
    char          ifname[16];
    int           bpf_map_fd;
    int           is_kernel_attached;
    csilk_mutex_t mutex;
};
```

---

## 3. OpenTelemetry Tracing & Ring Buffer (`otlp_trace.c`)

### 3.1 OTLP Span Data Model

```c
typedef struct csilk_otlp_span_s {
    char     trace_id[33];       /* 16-byte hex string */
    char     span_id[17];        /* 8-byte hex string */
    char     parent_span_id[17]; /* Parent Span ID */
    char     name[64];           /* Operation name */
    uint64_t start_time_ns;      /* Start timestamp ns */
    uint64_t end_time_ns;        /* End timestamp ns */
    double   duration_ms;        /* Total duration ms */
    int      status_code;        /* 0: UNSET, 1: OK, 2: ERROR */
    char     attributes_json[256];
} csilk_otlp_span_t;

typedef struct {
    csilk_otlp_span_t spans[2048];
    size_t            head;
    size_t            count;
    csilk_mutex_t     mutex;
} csilk_otlp_span_buffer_t;
```

---

## 4. Public API Contracts

### 4.1 `include/csilk/middleware/xdp_waf.h`

```c
#ifndef CSILK_XDP_WAF_H
#define CSILK_XDP_WAF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct csilk_xdp_waf_s csilk_xdp_waf_t;

/**
 * @brief Creates and binds an eBPF XDP WAF instance to a network interface.
 */
csilk_xdp_waf_t* csilk_xdp_waf_new(const char* ifname);

/**
 * @brief Dynamically adds an IP/CIDR rule with zero downtime.
 */
int csilk_xdp_waf_add_ip_rule(csilk_xdp_waf_t* waf, const char* ip_cidr, uint8_t action);

/**
 * @brief Frees eBPF XDP WAF instance.
 */
void csilk_xdp_waf_free(csilk_xdp_waf_t* waf);

#ifdef __cplusplus
}
#endif
#endif /* CSILK_XDP_WAF_H */
```

### 4.2 `include/csilk/middleware/otlp_trace.h`

```c
#ifndef CSILK_OTLP_TRACE_H
#define CSILK_OTLP_TRACE_H

#include <stddef.h>
#include <stdint.h>
#include "csilk/app/app.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct csilk_otlp_span_s csilk_otlp_span_t;

/**
 * @brief Starts a new OpenTelemetry Trace Span.
 */
csilk_otlp_span_t* csilk_otlp_tracer_start_span(const char* name, const char* parent_span_id);

/**
 * @brief Ends the span and pushes it to the ring buffer.
 */
void csilk_otlp_tracer_end_span(csilk_otlp_span_t* span, int status_code);

/**
 * @brief Registers the embedded APM Web Dashboard UI routes.
 */
void csilk_otlp_serve_apm_ui(csilk_app_t* app, const char* path);

#ifdef __cplusplus
}
#endif
#endif /* CSILK_OTLP_TRACE_H */
```

---

## 5. Embedded APM Dashboard Web Single-Page App (`share/csilk/apm_ui.html`)

Serves interactive Gantt chart waterfall timelines and QPS/Latency metrics via `/admin/apm` and `/admin/api/telemetry/spans`.

---

## 6. Test Plan

1. **`test_xdp_waf_rules.c`**: Test IP/CIDR string parsing, BPF-Map entry updates, and rule reloading.
2. **`test_otlp_trace_span.c`**: Test Span start/end duration timing, parent-child ID hierarchy, and 2048-span ring buffer overflow handling.
3. **`test_apm_dashboard_route.c`**: Test HTTP GET `/admin/apm` and `/admin/api/telemetry/spans` JSON output.
