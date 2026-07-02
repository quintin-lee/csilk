#ifndef CSILK_WORKFLOW_INTERNAL_H
#define CSILK_WORKFLOW_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

/** @brief Trace record for a single node execution. */
typedef struct csilk_wf_trace_node_s {
    char*    node_id;
    uint64_t start_time;  /**< Microseconds (csilk_io_hrtime) */
    uint64_t end_time;    /**< Microseconds */
    char*    input_dump;  /**< String representation of input */
    char*    output_dump; /**< String representation of output */
    char*    model;       /**< AI model used (if applicable) */
    int      prompt_tokens;
    int      completion_tokens;
    char*    error;       /**< Error message if failed */
} csilk_wf_trace_node_t;

/** @brief Complete execution trace of a workflow. */
typedef struct csilk_wf_trace_s {
    char*                   exec_id; /**< Unique execution ID */
    uint64_t                start_time;
    uint64_t                end_time;
    csilk_wf_trace_node_t** nodes;
    size_t                  node_count;
} csilk_wf_trace_t;

#endif
