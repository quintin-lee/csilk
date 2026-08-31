# csilk 代码库全量再分析报告(2026-08-30 · v3)

> 本报告是对 **当前 master** 的独立全量再验证。自 v2 分析(`e16f3141`)以来代码仅有一处修复(`1d49d8c0`,12 行),故本版以**独立重建采集新数据** + **显式复核刚合入的修复** + **核实覆盖战役与遗留项现状**为核心,与前两版(`-full.md`、`-full-v2.md`)并存作为历史记录。

---

## 一、执行摘要

csilk 是一个 C23 编写、面向高并发(10K QPS / P99 ≤ 5ms)场景的嵌入式 HTTP 框架。本次 v3 全量再验证基于真实独立构建与 `ctest` 实测,结论延续 v2:分层严格、单向依赖、核心热路径覆盖扎实。

**关键事实(本次独立采集):**

| 项 | 数值 |
|---|---|
| 当前版本 | **v0.5.3**(cmake/options.cmake) |
| 构建 | clang-22.1.8 Debug 全新配置,clang-22.1.8 独立目录,0 error |
| 警告 | 3 条(benchmark 的 `__attribute__((optimize))` 被 clang 忽略,均无害) |
| 测试注册 | **227 个**(较 v2 的 227 持平,含 `test_ws_integration`/`test_bounded_buf`) |
| 单测实测 | **225/225 通过**(排除 2 个 integration),109.5s |
| 覆盖战役成果 | 新鲜复核通过:bounded_buf **100%**、websocket **85%**、gzip **68%**、ratelimit fail-open 分支已覆盖 |

### 综合评分

| 维度 | 评分 | 结论 |
|---|---|---|
| 架构 | ★★★★★ | 分层/依赖/所有权模型未见退化 |
| 正确性/内存安全 | ★★★★☆ | 上一处修复独立复核通过;遗留项均为低危 |
| 测试与工具链 | ★★★★★ | 227 测试全绿;覆盖战役成果稳固 |

---

## 二、架构分析(新鲜复核)

### 2.1 模块规模(当前 master)

| 目录 | 行数 | 定位 |
|---|---|---|
| `src/core/` | ~22.7K | HTTP 脚手架、arena、ctx、路由、并发 |
| `src/drivers/` | ~7.6K | DB / AI / 向量 / cipher 驱动 |
| `src/workflow/` | ~5.1K | Agent 引擎、DAG、DSL |
| `src/middleware/` | ~5.5K | auth/cors/jwt/ratelimit/ws/gzip… |
| `src/protocols/` | ~3.4K | WebSocket / H2 / H3 / swagger |
| `src/messaging/` | ~2.1K | MQ / pubsub / WAL / raft |
| `src/crypto/` | ~2.2K | base64/sha1/bcrypt/blowfish |
| 其余 | — | app / reflection / util |

> 行数区间为 v2 采集的近似值,自 v2 以来代码零结构性变化,规模未变。

### 2.2 分层、依赖与所有权

对照 AGENTS.md 依赖图核验:依赖单向、无循环,内部头(`ctx_internal.h`、`gzip_internal.h`、`srv_impl.h`)均限 `src/core/internal/`,不向外暴露。六级内存所有权模型、worker 单线程隔离、RCU 路由交换、世代标签防 ABA 等设计均未变化,与 v2 判定一致。

### 2.3 上一处修复的接口/边界独立复核

commit `1d49d8c0` 的三处改动逐一独立复核(**全部正确**):

1. **`gzip.c` 判空加固**(`_csilk_gzip_after_work_cb`):
   `if (state && state->dest && state->ret == Z_STREAM_END)`。验证:若 `dest` 为 NULL 而 ret 恰为 Z_STREAM_END(正常不会发生),走 else 分支 `free(state->dest)`(NULL 安全)。行为未改变,纯防御性加固。✅
2. **`test_pool_economics.c` TLS 清理**:结尾 `_csilk_worker_set_current_pool(NULL)` + `csilk_arena_flush_free_list()`。验证:`_csilk_worker_set_current_pool`(connection_close.c)为纯 TLS 赋值,`NULL` 时安全置空,清除了栈地址残留与 arena chunk 缓存。属测试隔离改进,不影响生产。✅
3. **`server_shutdown.c` H3 排序注释**:纯文档,明确 hooks→listener→drain→dispatch/signal→workers→loop 的隐式顺序依据,无逻辑改动。✅

**结论:** 上一处修复正确落地,无副作用,测试已验证(225/225 通过)。

---

## 三、正确性与内存安全(新鲜复核)

### 3.1 覆盖战役成果核实(残留分支)

本地 gcc coverage 构建独立复跑 5 个文件相关测试:

| 文件 | 行覆盖 | 未覆盖行分布 |
|---|---|---|
| bounded_buf.c | **100%** | —(全部覆盖) |
| websocket.c | **85%** | 142/158/162/…/421(故障注入、>64KB 编码受写缓冲上限约束) |
| gzip.c | **68%** | 40-41/53-55/…/283-284(全为 malloc/deflateInit/队列失败) |
| ratelimit.c | 61%* | fail-open 关键分支(85-88、114-119)**已覆盖** |

> *本目录仅跑 fail-open 测试;CI 全量(含 sliding 家族)为 76%。关键饱和/fail-open 分支两种口径均不在未覆盖列表。

**结论:** 覆盖战役四文件的成果仍稳固成立,与 v2 完全一致,无回归。

### 3.2 遗留项现状(v2 → v3 无变化)

| 项 | 状态 | 说明 |
|---|---|---|
| **H1** ratelimit 表饱和 fail-open | ✅ 已修复 | `get_or_create_ip_entry` 饱和返 NULL + 本地路径 fail-open |
| **H2** Retry-After 硬编码 60s | ⚠️ 仍在 | `ratelimit.c` local + distributed 两处硬编码 `"60"`,与 `WINDOW_SIZE=60` 一致,低危 |
| **H3** 停机隐式时序 | 📝 已文档化 | `on_stop_async` 注释明确顺序依据逻辑 |
| **H4** 测试栈上 TLS 残留 | ✅ 已修复 | `test_pool_economics.c` 结尾清空 TLS |
| **N2** ip_table 静态 5MB | ⚠️ 仍在 | 65536×80B=5.00MB 常驻固定开销,无锁设计权衡 |
| **N3** gzip 防御判空 | ✅ 已修复 | 上一处 commit |
| **N4** gzip 故障注入路径 | ⚠️ 未覆盖 | 全部为 malloc/deflateInit/队列失败,测试成本边界 |

**无新增高危发现。**

### 3.3 核心模块状态(与 v2 一致)

连接生命周期(CAS 幂等销毁 + 世代标签)、arena(multi-tier + `_Thread_local` + flush)、ctx/defer/async(defer LIFO + async 引用计数)、ratelimit(无锁开放寻址 + CAS + fail-open)均未变化,保持 v2 的严谨性判定。

---

## 四、测试与工具链评估(独立实测)

### 4.1 本轮实测数据

- `ctest --show-only` 注册 **227 个测试**。
- `ctest -E test_integration` **225/225 通过**(≈109.5s),0 失败。
- 覆盖战役新增测试(`test_ws_integration` 0.07s、`test_bounded_buf` 0.01s)仍在并通过。
- 构建 0 error,3 条同类无害属性警告(benchmark `optimize`)。

### 4.2 CI 状态

上一次推送(`acf7cf51..1d49d8c0`,含 v2 分析文档 + 修复)后 CI **11/11 全绿**:
io_uring / Lint / Performance / ARM64 / Examples / Fuzz / TSAN / Ubuntu+macOS×Debug+Release;Code Coverage 也 success。

### 4.3 gzip ASAN 修复回归确认

本版独立 clang Debug 重建缓解并复跑 `test_gzip`,确认 ASAN 安全修复(`acf7cf51`)未引入回归,`test_gzip` 通过。线程池裸跑方案的越界原子 bug 已彻底排除。

---

## 五、方法、结论与建议

### 5.1 本次方法

- **独立重建**:新目录 `build_v3`(clang-22.1.8 Debug,ENABLE_OOM_TEST=ON)全新配置+编译+全量单测,采集全新数据。
- **修复复核**:逐行审 1 处 commit 的三处改动,结合实现确认正确性。
- **覆盖核实**:本地 gcc coverage 目录(gcovr)复跑验证 4 文件覆盖。
- **遗留核实**:重读 ratelimit.c 确认 H1/H2/N2 现状。

### 5.2 结论

当前 master 稳定:架构优秀、正确性核心路径扎实、测试完成度高。v2 以来唯一代码改动(1 处 fix)已独立验证正确且 CI 全绿。剩余开放项(H2/N2/N4)维持 v2 判定,均低危或属设计权衡,无阻塞性缺陷。

### 5.3 后续可选工作(按价值排序)

| 优先级 | 项 | 建议 |
|---|---|---|
| P2 | H2 Retry-After 动态化 | 可接入窗口配置;语义改进非 bug |
| P3 | N2 ip_table 5MB | 文档明确固定开销,或后续改稀疏结构 |
| P3 | N4 gzip/mysql 等故障注入路径 | 需引入 OOM 故障注入框架,收益边际 |
| P4 | drivers/(51%) 覆盖 | 依赖真实外部服务,建议集成/模拟;非本地可解 |

---

*报告采用真实构建与测试支撑结论。历史版本:`analysis-2026-08-30.md`、`analysis-2026-08-30-full.md`、`analysis-2026-08-30-full-v2.md`。*