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

/** @brief Opaque handle for a workflow instance. */
typedef struct csilk_wf_s csilk_wf_t;

/** @brief Opaque handle for a workflow execution context. */
typedef struct csilk_wf_ctx_s csilk_wf_ctx_t;

/**
 * @brief Join policies for nodes with multiple incoming dependencies.
 */
typedef enum {
  CSILK_WF_JOIN_AND, /**< Default: Trigger only when ALL inputs arrive. */
  CSILK_WF_JOIN_OR   /**< Trigger when ANY input arrives. */
} csilk_wf_join_policy_t;

/**
 * @brief Generic data container for passing messages between workflow nodes.
 */
typedef struct csilk_data_s {
  char* type;               /**< MIME-like type identifier. */
  void* value;              /**< Opaque data pointer. */
  void (*free_fn)(void*);   /**< Optional callback to free the value. */
} csilk_data_t;

/**
 * @brief Dynamic router function signature.
 * @param input Data from the node that just finished.
 * @return ID of the next node to trigger, or NULL for default routing.
 */
typedef const char* (*csilk_wf_router_t)(csilk_data_t* input);

/**
 * @brief Configuration for built-in AI nodes.
 */
typedef struct {
  const char* model;       /**< AI model identifier. */
  const char* system_msg;  /**< Optional system prompt. */
  const char* prompt;      /**< User prompt (supports {{node.value}} templates). */
  double temperature;
  int max_tokens;
} csilk_ai_config_t;

/**
 * @brief Function signature for a workflow node handler.
 * @param ctx   Execution context (can be used for arena allocation).
 * @param input Input data from previous node(s).
 * @param user_data Opaque pointer passed during node creation.
 */
typedef csilk_data_t* (*csilk_wf_handler_t)(csilk_wf_ctx_t* ctx,
                                            csilk_data_t* input,
                                            void* user_data);

/** @brief Opaque handle for a single node in a workflow. */
typedef struct csilk_wf_node_s csilk_wf_node_t;

/* --- Memory Helpers (Arena-backed) --- */

/**
 * @brief Allocate a new data container managed by the workflow arena.
 * @param ctx  Execution context.
 * @param type Data type identifier (copied).
 * @param value Pointer to the data.
 * @return New data container.
 */
csilk_data_t* csilk_wf_data_new(csilk_wf_ctx_t* ctx, const char* type,
                                void* value);

/**
 * @brief Duplicate a string using the workflow arena.
 * @param ctx Execution context.
 * @param s   Source string.
 * @return Copied string.
 */
char* csilk_wf_strdup(csilk_wf_ctx_t* ctx, const char* s);

/**
 * @brief Allocate memory from the workflow arena.
 * @param ctx  Execution context.
 * @param size Number of bytes.
 * @return Pointer to allocated memory.
 */
void* csilk_wf_alloc(csilk_wf_ctx_t* ctx, size_t size);

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

/**
 * @brief Add a built-in AI node with template support.
 * @param wf     Workflow handle.
 * @param id     Unique ID.
 * @param config AI configuration (copied internally).
 * @return Node handle.
 */
csilk_wf_node_t* csilk_wf_add_ai(csilk_wf_t* wf, const char* id,
                                 const csilk_ai_config_t* config);

/**
 * @brief Mark a node as an entry point for the workflow.
 * @param node     Node handle.
 * @param is_entry Non-zero to mark as entry, 0 to unmark.
 * @note By default, nodes with 0 incoming edges are entries. Use this
 *       to explicitly start a node that is also part of a loop.
 */
void csilk_wf_node_set_entry(csilk_wf_node_t* node, int is_entry);

/* --- Connections --- */

/**
 * @brief Bind two nodes sequentially (default routing).
 * @param from Source node.
 * @param to   Destination node.
 */
void csilk_wf_bind(csilk_wf_node_t* from, csilk_wf_node_t* to);

/**
 * @brief Add a conditional route between nodes.
 * @param from      Source node.
 * @param condition Result string that triggers this route (e.g., "fail").
 * @param to        Destination node.
 */
void csilk_wf_on(csilk_wf_node_t* from, const char* condition,
                 csilk_wf_node_t* to);

/**
 * @brief Add a loop-back / feedback route between nodes.
 * @param from      Source node.
 * @param condition Result string that triggers this route.
 * @param to        Destination node (typically an earlier node).
 * @note Unlike csilk_wf_on, this does NOT increment the 'to' node's
 *       incoming dependency count, preventing deadlocks in join logic.
 */
void csilk_wf_on_loop(csilk_wf_node_t* from, const char* condition,
                      csilk_wf_node_t* to);

/**
 * @brief Add an error fallback route.
 * @param from   Source node.
 * @param target Destination node triggered if the source handler fails.
 */
void csilk_wf_on_error(csilk_wf_node_t* from, csilk_wf_node_t* target);

/**
 * @brief Set a dynamic router for a node.
 * @param node   Node handle.
 * @param router Function that determines the next node based on output.
 */
void csilk_wf_route(csilk_wf_node_t* node, csilk_wf_router_t router);

/**
 * @brief Set the join policy for a node.
 * @param node   Node handle.
 * @param policy AND (default) or OR.
 */
void csilk_wf_node_set_join(csilk_wf_node_t* node, csilk_wf_join_policy_t policy);

/* --- Execution --- */

/**
 * @brief Run the workflow asynchronously.
 * @param wf       Workflow handle.
 * @param input    Initial input data.
 * @param callback Callback invoked when the workflow completes or exits.
 */
void csilk_wf_run(csilk_wf_t* wf, csilk_data_t* input,
                  void (*callback)(csilk_data_t* result));

/* --- Visualization --- */

/**
 * @brief Export the workflow graph as a Mermaid string.
 * @param wf Workflow handle.
 * @return Heap-allocated Mermaid code (caller must free).
 */
char* csilk_wf_to_mermaid(csilk_wf_t* wf);

#endif /* CSILK_WORKFLOW_H */
