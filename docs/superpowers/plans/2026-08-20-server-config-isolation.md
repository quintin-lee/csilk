# Server Configuration Isolation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Isolate startup-only configuration from runtime-mutable configuration using `csilk_runtime_config_t` with relaxed atomic accessors to eliminate data races and word tearing during dynamic config updates.

---

### Task 1: Add `csilk_runtime_config_t` and Hot-Path Accessors

**Files:**
- Modify: `src/core/internal/srv_internal.h`
- Modify: `src/core/internal/srv_impl.h`
- Modify: `src/core/server/server_lifecycle.c`

- [ ] **Step 1: Define `csilk_runtime_config_t` in `src/core/internal/srv_internal.h` and embed in `csilk_server_t`**

- [ ] **Step 2: Add inline fast accessors in `src/core/internal/srv_impl.h`**

- [ ] **Step 3: Update `csilk_server_set_config()`, `csilk_server_new()`, `csilk_server_set_max_connections()`, `csilk_server_check_backpressure()` in `src/core/server/server_lifecycle.c`**

- [ ] **Step 4: Commit Task 1**
```bash
git add src/core/internal/srv_internal.h src/core/internal/srv_impl.h src/core/server/server_lifecycle.c
git commit -m "feat(server): ✨ add atomic runtime config and thread-safe publishing"
```

---

### Task 2: Migrate Request Hot-Paths to Use Fast Runtime Config Accessors

**Files:**
- Modify: `src/core/http/http1_parse.c`
- Modify: `src/core/http/http1_pipeline.c`
- Modify: `src/core/http/http1_write.c`
- Modify: `src/core/http/h2_callbacks.c`
- Modify: `src/core/http/h2_response.c`
- Modify: `src/core/server/connection_io.c`
- Modify: `src/core/primitives/router_trie.c`

- [ ] **Step 1: Replace raw `server->config` reads in HTTP/1 and HTTP/2 parsers & pipeline with fast atomic accessors**

- [ ] **Step 2: Replace raw `server->config` reads in `connection_io.c` and `router_trie.c` with fast atomic accessors**

- [ ] **Step 3: Run unit tests via `ctest`**

- [ ] **Step 4: Commit Task 2**
```bash
git add src/core/http/ src/core/server/connection_io.c src/core/primitives/router_trie.c
git commit -m "refactor(server): ♻️ migrate hot request paths to atomic runtime config accessors"
```

---

### Task 3: TSAN Verification, Benchmark & Formatting

**Files:**
- Create: `tests/core/test_server_config_race.c`
- Modify: `cmake/tests.cmake`

- [ ] **Step 1: Write `tests/core/test_server_config_race.c` testing high-concurrency dynamic config updates while reading**

- [ ] **Step 2: Register test in `cmake/tests.cmake` and verify under TSAN**

- [ ] **Step 3: Run full CTest suite and code formatting**

- [ ] **Step 4: Final commit**
