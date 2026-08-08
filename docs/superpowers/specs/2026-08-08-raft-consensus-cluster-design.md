# Integrated Raft Consensus & Cluster HA Engine Design Specification

## Overview

This specification defines the architecture, components, API contracts, and binary frame protocols for integrating an embedded **Raft Consensus & High Availability Cluster Engine** into `csilk` (server-c).

The system provides zero-dependency, linearizable consensus across `csilk` nodes, facilitating Leader election, WAL log replication, snapshotting, and distributed Workflow Replicated State Machine (RSM) synchronization with sub-second failover.

---

## 1. System Architecture & Module Boundaries

### 1.1 Directory Structure

```
include/csilk/
  └── messaging/
      └── raft.h              # Public Raft cluster consensus API header

src/
  ├── messaging/
  │   ├── raft_consensus.c    # Raft core state machine (Follower/Candidate/Leader, Term & Election)
  │   ├── raft_rpc.c          # RequestVote, AppendEntries, InstallSnapshot binary RPC framing
  │   └── raft_snapshot.c     # Log compaction and snapshot creation/restoration
  └── workflow/
      └── wf_cluster_sm.c     # Workflow Replicated State Machine & Distributed Task Locker
```

### 1.2 Architectural Principles

1. **Zero External Dependencies**: Operates natively in C23 using `csilk`'s low-latency network primitives and existing `raft_wal.c` storage.
2. **Linearizable State Machine**: Workflow node states (`QUEUED`, `RUNNING`, `COMPLETED`, `FAILED`) and lock leases are replicated via Raft log entries to guarantee exact-once execution semantics.
3. **Sub-second Failover**: Uses randomized election timeouts (150ms ~ 300ms) and 50ms heartbeat intervals to detect node failure and re-elect a new Leader within 300ms.

---

## 2. Raft State Machine & Node Control Block

### 2.1 Node State Definitions (`raft_consensus.c`)

```c
typedef enum {
    CSILK_RAFT_ROLE_FOLLOWER  = 0,
    CSILK_RAFT_ROLE_CANDIDATE = 1,
    CSILK_RAFT_ROLE_LEADER    = 2
} csilk_raft_role_t;

typedef struct csilk_raft_node_s {
    csilk_mutex_t        mutex;
    csilk_raft_role_t    role;
    uint64_t             current_term;    /* Persistent term counter */
    char                 voted_for[64];   /* Persistent Candidate ID voted for */
    csilk_raft_wal_t*    wal;             /* WAL log handle */
    
    uint64_t             commit_index;    /* Volatile: highest committed index */
    uint64_t             last_applied;    /* Volatile: highest applied index */
    
    /* Leader-only state */
    uint64_t             next_index[64];  /* Next log index for each peer */
    uint64_t             match_index[64]; /* Highest replicated index for each peer */
    
    uint64_t             election_timeout_ms;
    uint64_t             last_heartbeat_time;
} csilk_raft_node_t;
```

---

## 3. RPC Framing & Protocol Serialization

### 3.1 Binary Frame Header (`raft_rpc.c`)

```c
#define CSILK_RAFT_MAGIC 0x52414654 /* "RAFT" Magic */

typedef enum {
    CSILK_RAFT_MSG_REQUEST_VOTE_REQ     = 0x01,
    CSILK_RAFT_MSG_REQUEST_VOTE_RESP    = 0x02,
    CSILK_RAFT_MSG_APPEND_ENTRIES_REQ   = 0x03,
    CSILK_RAFT_MSG_APPEND_ENTRIES_RESP  = 0x04,
    CSILK_RAFT_MSG_INSTALL_SNAPSHOT_REQ = 0x05,
    CSILK_RAFT_MSG_INSTALL_SNAPSHOT_RESP= 0x06
} csilk_raft_msg_type_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;       /* 0x52414654 */
    uint8_t  msg_type;    /* csilk_raft_msg_type_t */
    uint32_t payload_len; /* Payload byte size */
    uint64_t term;        /* Current term */
} csilk_raft_hdr_t;
```

---

## 4. Replicated State Machine (RSM) Workflow Integration

### 4.1 Cluster Workflow Commands (`wf_cluster_sm.c`)

Workflow actions are encapsulated into log commands replicated across the cluster:

```c
typedef enum {
    CSILK_WF_CMD_REGISTER   = 0x10,  /* Register workflow definition */
    CSILK_WF_CMD_NODE_START = 0x11,  /* Mark node execution start */
    CSILK_WF_CMD_NODE_DONE  = 0x12,  /* Mark node completion and output */
    CSILK_WF_CMD_LOCK_ACQ   = 0x13,  /* Acquire distributed lock */
    CSILK_WF_CMD_LOCK_REL   = 0x14   /* Release distributed lock */
} csilk_wf_cmd_type_t;
```

---

## 5. Public API Contracts

### 5.1 `include/csilk/messaging/raft.h`

```c
#ifndef CSILK_RAFT_H
#define CSILK_RAFT_H

#include <stddef.h>
#include <stdint.h>
#include "csilk/app/workflow_dsl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct csilk_raft_node_s csilk_raft_node_t;

typedef enum {
    CSILK_RAFT_ROLE_FOLLOWER  = 0,
    CSILK_RAFT_ROLE_CANDIDATE = 1,
    CSILK_RAFT_ROLE_LEADER    = 2
} csilk_raft_role_t;

typedef struct {
    char              node_id[64];
    char              listen_addr[128];
    uint16_t          port;
    const char*       wal_dir;
    uint64_t          heartbeat_interval_ms;
    uint64_t          election_timeout_min_ms;
    uint64_t          election_timeout_max_ms;
} csilk_raft_config_t;

/**
 * @brief Constructs a new Raft consensus node.
 */
csilk_raft_node_t* csilk_raft_node_new(const csilk_raft_config_t* config);

/**
 * @brief Frees a Raft consensus node.
 */
void csilk_raft_node_free(csilk_raft_node_t* node);

/**
 * @brief Registers a peer node in the cluster topology.
 */
int csilk_raft_node_add_peer(csilk_raft_node_t* node, const char* peer_id, const char* peer_addr);

/**
 * @brief Starts Raft consensus loops and timers.
 */
int csilk_raft_node_start(csilk_raft_node_t* node);

/**
 * @brief Queries current role (Follower, Candidate, or Leader).
 */
csilk_raft_role_t csilk_raft_get_role(csilk_raft_node_t* node);

/**
 * @brief Binds Workflow Manager to Raft node for cluster state replication.
 */
int csilk_wf_cluster_bind_raft(csilk_wf_manager_t* mgr, csilk_raft_node_t* raft_node);

#ifdef __cplusplus
}
#endif

#endif /* CSILK_RAFT_H */
```

---

## 6. Test Plan

1. **`test_raft_consensus.c`**: Test state transitions (Follower -> Candidate -> Leader), term increments, and majority quorum counting.
2. **`test_raft_rpc.c`**: Test binary frame serialization, deserialization, and header magic verification.
3. **`test_wf_cluster_sm.c`**: Test workflow state machine command replication and state machine callback execution.
4. **`test_raft_failover.c`**: Simulate a 3-node cluster, terminate the active Leader, and verify election of a new Leader within 300ms.
