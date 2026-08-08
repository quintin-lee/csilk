# eBPF XDP Dynamic WAF & OTLP APM Dashboard Implementation Plan

Implementation plan for building the native eBPF XDP Dynamic WAF and OpenTelemetry APM Dashboard in `csilk`.

## User Review Required

> [!IMPORTANT]
> All code changes must follow C23 standard, keep file sizes under 700 lines, achieve 0 clang-tidy warnings (`make tidy`), and pass 100% of unit/integration tests.

## Proposed Changes

### Core Subsystem: eBPF WAF & OTLP APM (`src/middleware/`)

- Public API headers: `include/csilk/middleware/xdp_waf.h` & `include/csilk/middleware/otlp_trace.h`
- Internal structs & BPF-Maps: `src/middleware/xdp_waf_internal.h`
- eBPF XDP BPF-Map engine: `src/middleware/xdp_waf.c`
- OTLP Span ring buffer & tracer: `src/middleware/otlp_trace.c`
- Embedded APM Dashboard Web SPA: `share/csilk/apm_ui.html`

---

## Execution Plan

### Task 1: Public and Internal Header Files
- Create `include/csilk/middleware/xdp_waf.h` and `include/csilk/middleware/otlp_trace.h` with public function prototypes.
- Create `src/middleware/xdp_waf_internal.h` defining BPF-Map structures and rule action types.

### Task 2: eBPF XDP BPF-Map Dynamic Rule Engine
- Implement BPF-Map syscall wrapper, IP CIDR parser, zero-downtime rule reloading, and userspace fallback in `src/middleware/xdp_waf.c`.
- Create `tests/middleware/test_xdp_waf_rules.c` testing IP/CIDR rule insertion, deletion, and reloading.

### Task 3: OpenTelemetry Span Sampler & Ring Buffer
- Implement nanosecond timestamp duration tracer, parent-child Span hierarchy, and 2048-span ring buffer in `src/middleware/otlp_trace.c`.
- Create `tests/middleware/test_otlp_trace_span.c` verifying Span timing, ring buffer overflow protection, and JSON serialization.

### Task 4: Embedded APM Single-Page Dashboard & Telemetry API Routes
- Create responsive HTML5/JS APM Dashboard SPA in `share/csilk/apm_ui.html`.
- Implement `csilk_otlp_serve_apm_ui` and `GET /admin/api/telemetry/spans` route handlers in `src/middleware/otlp_trace.c`.
- Create `tests/middleware/test_apm_dashboard_route.c` testing HTTP API telemetry routes.

### Task 5: CMake Registration & Quality Assurance
- Update `cmake/sources.cmake` and `cmake/tests.cmake`.
- Run `make format`, `make check-format`, `make tidy`, and `ctest --output-on-failure`.

---

## Verification Plan

```bash
cd build
make format
make check-format
make tidy
make -j4
ctest --output-on-failure
```
