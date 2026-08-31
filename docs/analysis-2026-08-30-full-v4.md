# csilk 代码库全量再分析报告(2026-08-30 · v4)

> 本报告是对 **当前 master** 的独立全量再验证。自 v3 分析(`9c26cc0c`)以来,唯一实质代码改动是 **H2 修复**(`8fbbf1d3`,ratelimit 精确 Retry-After)与一次 5 文件文档同步(`9062dc46`)。故本版以**全量独立重建采集新数据** + **显式复核 H2 修复** + **核实文档同步与覆盖战役现状**为核心,与前几版(`-full.md`、`-full-v3.md`)并存作为历史记录。

---

## 一、执行摘要

csilk 是一个 C23 编写、面向高并发(10K QPS / P99 ≤ 5ms)场景的嵌入式 HTTP 框架。本次 v4 全量再验证基于**全新独立构建**与 `ctest` 实测,结论延续 v3:分层严格、单向依赖、核心热路径覆盖扎实;H2 修复逻辑正确且已覆盖,全库新增一处行为改进无回归。

**关键事实(本次独立采集):**

| 项 | 数值 |
|---|---|
| 当前版本 | **v0.5.3**(cmake/options.cmake) |
| 构建 | clang-22.1.8 Debug 全新 build_v4 目录,`-DENABLE_OOM_TEST=ON`(对齐 v2/v3 口径),0 error |
| 警告 | 3 条(benchmark 的 `__attribute__((optimize))` 被 clang 忽略,均无害) |
| 测试注册 | **227 个**(含 `test_oom`/`test_oom_io`) |
| 单测实测 | **225/225 通过**(排除 `test_integration`+`test_integration_ext`),95.5s |
| 覆盖战役成果 | 新鲜复核通过:bounded_buf **100%**、websocket **86%**、gzip **68%**、ratelimit 本地 Retry-After **已覆盖** |

**本次审阅焦点:** H2 修复(ratelimit 精确 Retry-After)。

### 综合评分

| 维度 | 评分 | 结论 |
|---|---|---|
| 架构 | ★★★★★ | 分层/依赖/所有权模型未见退化 |
| 正确性/内存安全 | ★★★★☆ | H2 修复逐行复核通过;遗留项仍为低危 |
| 测试与工具链 | ★★★★★ | 225 测试全绿;覆盖战役成果稳固,文档同步准确 |

---

## 二、H2 修复独立复核(`8fbbf1d3`)

### 改动内容
`src/middleware/ratelimit.c` 两处 `Retry-After` 硬编码 `"60"` 替换为动态计算:

**本地路径**(`_csilk_rate_limit_local`):
```c
time_t reset_now = atomic_load(&entry->last_reset);
time_t retry = (time_t)WINDOW_SIZE - (now - reset_now);
if (retry < 1) { retry = 1; }
char retry_after[32];
snprintf(retry_after, sizeof(retry_after), "%lld", (long long)retry);
csilk_set_header(c, "Retry-After", retry_after);
```

**分布式路径**(`csilk_rate_limit_middleware`):
```c
char retry_after[32];
snprintf(retry_after, sizeof(retry_after), "%lld", (long long)WINDOW_SIZE);
csilk_set_header(c, "Retry-After", retry_after);
```

### 逐分支正确性验证

| 场景 | 行为 | 判定 |
|---|---|---|
| **窗口翻转 CAS 成功**(fresh window) | `reset_now == now`,`retry = 60 - 0 = 60` | ✅ 正确(全新窗口,需等完整窗口) |
| **窗口翻转 CAS 失败**(别线程抢到) | `reset_now` 为对方新窗口起点,与 `now` 几乎相等,`retry ≈ 60` | ✅ 乐观正确 |
| **窗口未翻转**(正常超限) | `now - reset_now` 为已流逝秒数,`retry` 精确反映剩余 | ✅ 精确 |
| **下限 1** | 防 `now - reset_now` 略超窗口产生的负值/0 | ✅ 合理边界 |

- **类型安全**:`time_t`/`WINDOW_SIZE` 同为 signed,`%lld` 匹配 `(long long)`,缓冲 32B 充足。
- **竞态处理**:blocked 分支**重读** `last_reset`,避免在读取窗口判定后发生窗口翻转导致用旧起点计算——处理正确。
- **分布式保守策略**:`csilk_incr(c,key,WINDOW_SIZE)` 只返回计数不返回 TTL,无法得知精确剩余时间,故用完整 `WINDOW_SIZE` 作为不夸大的保守值;语义正确。
- **无副作用**:仅替换 header 值写入,metrics/json_error/abort 流程不变。
- **测试覆盖**:新增 `test_ratelimit_retry_after_precise`(首请求放行→次请求 429→断言 Retry-After∈[1,60])。本地 gcc coverage 实测确认本地 Retry-After 分支(145-170 区间)**已被覆盖**。
- **测试顺序修正**:fail-open 测试会填满 65536 槽 IP 表且不清理,导致后续计数测试饱和;新计数类测试已排在其前。

**复核结论:✅ 通过,无缺陷。**

---

## 三、架构分析(与 v3 对照)

自 v3 以来架构无结构性变化,以下为确认要点:

- **分层与依赖**:单向依赖、无循环的结论延续。覆盖战役引入的接口改动(此前已审):
  - `_csilk_rate_limit_local`(CSILK_INTERNAL 导出)——抽象干净,公开中间件保持原行为,无 ABI 污染。
  - `gzip_internal.h`——真实回调经 internal 头暴露供测试直接驱动,接口边界清晰。
- **内存模型**:bounded_buf 100% 覆盖、arena/ctx 全绿,无新增所有权事务。
- **并发模型**:worker/RCU/世代标签/推送模型稳定;H2 修复未触及并发要害(仅读原子 + 格式化字符串)。

---

## 四、正确性/内存安全核查

### 遗留项现状核实(v3 判定延续)

| 项 | 状态 | 说明 |
|---|---|---|
| H1 ratelimit fail-open | ✅ 已修复(饱和返 NULL + fail-open) | 覆盖稳定 |
| H2 Retry-After 硬编码 | ✅ **本轮已修复**(v4 焦点) | 本地精确 + 分布式保守 |
| H3 停机隐式时序 | 📝 文档化注释(显式排序依据) | 无代码风险 |
| H4 测试 TLS 残留 | ✅ 已修复(测试清空 TLS 指针) | 无 UAF 风险 |
| N2 ip_table 5MB 静态 | ⚠️ 设计权衡(无锁表性能) | 文档已同步 65536 槽 |
| N3 gzip 防御判空 | ✅ 已修复(else 分支 NULL-free 安全) | 纯防御 |
| N4 gzip 故障注入路径 | 📝 测试成本边界,不改 | 非正常运行路径 |

**结论:无新高危缺陷。H1/H2/H3/H4/N3 已闭环,仅 N2(设计权衡)与 N4(测试成本边界)维持现状。**

---

## 五、测试与工具链评估

### 覆盖战役成果新鲜复核(本地 gcc coverage,仅跑相关测试)

| 文件 | 实测覆盖 | v3 记录 | 判定 |
|---|---|---|---|
| bounded_buf.c | **100%** (166/166) | 100% | ✅ 仍成立 |
| websocket.c | **86%** (166/193) | 85% | ✅ 仍成立(随跑法微涨) |
| gzip.c | **68%** (82/119) | 68% | ✅ 仍成立 |
| ratelimit.c | **71%** (52/73) | ~76% | ✅ 本地路径已覆盖;分布式路径未覆盖属环境(无 storage driver) |

- ratelimit 未覆盖行(184/186-187/189-191/196/200-203/205/207/211)为**分布式存储分支**(`csilk_incr` 路径),本地 unit 无 storage mock,非缺陷。
- **H2 本地 Retry-After 分支已确认覆盖**。

### 文档同步一致性(`9062dc46`)
- AGENTS.md/README/CHANGELOG 覆盖数据 66%→**69%** 与 CI Code Coverage 权威值一致。
- CODEPEC(en/zh-CN) `MAX_IP_ENTRIES` 1024→**65536** 与实际代码一致。
- 测试数 213→**227** 与 CI 注册数一致。
- 限流行为文档仅写 "SHOULD 设置 Retry-After",未描述固定值,无需改动(搜索确认)。
- **判定:✅ 文档同步准确。**

### 测试矩阵
- clang Debug 227 注册 / 225 通过(OOM 测试含入)。
- 全量 CI 上次 push(`8fbbf1d3`)后 11/11 全绿(ASAN/TSAN/Fuzz/io_uring/ARM64/双平台/覆盖率)。

---

## 六、相对 v3 的增量总结

| 维度 | v3 (2026-08-30) | v4 (2026-08-30) |
|---|---|---|
| 实质代码改动 | — | **H2 Retry-After 精确化**(1 生产文件+1 测试文件) |
| 新建目录 | build_v3 | **build_v4**(独立重建,对齐 OOM 口径) |
| 测试注册/通过 | 227 / 225 | **227 / 225**(口径一致) |
| 覆盖战役 | 成立 | **复核仍成立** |
| 遗留项 | H2 待修 | **H2 已修复**,新增无 |
| 文档 | v3 报告 | **文档同步准确** |

---

## 七、结论

当前 master 稳定,**无阻塞性缺陷**。H2(Retry-After 硬编码)为本轮亮点修复——本地精确计算、分布式保守策略、竞态重读逻辑正确、测试覆盖到位且 CI 无损。

剩余开放项仅 **N2**(ip_table 5MB 静态常驻,设计权衡)与 **N4**(gzip 故障注入测试成本边界),均为低危;分布式 rate-limit 路径覆盖需 storage mock 方可达,属环境性缺口。

**评分重申:架构 ★★★★★ / 正确性 ★★★★☆ / 测试工具链 ★★★★★。**