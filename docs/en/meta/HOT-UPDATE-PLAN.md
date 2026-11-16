# 热更新完善 — v1.2.0

## 现状

### 已有的热更新能力

1. **RCU Router Swap**（`server_rcu.c`）
   - `csilk_dev_hot_reload_trigger()` 原子切换路由表
   - EBR grace period 保证并发读者安全
   - 100% wait-free readers（无锁查找）

2. **WASM Hot Reload**（`hot_reload.c`）
   - `csilk_dev_hot_reload_start(server, lib_path, init_sym)` 监控 .so 文件
   - inotify 触发 → dlopen 新库 → 调用 init_sym → RCU swap
   - 临时库拷贝使用 `mkstemp(0600)`，OOM 回滚

3. **Config Hot Reload**（`config_hot_reload.c`）
   - 监控 YAML 配置文件
   - 重新解析 → 原子更新 `csilk_server_config_t`

### 缺失：三合一原子切换

当前三个组件独立触发，不保证原子性：
- Router swap 后 WASM 未加载 → 路由到旧 handler
- WASM 加载后 config 未更新 → 新 WASM 用旧配置
- Config 更新后 router 未 swap → 新配置不影响路由

## 目标

实现 `csilk_hot_update(server, router_path, wasm_path, config_path)` 一次性原子切换三个组件。

## 设计方案

### 三阶段原子切换

```
Phase 1: 加载（非阻塞，后台线程）
  ├─ 1a. dlopen 新 WASM 库（独立 handle）
  ├─ 1b. 解析新 router YAML → 构建 trie（独立 handle）
  └─ 1c. 解析新 config YAML → 构建 config 结构（独立 handle）

Phase 2: 验证（非阻塞）
  ├─ 2a. 验证 router trie 完整性
  ├─ 2b. 验证 WASM 入口点存在
  └─ 2c. 验证 config schema 兼容

Phase 3: 原子切换（持有 config_mutex）
  ├─ 3a. RCU swap: router_old → router_new
  ├─ 3b. RCU swap: wasm_old → wasm_new
  ├─ 3c. 指针更新: config_old → config_new
  └─ 3d. EBR grace period → 释放旧资源
```

### 关键数据结构

```c
typedef struct {
    csilk_router_t*    router;
    void*              wasm_handle;
    csilk_config_t*    config;
} hot_update_bundle_t;

int csilk_hot_update(csilk_server_t* server,
                     const char*     router_yaml,
                     const char*     wasm_lib,
                     const char*     config_yaml);
```

### 回滚

任何 Phase 2 验证失败 → 不执行 Phase 3，旧组件继续运行。
Phase 3 执行中失败 → 不可能（仅指针交换，无分配）。
Phase 3 后运行时错误 → 通过 `csilk_hot_update_rollback(server, prev_bundle)` 回滚。

## 实现计划

| 阶段 | 工作量 | 说明 |
|---|---|---|
| Bundle 结构 + 加载 | 2d | 三组件并行加载 |
| 验证 | 1d | 各组件 schema/入口验证 |
| 原子切换 | 1d | config_mutex + RCU |
| 回滚 | 1d | 保存 prev_bundle |
| 测试 | 1d | 并发切换 + 回滚 + OOM |

**总计**：6 天

## 验证

- `test_hot_reload_stress`：1000 次并发切换
- `test_hot_update_rollback`：注入失败验证回滚
- `test_config_hot_reload`：config 原子性
