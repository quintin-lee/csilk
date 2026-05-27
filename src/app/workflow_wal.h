#ifndef CSILK_WORKFLOW_WAL_H
#define CSILK_WORKFLOW_WAL_H

#include <stdint.h>
#include "csilk/app/workflow.h"

#define CSILK_WF_MAGIC 0x5746414C /* "WFAL" */

typedef enum {
    WF_EV_START = 1,
    WF_EV_NODE_START = 2,
    WF_EV_NODE_FINISH = 3,
    WF_EV_END = 4
} csilk_wf_event_type_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t type;
    uint32_t timestamp;
    uint32_t payload_len;
} csilk_wf_wal_header_t;

/* Internal helpers for workflow.c */
int _wf_wal_append(const char* wal_path, csilk_wf_event_type_t type, const void* payload, size_t len);

#endif /* CSILK_WORKFLOW_WAL_H */
