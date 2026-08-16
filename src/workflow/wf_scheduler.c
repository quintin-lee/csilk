/**
 * @file wf_scheduler.c
 * @brief Workflow DAG scheduler — thin wrapper for backward compatibility.
 *
 * Implementation split across:
 *   wf_ctx.c  - context lifecycle (register/unregister/find/cleanup)
 *   wf_wal.c  - WAL event logging
 *   wf_node.c - node execution (workers, timers, dispatch)
 *   wf_run.c  - run orchestration (entry points, TTL)
 */

#include "workflow_internal.h"
