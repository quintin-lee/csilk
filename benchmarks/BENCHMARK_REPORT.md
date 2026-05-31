# Csilk Performance Benchmark Report

> **Date**: 2026-05-31
> **Version**: 0.3.0 (Release mode)
> **Hardware**: Manjaro Linux, x86_64 (4 workers)

## Methodology

- **Tool**: [wrk](https://github.com/wg/wrk) — modern HTTP benchmarking tool
- **Duration**: 5 seconds per test (after warmup)
- **Concurrency**: t=threads, c=connections
- **Server**: `example_server` with 4 worker threads
- **Build**: `-DCMAKE_BUILD_TYPE=Release`

## Results

### 1. Static File Serving (zero-copy sendfile)

| Test | Threads/Conns | Requests/sec | Latency |
|------|:------------:|:-----------:|:-------:|
| `small.html` (31B) | 2 / 10 | **107,232** | 134 µs |
| `small.html` (31B) | 4 / 50 | **139,559** | 446 µs |
| `small.html` (31B) | 4 / 100 | **147,871** | 783 µs |
| `1k.bin` (1KB) | 4 / 100 | **138,176** | 816 µs |

### 2. JSON Response (cJSON serialization)

| Test | Threads/Conns | Requests/sec | Latency |
|------|:------------:|:-----------:|:-------:|
| `/api/data` | 2 / 10 | **94,810** | 113 µs |
| `/api/data` | 4 / 50 | **124,807** | 456 µs |
| `/api/data` | 4 / 100 | **122,895** | 890 µs |

### 3. Plain Text Response

| Test | Threads/Conns | Requests/sec | Latency |
|------|:------------:|:-----------:|:-------:|
| `GET /` ("Hello, World!") | 2 / 10 | **105,875** | 89 µs |
| `GET /` | 4 / 50 | **130,043** | 486 µs |
| `GET /` | 4 / 100 | **146,604** | 742 µs |

### 4. Radix Tree Routing (parameter matching)

| Test | Threads/Conns | Requests/sec | Latency |
|------|:------------:|:-----------:|:-------:|
| `GET /user/42` (`:id` param) | 4 / 100 | **132,631** | 842 µs |

### 5. HTTP Pipelining

| Test | Threads/Conns | Requests/sec | Latency |
|------|:------------:|:-----------:|:-------:|
| `GET /` (pipeline depth 16) | 4 / 100 | **136,120** | 802 µs |

### 6. OpenAPI Spec (large JSON)

| Test | Threads/Conns | Requests/sec | Latency |
|------|:------------:|:-----------:|:-------:|
| `GET /openapi.json` (~400KB) | 4 / 100 | **16,152** | 6.25 ms |

## Analysis

| Benchmark | Peak RPS | Notes |
|-----------|:-------:|-------|
| Static file (sendfile) | **147,871** | Zero-copy I/O, minimal CPU overhead |
| Plain text | **146,604** | Pure framework overhead |
| Radix routing | **132,631** | Param matching adds <10% overhead |
| Pipeline | **136,120** | Negligible difference from keep-alive |
| JSON (cJSON) | **124,807** | Serialization + arena allocation |
| Static 1KB | **138,176** | Larger payload, still sendfile-accelerated |
| OpenAPI spec | **16,152** | ~400KB response, bandwidth-bound |

## Running

```bash
# Run all benchmarks, print summary
bash scripts/run_benchmarks.sh

# Run and save results for later comparison
bash scripts/run_benchmarks.sh --save

# Compare against last saved result
bash scripts/run_benchmarks.sh --compare

# CI mode: save, compare, fail on >10% regression
bash scripts/run_benchmarks.sh --ci

# Custom duration
BENCH_DURATION=10s bash scripts/run_benchmarks.sh --save
```

Raw JSON results are saved to `benchmarks/results/`.
CI runs `--save` on every main push, uploads results as an artifact,
and compares against the previous run (non-blocking).
