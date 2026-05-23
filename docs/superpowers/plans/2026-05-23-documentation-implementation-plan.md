# Documentation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Create comprehensive project documentation in the `docs/` folder.

**Architecture:** The documentation set will consist of foundation, module design, and user manual sections, structured to complement the existing Doxygen API references.

**Tech Stack:** Markdown

---

### Task 1: Foundation Documentation

**Files:**
- Create: `docs/index.md`
- Create: `docs/getting-started.md`
- Create: `docs/architecture.md`

- [ ] **Step 1: Create `docs/index.md`**

```markdown
# csilk Documentation

Welcome to the csilk documentation! 

csilk is a lightweight, high-performance HTTP web framework written in C, inspired by Gin (Golang) and built on top of libuv, llhttp, and cJSON.

## Key Resources
- [Getting Started](getting-started.md)
- [Architecture](architecture.md)
- [API Reference](html/index.html) (Requires local Doxygen build)
```

- [ ] **Step 2: Create `docs/getting-started.md`**

```markdown
# Getting Started

## Prerequisites
- CMake 3.11+
- C compiler (C11 support)
- Git
- libyaml-dev

## Build
```bash
git clone ...
mkdir build && cd build
cmake ..
make
```

## Run Example
See `examples/example_server.c`.
```

- [ ] **Step 3: Create `docs/architecture.md`**

```markdown
# Architecture

csilk is built using:
- **libuv**: For async I/O.
- **llhttp**: For parsing HTTP.
- **Arena Allocator**: For memory management.

Lifecycle: Init Router -> Load Config -> Init Server (libuv loop) -> Handle Requests.
```

- [ ] **Step 4: Commit**

```bash
git add docs/index.md docs/getting-started.md docs/architecture.md
git commit -m "docs: add foundation documentation"
```

### Task 2: Module Design Documentation

**Files:**
- Create: `docs/module-design/router.md`
- Create: `docs/module-design/middleware.md`
- Create: `docs/module-design/context.md`
- Create: `docs/module-design/reflection.md`

- [ ] **Step 1: Create `docs/module-design/router.md`**

```markdown
# Router Design

csilk uses a Radix Tree for efficient route matching, supporting static routes, parameters (`:param`), and wildcards (`*wildcard`).
```

- [ ] **Step 2: Create `docs/module-design/middleware.md`**

```markdown
# Middleware Design

Middleware in csilk is designed as a chain of handlers executed in order, supporting both global and group-level application.
```

- [ ] **Step 3: Create `docs/module-design/context.md`**

```markdown
# Context Design

The `csilk_ctx_t` object contains the request state, response buffer, and pointers to the Arena allocator.
```

- [ ] **Step 4: Create `docs/module-design/reflection.md`**

```markdown
# Reflection Engine

The reflection engine binds JSON structures to C structs automatically, leveraging cJSON for parsing and serialization.
```

- [ ] **Step 5: Commit**

```bash
git add docs/module-design/
git commit -m "docs: add module design documentation"
```

### Task 3: User Manual

**Files:**
- Create: `docs/user-manual/configuration.md`
- Create: `docs/user-manual/middleware-dev.md`
- Create: `docs/user-manual/advanced-usage.md`

- [ ] **Step 1: Create `docs/user-manual/configuration.md`**

```markdown
# Configuration

The server supports YAML configuration for defining server settings, logging, and middleware parameters.
```

- [ ] **Step 2: Create `docs/user-manual/middleware-dev.md`**

```markdown
# Custom Middleware Development

To create custom middleware, implement a handler function that takes `csilk_ctx_t*` and calls `csilk_next(c)`.
```

- [ ] **Step 3: Create `docs/user-manual/advanced-usage.md`**

```markdown
# Advanced Usage

Learn how to use Route Groups, WebSockets, and Server-Sent Events (SSE) in your applications.
```

- [ ] **Step 4: Commit**

```bash
git add docs/user-manual/
git commit -m "docs: add user manual documentation"
```
