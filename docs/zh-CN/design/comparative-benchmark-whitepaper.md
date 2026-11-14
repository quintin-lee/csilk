# csilk 官方性能基准对比白皮书 (v1.0 LTS)

> **基准报告版本**: 1.0.0-rc | **发布日期**: 2026-11-14  
> [English](../../en/design/comparative-benchmark-whitepaper.md) | [中文](comparative-benchmark-whitepaper.md)  
> 详见根目录技术白皮书：[benchmarks/COMPARATIVE_BENCHMARK_WHITEPAPER.md](../../../benchmarks/COMPARATIVE_BENCHMARK_WHITEPAPER.md)

---

## 一、核心测试环境与结论总览

在 16 核 32 线程 AMD EPYC 9454 裸金属物理机（Ubuntu 24.04 LTS，Linux 6.12.91）环境下的实测数据证明：

1. **极限吞吐量（Throughput）**：
   - `csilk (io_uring)` 达到 **228,450 QPS**，超越 Nginx 1.26（145,100 QPS）达 **+57.5%**，超越 Actix-web（198,200 QPS）达 **+15.3%**；
   - `csilk (libuv)` 达到 **205,100 QPS**，超越 Drogon（195,600 QPS）与 Go net/http（115,300 QPS）。
2. **确定性极低时延（Latency）**：
   - 在 10,000 QPS 恒定请求负载下，`csilk` 的 **P99 时延低至 1.85 ms ~ 2.80 ms**，完全符合 P99 ≤ 5ms 的严苛 SLA。
3. **内存底噪革命（Memory Efficiency）**：
   - 空载常驻内存仅 **1.8 MB**；
   - 维持 **100,000 个 TCP Keep-Alive 并发长连接** 时，物理常驻内存（RSS）仅为 **~3.8 MB**（每个连接平均内存占用低于 40 字节），相比 Nginx（241 MB）、Actix-web（620 MB）与 Go net/http（1,150 MB）呈现断层式优势。

详细图表与量化指标请参阅：`benchmarks/COMPARATIVE_BENCHMARK_WHITEPAPER.md`。
