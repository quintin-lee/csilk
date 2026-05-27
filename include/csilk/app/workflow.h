/**
 * @file workflow.h
 * @brief AI Workflow engine for the csilk framework.
 *
 * Provides a graph-based orchestration engine for AI pipelines and agents.
 * Supports sequential, parallel, and agentic loops.
 */

#ifndef CSILK_WORKFLOW_H
#define CSILK_WORKFLOW_H

#include "csilk/csilk.h"

/**
 * @brief Generic data container for passing messages between workflow nodes.
 */
typedef struct csilk_data_s {
  char* type;               /**< MIME-like type identifier. */
  void* value;              /**< Opaque data pointer. */
  void (*free_fn)(void*);   /**< Optional callback to free the value. */
} csilk_data_t;

/**
 * @brief Function signature for a workflow node handler.
 */
typedef csilk_data_t* (*csilk_wf_handler_t)(csilk_data_t* input, void* user_data);

/** @brief Opaque handle for a single node in a workflow. */
typedef struct csilk_wf_node_s csilk_wf_node_t;

/** @brief Opaque handle for a workflow instance. */
typedef struct csilk_wf_s csilk_wf_t;

/* --- Lifecycle --- */

/**
 * @brief Create a new AI workflow instance.
 * @param name Descriptive name for the workflow.
 * @return New workflow handle, or NULL on failure.
 */
csilk_wf_t* csilk_wf_new(const char* name);

/**
 * @brief Deallocate a workflow and all its nodes.
 * @param wf Workflow handle.
 */
void csilk_wf_free(csilk_wf_t* wf);

/**
 * @brief Add a node to the workflow.
 * @param wf        Workflow handle.
 * @param id        Unique ID for the node.
 * @param handler   Function to execute.
 * @param user_data Opaque pointer passed to the handler.
 * @return Handle to the created node, or NULL on failure.
 */
csilk_wf_node_t* csilk_wf_add(csilk_wf_t* wf, const char* id,
                              csilk_wf_handler_t handler, void* user_data);

#endif /* CSILK_WORKFLOW_H */
