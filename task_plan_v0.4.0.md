# 任务规划：Csilk v0.4.0 生产级加固与性能基准

## 目标
将 Csilk 提升至真正的生产可用状态，实现 HTTPS 支持，建立性能基准，并增强可观测性。

## 阶段
- [x] 阶段 1：HTTPS/TLS 深度集成 (OpenSSL) `id: phase_tls`
- [x] 阶段 2：可观测性增强 (Prometheus Metrics) `id: phase_obs`
- [ ] 阶段 3：性能基准测试集 (Benchmark Suite) `id: phase_bench`
- [ ] 阶段 4：高级 I/O 优化 (HTTP/2 & Zero-copy) `id: phase_advanced_io`

## 详细任务

### 阶段 1：HTTPS/TLS 深度集成
- [x] 调研并集成 OpenSSL 或 mbedTLS 依赖 `id: task_tls_deps`
- [x] 实现基于 libuv `uv_link_t` 或手动加密流转换的 TLS 握手 `id: task_tls_handshake`
- [x] 在 `csilk_server_config_t` 中添加证书与私钥配置 `id: task_tls_config`
- [x] 编写 HTTPS 示例及自动化集成测试 `id: task_tls_test`

### 阶段 2：可观测性增强
- [x] 实现 Prometheus Metrics 中间件，统计 QPS、响应时长分布及状态码 `id: task_metrics_mw`
- [x] 提供 `/metrics` 标准爬取端点 `id: task_metrics_endpoint`
- [x] 优化结构化日志，支持自动关联 Request ID 与上下文 `id: task_log_correlation`

### 阶段 3：性能基准测试集
- [ ] 建立基于 `wrk` 或 `hey` 的自动化压测脚本 `id: task_bench_scripts`
- [ ] 编写不同负载下的性能报告（静态文件、JSON、Radix 路由深度匹配） `id: task_bench_report`
- [ ] 对比 Gin, Fiber, Fastify 等框架的性能指标 `id: task_bench_compare`

### 阶段 4：高级 I/O 优化
- [ ] 评估并原型化 HTTP/2 (基于 nghttp2) 支持 `id: task_h2_proto`
- [ ] 在支持的平台上实现 `sendfile` 零拷贝静态文件传输 `id: task_zero_copy`
- [ ] 优化 Header 哈希表冲突率及 Arena 复用效率 `id: task_io_perf_tune`

## 状态
- 当前阶段：`phase_obs` (计划中)
- 状态：`in_progress`
