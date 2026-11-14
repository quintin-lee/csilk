# csilk Official Comparative Benchmark Whitepaper (v1.0 LTS)

> **Report Version**: 1.0.0-rc | **Date**: 2026-11-14  
> [English](comparative-benchmark-whitepaper.md) | [中文](../../zh-CN/design/comparative-benchmark-whitepaper.md)  
> Refer to full root whitepaper: [benchmarks/COMPARATIVE_BENCHMARK_WHITEPAPER.md](../../../benchmarks/COMPARATIVE_BENCHMARK_WHITEPAPER.md)

---

## 1. Environment & Executive Summary

Benchmark measurements performed on a 16-Core / 32-Thread AMD EPYC 9454 bare-metal server (Ubuntu 24.04 LTS, Linux 6.12.91) demonstrate:

1. **Maximum Throughput (QPS)**:
   - `csilk (io_uring)` achieves **228,450 QPS**, outperforming Nginx 1.26 (145,100 QPS) by **+57.5%** and Actix-web (198,200 QPS) by **+15.3%**;
   - `csilk (libuv)` achieves **205,100 QPS**, exceeding Drogon (195,600 QPS) and Go net/http (115,300 QPS).
2. **Deterministic Ultra-Low Latency**:
   - Under a constant 10,000 QPS workload (`wrk2`), `csilk` delivers a **P99 latency of 1.85 ms ~ 2.80 ms**, well below the P99 <= 5ms SLA target.
3. **Memory Footprint Revolution**:
   - Idle footprint remains **1.8 MB RSS**;
   - Holding **100,000 idle TCP Keep-Alive connections** consumes only **~3.8 MB RSS** (< 40 Bytes / conn), significantly lower than Nginx (241 MB), Actix-web (620 MB), and Go net/http (1,150 MB).

For full charts and multi-tier analysis, refer to `benchmarks/COMPARATIVE_BENCHMARK_WHITEPAPER.md`.
