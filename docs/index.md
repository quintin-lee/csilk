# Csilk Documentation Portal | 文档中心

> **Version**: 0.9.0 | **Last updated**: 2026-11-14

Welcome to the official documentation for **Csilk (`csilk`)**, a high-performance C23 asynchronous Web framework, microservice runtime, and distributed agentic workflow engine.

欢迎查阅 **Csilk (`csilk`)** 官方文档。Csilk 是基于 C23 标准开发的高性能异步 Web 服务、微服务运行时与分布式智能体工作流引擎。

---

## 🌐 Choose Your Language | 选择文档语言

<div align="center">

| [🇺🇸 English Documentation](en/index.md) | [🇨🇳 简体中文文档](zh-CN/index.md) |
|---|---|
| Complete English guides, architecture whitepapers, user manuals, and API specifications. | 完整的中文架构设计解析、模块深度白皮书、用户开发手册及工程规范。 |

</div>

---

## 🚀 Quick Navigation | 快速导航

| Topic / 主题 | English Documentation | 简体中文文档 |
|---|---|---|
| **Getting Started / 快速上手** | [Getting Started Guide](en/getting-started.md) | [快速上手指南](zh-CN/getting-started.md) |
| **Architecture / 系统架构** | [Architecture & Philosophy](en/architecture.md)<br/>[Codebase Deep Dive](en/architecture/csilk-codebase-deep-dive-report.md) | [系统架构与设计哲学](zh-CN/architecture.md)<br/>[代码全景深度剖析白皮书](zh-CN/architecture/csilk-codebase-deep-dive-report.md) |
| **Design Documents / 设计白皮书** | [Comparative Performance Benchmarks (v1.0 LTS)](en/design/comparative-benchmark-whitepaper.md)<br/>[ABI Opaque Roadmap](en/design/abi-opaque-roadmap.md) | [四方性能基准对比白皮书](zh-CN/design/comparative-benchmark-whitepaper.md)<br/>[不透明 ABI 演进路线图](zh-CN/design/abi-opaque-roadmap.md) |
| **User Manual / 用户开发手册** | [User Manual Index](en/user-manual/index.md) | [用户手册总览](zh-CN/user-manual/index.md) |
| **Module Design / 模块设计解析** | [Module Design Index](en/module-design/index.md) | [模块深度设计解析](zh-CN/module-design/index.md) |
| **Performance / 性能调优** | [Performance Tuning Guide](en/performance-tuning.md) | [性能极致调优指南](zh-CN/performance-tuning.md) |
| **Troubleshooting / 故障诊断** | [Troubleshooting Guide](en/troubleshooting.md) | [常见问题排查与诊断](zh-CN/troubleshooting.md) |
| **Contributing / 参与贡献** | [How to Build](en/contributing/how-to-build.md)<br/>[Test Guide](en/contributing/test-guide.md) | [编译与构建指南](zh-CN/contributing/how-to-build.md)<br/>[测试与验证指南](zh-CN/contributing/test-guide.md) |
| **API Reference / API 索引** | [Doxygen HTML Reference](html/index.html) | [Doxygen 在线 API 索引](html/index.html) |

---

## 📂 Documentation Structure | 文档目录树

```text
docs/
├── en/                       # 🇺🇸 English Documentation
│   ├── index.md              # English Home & Sitemap
│   ├── getting-started.md    # Quickstart Guide
│   ├── architecture.md       # Core Architecture & Memory Models
│   ├── performance-tuning.md # Compiler flags, Kernel tuning, PGO
│   ├── troubleshooting.md    # Diagnostics & Common Fixes
│   ├── architecture/         # Deep Dive Technical Reports
│   ├── contributing/         # Build & Test Guides
│   ├── design/               # Architectural Design Specifications (ABI, Benchmarks)
│   ├── module-design/        # Subsystem Deep-Dive Reports
│   ├── user-manual/          # Comprehensive User Manuals
│   └── meta/                 # Evolution Roadmap & Specifications
│
├── zh-CN/                    # 🇨🇳 简体中文文档
│   ├── index.md              # 中文首页与索引地图
│   ├── getting-started.md    # 快速上手指南
│   ├── architecture.md       # 核心架构机制与内存模型
│   ├── performance-tuning.md # 性能调优与内核参数
│   ├── troubleshooting.md    # 故障诊断与问题排查
│   ├── architecture/         # 架构深度剖析白皮书
│   ├── contributing/         # 编译与测试指南
│   ├── design/               # 架构设计规范 (ABI, 性能白皮书)
│   ├── module-design/        # 子系统深度解析报告
│   ├── user-manual/          # 用户开发与运维手册
│   └── meta/                 # 演进路线图与规范
│
├── superpowers/              # 🛠️ Internal Project Specs & Plans
├── assets/                   # Static Documentation Assets (Mermaid, CSS)
└── index.md                  # 🌐 Root Documentation Portal & Language Switcher
```
