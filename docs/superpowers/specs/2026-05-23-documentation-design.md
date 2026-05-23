# Documentation Design Specification - csilk Project

This document outlines the proposed structure and content plan for the `docs/` directory of the `csilk` project.

## 1. Overview
The goal is to provide a holistic, user-friendly documentation set that complements the existing Doxygen-based API reference. It aims to improve onboarding for new developers and provide a comprehensive guide for existing users.

## 2. Proposed Structure
- `docs/index.md`: Introduction and high-level project summary.
- `docs/getting-started.md`: Quick-start guide (installation, building, running example server).
- `docs/architecture.md`: Architectural design (libuv, lifecycle, memory management).
- `docs/module-design/`: Detailed module design docs.
    - `router.md`: Radix tree details.
    - `middleware.md`: Middleware system overview.
    - `context.md`: Context handling.
    - `reflection.md`: Reflection engine details.
- `docs/user-manual/`: Practical guides.
    - `configuration.md`: YAML config details.
    - `middleware-dev.md`: Writing custom middleware.
    - `advanced-usage.md`: Route groups, WebSocket/SSE.

## 3. Implementation Phases
1. **Foundation**: Create index, getting-started, and architecture docs.
2. **Module Design**: Populate `module-design/` content.
3. **Manual**: Populate `user-manual/` content.

## 4. Maintenance
- Documentation will be reviewed as part of the PR process.
- Doxygen remains the primary source for API references.
