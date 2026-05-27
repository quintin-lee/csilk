/**
 * @file workflow_wal.h
 * @brief Write-Ahead Log (WAL) persistence for the AI Workflow engine.
 *
 * Provides fault-tolerant execution logging for csilk_wf_t workflows.
 * Every workflow event (start, node-start, node-finish, end) is appended
 * to a binary log file before it is processed.  If the server crashes,
 * interrupted workflows can be resumed by replaying the WAL.
 *
 * The WAL format is a sequence of packed binary records, each prefixed
 * by a csilk_wf_wal_header_t.  The payload is event-type-specific JSON
 * (e.g., node ID, input/output data).  This header is internal to the
 * workflow implementation and is not intended for direct use.
 *
 * @copyright MIT License
 */

#ifndef CSILK_WORKFLOW_WAL_H
#define CSILK_WORKFLOW_WAL_H

#include <stdint.h>

#include "csilk/app/workflow.h"

/** @brief Magic number identifying a WAL file ("WFAL" in ASCII). */
#define CSILK_WF_MAGIC 0x5746414C /* "WFAL" */

/**
 * @brief Event types recorded in the workflow WAL.
 *
 * Each event is written before the corresponding action is performed
 * (write-ahead semantics), ensuring that on recovery we can identify
 * partially-completed executions.
 */
typedef enum {
  WF_EV_START = 1,       /**< Workflow execution started. */
  WF_EV_NODE_START = 2,  /**< A specific node began executing (carries node ID
                            and input in payload). */
  WF_EV_NODE_FINISH = 3, /**< A specific node completed (carries node ID and
                            output in payload). */
  WF_EV_END = 4          /**< Workflow execution completed or was aborted. */
} csilk_wf_event_type_t;

/**
 * @brief Packed binary header prefixed to every WAL record.
 *
 * The header is followed by @p payload_len bytes of event-specific data
 * (typically a JSON string).  Total record size = sizeof(header) + payload_len.
 *
 * @note __attribute__((packed)) eliminates padding so the on-disk format is
 *       consistent across architectures and compilers.
 */
typedef struct __attribute__((packed)) {
  uint32_t magic;     /**< Must equal CSILK_WF_MAGIC for file validation. */
  uint8_t type;       /**< csilk_wf_event_type_t value. */
  uint32_t timestamp; /**< Unix timestamp (seconds since epoch) of the event. */
  uint32_t
      payload_len; /**< Byte length of the payload that follows this header. */
} csilk_wf_wal_header_t;

/**
 * @brief Append a WAL record to the log file.
 *
 * Internal helper used by workflow.c to persist each event.  Creates the
 * log file if it does not yet exist.  The WAL directory is set via
 * csilk_wf_set_persistence.
 *
 * @param wal_path  Path to the WAL file.
 * @param type      Event type enumerator.
 * @param payload   Event-specific data (typically a JSON string).
 * @param len       Byte length of @p payload.
 * @return 0 on success, -1 on I/O error.
 */
int _wf_wal_append(const char* wal_path, csilk_wf_event_type_t type,
                   const void* payload, size_t len);

#endif /* CSILK_WORKFLOW_WAL_H */
