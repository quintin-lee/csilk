# csilk Benchmarking Guide

> **Last updated**: 2026-06-29

This directory contains scripts and guidance for performance benchmarking the csilk web framework. Baseline targets: **P99 latency ≤ 5ms under 10K QPS**, **~50K QPS single worker (4-core)**, **linear scaling to ~200K QPS (16-core multi-worker)**.

---

## Prerequisites

| Tool | Install | Purpose |
|:-----|:--------|:--------|
| `wrk` | `sudo apt install wrk` | HTTP benchmarking |
| `wrk2` | [github.com/giltene/wrk2](https://github.com/giltene/wrk2) | Latency-focused benchmarking |
| `perf` | `sudo apt install linux-tools-common` | CPU profiling |
| `flamegraph.pl` | [github.com/brendangregg/FlameGraph](https://github.com/brendangregg/FlameGraph) | Flame graph generation |

Build csilk in Release mode:

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON
make -C build -j$(nproc)
```

---

## Quick Start

```bash
# Terminal 1: Start the example server
./build/examples/example_server

# Terminal 2: Run the automated benchmark suite
./scripts/run_benchmarks.sh

# Or run a single wrk test
wrk -t4 -c100 -d30s http://localhost:8080/json
```

---

## Benchmark Targets

The example server provides these endpoints for benchmarking:

| Endpoint | Tests | Expected P99 |
|:---------|:------|:-------------|
| `GET /json` | JSON serialization overhead | ≤1ms |
| `GET /static/test.html` | Zero-copy static file serving | ≤1ms |
| `GET /api/v1/users/profile` | Radix tree routing (deeply nested) | ≤0.5ms |
| `GET /delay/50` | Connection under simulated latency | ≤55ms |

---

## Using `wrk`

### Basic Syntax

```bash
wrk <options> <url>
```

### Common Options

| Option | Example | Description |
|:-------|:--------|:------------|
| `-t` | `-t4` | Number of threads |
| `-c` | `-c100` | Number of concurrent connections |
| `-d` | `-d30s` | Test duration |
| `-R` | `-R10000` | Request rate (wrk2 only) |
| `-H` | `-H "Connection: keep-alive"` | Custom header |

### Single Worker (Baseline)

```bash
# 4 threads, 100 connections, 30 seconds
wrk -t4 -c100 -d30s http://localhost:8080/json
```

Expected output:

```
Running 30s test @ http://localhost:8080/json
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     1.20ms    0.45ms  12.00ms   87.50%
    Req/Sec    12.50k     1.20k    15.80k    70.00%
  Latency Distribution (P99: 4.50ms)
  1498000 requests in 30.00s, 245.00MB read
Requests/sec:  49933.33
Transfer/sec:      8.17MB
```

### Multi-Worker (16-core)

Configure `worker_threads: 16` in `config.yaml`, then:

```bash
wrk -t16 -c200 -d30s http://localhost:8080/json
```

Expected throughput: **~200K QPS** with P99 ≤ 8ms.

### Keep-Alive vs No Keep-Alive

```bash
# Keep-alive (default — measures connection reuse)
wrk -t4 -c100 -d30s http://localhost:8080/json

# No keep-alive (measures connection setup overhead)
wrk -t4 -c100 -d30s -H "Connection: close" http://localhost:8080/json
```

---

## Using `wrk2` (Latency-Focused)

`wrk2` provides precise latency distribution at fixed request rates:

```bash
# 10K QPS for 60 seconds, measure P99 latency
wrk2 -t4 -c100 -d60s -R10000 --latency http://localhost:8080/json
```

Expected:

```
  Latency Distribution (P99: 4.20ms)
  Requests/sec:  10000.00
```

Gradually increase `-R` until P99 exceeds 5ms to find the saturation point.

---

## Benchmark Scenarios

### 1. Throughput Test

Maximize requests per second:

```bash
# Find max QPS with increasing concurrency
for c in 10 50 100 200 500; do
    echo "=== Connections: $c ==="
    wrk -t4 -c$c -d30s http://localhost:8080/json
done
```

### 2. Latency Under Load

```bash
# wrk2: measure P99 at fixed rates
for rate in 5000 10000 15000 20000 25000; do
    echo "=== Rate: $rate ==="
    wrk2 -t4 -c100 -d30s -R$rate --latency http://localhost:8080/json
done
```

### 3. Routing Benchmark

```bash
# Deeply nested route vs flat route
wrk -t4 -c100 -d30s http://localhost:8080/api/v1/users/profile
wrk -t4 -c100 -d30s http://localhost:8080/json
```

### 4. Static File Serving

```bash
# Small file
wrk -t4 -c100 -d30s http://localhost:8080/static/test.html

# (Optional) Large file — create a 1MB test file first
# dd if=/dev/urandom of=static/large.bin bs=1M count=1
# wrk -t4 -c100 -d30s http://localhost:8080/static/large.bin
```

---

## Profiling with `perf`

```bash
# Run server with profiling
perf record -g -p $(pgrep example_server) -- sleep 30

# Generate flame graph
perf script | ./stackcollapse-perf.pl | ./flamegraph.pl > csilk.svg
```

Or use the built-in admin dashboard flame graph:

```bash
# Enable admin dashboard, then visit:
# http://localhost:8080/admin/ (CPU Flame Graph panel)
```

---

## Interpreting Results

### Key Metrics

| Metric | Good | Target | Concern |
|:-------|:----:|:------:|:--------|
| Requests/sec | >40K | 50K+ | <30K |
| P99 Latency | <3ms | ≤5ms | >10ms |
| Avg Latency | <1ms | ≤2ms | >5ms |
| Error Rate | 0% | 0% | >0.1% |
| Transfer/sec | >6MB | 8MB+ | <4MB |

### Bottleneck Diagnosis

| Symptom | Likely Cause | Fix |
|:--------|:-------------|:----|
| Low QPS, normal latency | Insufficient concurrency | Increase `-c` |
| High latency, low QPS | CPU saturation | Check CPU affinity, worker count |
| Latency spikes | GC/alloc pressure | Check Arena usage, avoid `malloc` |
| Connection errors | File descriptor limit | `ulimit -n 1048576` |
| Uneven worker load | No SO_REUSEPORT affinity | Set `enable_cpu_affinity = 1` |

---

## Recording Results

When reporting benchmark results, include:

```yaml
date: 2026-06-29
csilk_version: 0.3.0
compiler: GCC 13.2
cmake_flags: -DCMAKE_BUILD_TYPE=Release -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON
kernel: Linux 6.12.91
cpu: AMD EPYC 9454 48C/96T
cpu_cores_used: 4 (single worker)
ram: 64GB
worker_threads: 4
wrk_command: wrk -t4 -c100 -d30s http://localhost:8080/json
results:
  requests_per_sec: 49933
  p99_latency_ms: 4.50
  avg_latency_ms: 1.20
  transfer_mbps: 8.17
```

---

## Automated Benchmark Suite

```bash
./scripts/run_benchmarks.sh       # Run all benchmarks
./scripts/run_benchmarks.sh --save # Save results to benchmarks/results/
```

Results are saved as JSON in `benchmarks/results/` for comparison tracking.

---

## Further Reading

| Document | Content |
|:---------|:--------|
| [Performance Tuning Guide](../docs/performance-tuning.md) | Compiler flags, kernel tuning, PGO |
| [Server Core Design](../docs/module-design/server.md) | Multi-worker internals |
| [compare_benchmarks.py](../scripts/compare_benchmarks.py) | Result comparison script |
| [run_benchmarks.sh](../scripts/run_benchmarks.sh) | Automated benchmark runner |
