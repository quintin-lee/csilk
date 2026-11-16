# DPDK 评估结论 — v1.2.0

## 决定：保留 stub，不实现真实 DPDK

### 理由

1. **项目定位不匹配**：csilk 是轻量级、可嵌入的 HTTP 框架（目标 < 2 MB RSS / 10K 连接）。DPDK 需要 hugepages、kernel bypass、root 权限，与嵌入式场景矛盾。

2. **AF_XDP 已覆盖 kernel-bypass 需求**：`src/core/io/xdp_afxdp.c` 提供了 AF_XDP socket 支持，无需 hugepages，兼容标准 Linux 网络栈。

3. **维护成本**：DPDK 依赖 libdpdk（~200 MB），版本更新频繁（每 6 个月一个 LTS），增加 CI 矩阵复杂度。

4. **实际收益有限**：HTTP 工作负载瓶颈通常在 TLS、JSON 解析、应用逻辑，而非 L2/L3 包处理。DPDK 的 ~10 ns/packet 优势对 HTTP 框架几乎无感知。

### 保留内容

- `src/core/io/dpdk_pmd.c`：API stub（`csilk_dpdk_engine_new/free/rx_burst/tx_burst`）
- `include/csilk/core/io/dpdk_pmd.h`：类型定义
- 默认 `CSILK_ENABLE_EXPERIMENTAL_STUBS=1`，提供内存池模拟
- 未来若社区有强需求，可基于 stub 接口扩展真实 DPDK PMD

### 后续动作

- [x] 保持现有 stub 不变
- [ ] 在 README 中注明 DPDK 为实验性 stub
- [ ] 若 v1.3+ 引入真实 DPDK，需新增 `CSILK_ENABLE_DPDK=ON` CMake 选项
