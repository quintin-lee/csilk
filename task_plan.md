# 任务计划：Server-C 框架演进与重构完善

## 目标
执行“稳扎稳打：性能与稳定性优先”战略，通过基建先行、性能压榨、体验打磨、功能拓展四个阶段完善框架。

## 阶段

### 阶段一：稳定性与测试基建 (Stability & Testing Infrastructure) [已完成]
- [x] 1.1 覆盖率与边界单元测试 (`url_parser.c` 和 `router.c` 等，目标覆盖率 95%+)
- [x] 1.2 内存泄漏检测：在测试套件中全面集成 ASan (AddressSanitizer)
- [x] 1.3 Fuzzing 模糊测试：针对 HTTP 报文解析逻辑及路由匹配逻辑进行模糊测试
- [x] 1.4 CI/CD 自动化：配置自动化运行构建、测试和内存检查

### 阶段二：架构与性能深度调优 (Architecture & Performance Tuning) [已完成]
- [x] 2.1 请求级内存池 (Memory Pool)：降低 `malloc` 碎片与锁竞争
- [x] 2.2 路由树局部性优化与 Fast-path
- [x] 2.3 异常流优化：优化 `setjmp/longjmp` 的 recovery 开销

### 阶段三：文档与开发者体验 (Documentation & DX)
- [ ] 3.1 规范化注释与 Doxygen API 文档
- [ ] 3.2 架构机制白皮书
- [ ] 3.3 示例工程扩展 (example_server 增强)

### 阶段四：新功能拓展 (New Features)
- [ ] 4.1 WebSocket 协议支持
- [ ] 4.2 更多官方中间件 (Rate Limiter, CSRF, CORS)
- [ ] 4.3 传输层安全 (TLS/HTTPS) 调研与集成

## 遇到的错误
*(暂无)*
