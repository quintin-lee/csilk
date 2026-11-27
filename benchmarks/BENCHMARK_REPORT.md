# csilk Performance Benchmark Report

> **Date**: 2026-11-27
> **Version**: 1.2.0 (`15ce49eb`)
> **Hardware**: 13th Gen Intel(R) Core(TM) i9-13900HX (32 logical cores), Linux `6.12.108-1-MANJARO` (`x86_64`)
> **Compiler / Profile**: GCC 16.2.1 (`-DCMAKE_BUILD_TYPE=Release -DCSILK_ENABLE_NATIVE_ARCH=ON`)
> **Raw Artifact**: [`benchmarks/results/builtin-latest.json`](results/builtin-latest.json)

## Methodology

- **Reproducible Built-in Suite**: Executed via [`scripts/run_microbenchmarks.py`](../scripts/run_microbenchmarks.py) (or `./scripts/run_benchmarks.sh --builtin`) sequentially to avoid cross-process CPU contention.
- **Backends Tested**: Default `libuv` and native Linux `-DCSILK_USE_URING=ON`.
- **External `wrk` Suite**: Preserved via `./scripts/run_benchmarks.sh --save` (executed in `.github/workflows/bench.yml` on `ubuntu-24.04` runners where `wrk` is installed).

## v1.2.0 Reproducible Measured Results

### 1. Real TCP HTTP/1.1 End-to-End (`test_http1_e2e_bench`, 4 keep-alive clients × 5 rounds)

| Backend | Workers | Requests | Throughput (QPS ± 95% CI) | P50 Latency | P95 Latency | P99 Latency | Errors |
|:---|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| `libuv` | 1 | 5,000 | 113,799.03 ± 4,316.76 | 23.67 us | 48.16 us | 107.92 us | 0 |
| `libuv` | 2 | 5,000 | 190,900.41 ± 14,903.67 | 13.76 us | 32.58 us | 33.47 us | 0 |
| `libuv` | 4 | 5,000 | 205,350.62 ± 8,206.57 | 11.40 us | 19.21 us | 26.04 us | 0 |
| `libuv` | 8 | 5,000 | 247,128.69 ± 29,041.52 | 10.49 us | 22.35 us | 45.74 us | 0 |
| `io_uring` | 1 | 5,000 | 83,840.55 ± 2,303.08 | 46.40 us | 50.46 us | 60.11 us | 0 |
| `io_uring` | 2 | 5,000 | 143,539.69 ± 25,427.97 | 30.23 us | 42.85 us | 45.64 us | 0 |
| `io_uring` | 4 | 5,000 | 208,952.93 ± 29,755.37 | 12.67 us | 32.29 us | 36.42 us | 0 |
| `io_uring` | 8 | 5,000 | 247,929.56 ± 17,409.32 | 12.88 us | 16.87 us | 25.59 us | 0 |

### 2. Linux Native `IORING_OP_RECV` Path (`test_uring_recv_bench`, `CSILK_USE_URING=ON`)

| Scenario | Requests / Conns | Throughput (req/s) | P50 | P95 | P99 |
|:---|:---:|:---:|:---:|:---:|:---:|
| Hello World (Small) | 1,000 / 1 | 52,962.09 | 19.0 us | 20.0 us | 116.4 us |
| Hello World (KeepAlive) | 2,000 / 4 | 215,906.97 | 16.8 us | 18.7 us | 26.8 us |
| 1KB Payload Response | 2,000 / 4 | 293,669.93 | 11.8 us | 14.3 us | 25.1 us |
| 10K Concurrent Batch | 4,000 / 8 | 184,280.35 | 40.8 us | 55.3 us | 59.8 us |
| High-Load KeepAlive (5K) | 4,992 / 16 | 239,225.19 | 63.7 us | 67.8 us | 86.1 us |

### 3. Core Subsystem Microbenchmarks (`libuv` Release + `-march=native`)

| Subsystem | Workload | Measured Result |
|:---|:---|:---|
| HTTP/1.1 Parse + Route + Serialize (`test_http1_pipeline_bench`) | `parse_tiny` / `router_match` / `response_serialize` | 105.81 ns / 39.76 ns / 32.52 ns per op |
| HTTP/2 Stream Hash Map (`test_h2_stream_bench`) | Pool cycle / 10–10,000 stream lookup | 34.71 ns/cycle / 4.22–8.66 ns/lookup (median) |
| Small-Vector Router Trie (`test_router_bench`) | 100 / 1,000 / 10,000 routes | 230.61 ns / 308.88 ns / 248.47 ns per lookup |
| Bump-Pointer Arena (`test_arena_bench`) | 8 B / 32 B / 128 B / 1024 B alloc | 1.83 ns / 1.68 ns / 2.11 ns / 4.39 ns (596.8 M ops/s peak) |
| Interned Header Map (`test_header_map_bench`) | Interned ID vs Legacy `strcasecmp` | 11.96 ns/req vs 66.96 ns/req (5.60× faster) |
| JSON Value Accessor (`test_json_accessor_bench`) | Value Object / Pointer View | 15.01 ns (66.6 M ops/s) / 15.04 ns (66.5 M ops/s) |
| Lock-Free Dispatch (`test_dispatch_bench`) | 1 / 8 / 32 / 128 producers | 4.86M / 11.80M / 12.38M / 11.29M dispatch ops/s |
| Concurrent Server Stats (`test_server_stats_bench`) | 8 workers, 4 reader threads | 2.35 M queries/sec (1,702.17 ns/query) |

## Reproducing Locally

```bash
# Built-in Release + -march=native benchmark suite (produces benchmarks/results/builtin-latest.json)
./scripts/run_benchmarks.sh --builtin

# Or compare both libuv and native io_uring Release builds directly:
python3 scripts/run_microbenchmarks.py \
  --uv-build-dir /tmp/bench_uv \
  --uring-build-dir /tmp/bench_uring \
  --output benchmarks/results/builtin-latest.json

# External wrk load test suite (requires wrk installed):
./scripts/run_benchmarks.sh --save
```

> Historical note: Earlier `v0.5.0`/`v1.0.0-rc` `wrk` runs are archived in [`benchmarks/results/*.txt`](results/) and [`benchmarks/COMPARATIVE_BENCHMARK_WHITEPAPER.md`](COMPARATIVE_BENCHMARK_WHITEPAPER.md) for reference.

## Further Reading

| Document | Content |
|:---------|:--------|
| [Benchmarking Guide](README.md) | Built-in and `wrk`/`wrk2` suite usage, scenario tuning, bottleneck diagnosis |
| [Performance Tuning Guide](../docs/en/performance-tuning.md) | Compiler flags, kernel tuning, PGO |
| [Architecture Whitepaper](../docs/en/architecture.md) | Zero-copy, lock-free design |
