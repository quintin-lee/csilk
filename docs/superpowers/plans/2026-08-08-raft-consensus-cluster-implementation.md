# Integrated Raft Consensus & Cluster HA Engine Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement zero-dependency embedded Raft consensus engine, binary RPC transport, snapshotting, and Workflow Replicated State Machine (RSM) replication in `csilk` (server-c).

**Architecture:** Add Raft consensus engine under `src/messaging/raft_consensus.c`, binary RPC under `src/messaging/raft_rpc.c`, workflow state machine under `src/workflow/wf_cluster_sm.c`, and public header `include/csilk/messaging/raft.h`.

**Tech Stack:** C23, Linux pthreads/sync primitives, CMake, clang-format, clang-tidy

**Spec:** `docs/superpowers/specs/2026-08-08-raft-consensus-cluster-design.md`

---

## File Structure

### New Files to Create

| File | Responsibility |
|------|---------------|
| `include/csilk/messaging/raft.h` | Public API header for Raft node lifecycle and workflow cluster binding |
| `src/messaging/raft_internal.h` | Internal Raft node control block, RPC structures, and state definitions |
| `src/messaging/raft_consensus.c` | Core Raft state machine (Follower, Candidate, Leader election & heartbeat) |
| `src/messaging/raft_rpc.c` | Binary RPC frame encoder/decoder for RequestVote and AppendEntries |
| `src/messaging/raft_snapshot.c` | Log compaction and snapshot creation/restoration |
| `src/workflow/wf_cluster_sm.c` | Replicated State Machine (RSM) for workflow state replication & distributed lock |
| `tests/messaging/test_raft_rpc.c` | Unit test binary RPC frame serialization and magic header verification |
| `tests/messaging/test_raft_consensus.c` | Unit test state transitions, term increments, and quorum vote counting |
| `tests/workflow/test_wf_cluster_sm.c` | Unit test workflow command log replication & state machine callbacks |
| `tests/messaging/test_raft_failover.c` | Integration test 3-node cluster leader failover and election recovery |

### Files to Modify

| File | Change |
|------|--------|
| `cmake/sources.cmake` | Add new Raft and Cluster source files |
| `cmake/tests.cmake` | Register new Raft test targets |

---

### Task 1: Create Public Header & Internal Definitions

**Files:**
- Create: `include/csilk/messaging/raft.h`, `src/messaging/raft_internal.h`

- [ ] **Step 1: Create `include/csilk/messaging/raft.h`**
Define `csilk_raft_role_t`, `csilk_raft_config_t`, node lifecycle functions, and `csilk_wf_cluster_bind_raft`.

- [ ] **Step 2: Create `src/messaging/raft_internal.h`**
Define `csilk_raft_node_t`, `csilk_raft_hdr_t`, and RPC structures.

---

### Task 2: Implement Binary RPC Frame Protocol (`raft_rpc.c`)

**Files:**
- Create: `src/messaging/raft_rpc.c`
- Create: `tests/messaging/test_raft_rpc.c`

- [ ] **Step 1: Implement `raft_rpc.c`**
Encode and decode `CSILK_RAFT_MSG_REQUEST_VOTE_REQ/RESP` and `CSILK_RAFT_MSG_APPEND_ENTRIES_REQ/RESP`.

- [ ] **Step 2: Write unit test `test_raft_rpc.c`**
Verify magic header `0x52414654` and payload serialization.

---

### Task 3: Implement Raft Core Consensus State Machine (`raft_consensus.c`)

**Files:**
- Create: `src/messaging/raft_consensus.c`
- Create: `tests/messaging/test_raft_consensus.c`

- [ ] **Step 1: Implement `raft_consensus.c`**
State machine transitions (Follower -> Candidate -> Leader), election timers, RequestVote handling, and AppendEntries heartbeat logic.

- [ ] **Step 2: Write unit test `test_raft_consensus.c`**
Verify term counter increments, quorum vote calculations, and heartbeat timers.

---

### Task 4: Implement Log Compaction & Snapshotting (`raft_snapshot.c`)

**Files:**
- Create: `src/messaging/raft_snapshot.c`

- [ ] **Step 1: Implement `raft_snapshot.c`**
Snapshot generation, WAL log truncation, and `InstallSnapshot` handling.

---

### Task 5: Implement Replicated State Machine & Workflow Integration (`wf_cluster_sm.c`)

**Files:**
- Create: `src/workflow/wf_cluster_sm.c`
- Create: `tests/workflow/test_wf_cluster_sm.c`

- [ ] **Step 1: Implement `wf_cluster_sm.c`**
Log command encoding for workflow node status and distributed lock acquisition.

- [ ] **Step 2: Write unit test `test_wf_cluster_sm.c`**
Verify workflow execution replication across state machine instances.

---

### Task 6: Implement Cluster Failover & 3-Node Integration Test (`test_raft_failover.c`)

**Files:**
- Create: `tests/messaging/test_raft_failover.c`

- [ ] **Step 1: Write integration test `test_raft_failover.c`**
Simulate 3 local nodes, kill active Leader, and assert new Leader election within 300ms.

---

### Task 7: CMake Integration & Final Verification

**Files:**
- Modify: `cmake/sources.cmake`, `cmake/tests.cmake`

- [ ] **Step 1: Add new sources to `cmake/sources.cmake` and `cmake/tests.cmake`**
- [ ] **Step 2: Full clean rebuild and test execution**
Run `make check-format` and `ctest --output-on-failure`.
