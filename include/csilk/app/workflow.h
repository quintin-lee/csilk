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
  void* meta;               /**< Optional metadata (e.g., AI token usage). */
} csilk_data_t;

/** @brief Trace record for a single node execution. */
typedef struct {
  char* node_id;
  uint64_t start_time;      /**< Microseconds (uv_hrtime) */
  uint64_t end_time;        /**< Microseconds */
  char* input_dump;         /**< String representation of input */
  char* output_dump;        /**< String representation of output */
  char* model;              /**< AI model used (if applicable) */
  int prompt_tokens;
  int completion_tokens;
  char* error;              /**< Error message if failed */
} csilk_wf_trace_node_t;

/** @brief Complete execution trace of a workflow. */
typedef struct {
  char* exec_id;            /**< Unique execution ID */
  uint64_t start_time;
  uint64_t end_time;
  csilk_wf_trace_node_t** nodes;
  size_t node_count;
} csilk_wf_trace_t;

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

/* --- Tool Calling --- */

/**
 * @brief Function signature for a workflow tool.
 * @param args_json JSON string of arguments provided by the LLM.
 * @param user_data Opaque pointer passed during registration.
 * @return JSON string or text result (caller will free).
 */
typedef char* (*csilk_wf_tool_fn)(const char* args_json, void* user_data);

/**
 * @brief Register a tool that AI nodes can call.
 * @param wf Workflow handle.
 * @param name Function name exposed to the LLM.
 * @param description Description of what the tool does.
 * @param parameters_json JSON Schema string for the arguments.
 * @param fn C function to execute.
 * @param user_data Context for the function.
 */
void csilk_wf_register_tool(csilk_wf_t* wf, const char* name,
                            const char* description,
                            const char* parameters_json, csilk_wf_tool_fn fn,
                            void* user_data);

/**
 * @brief Get a node by ID.
 * @param wf Workflow handle.
 * @param id Node ID.
 * @return Node handle or NULL if not found.
 */
csilk_wf_node_t* csilk_wf_get_node(csilk_wf_t* wf, const char* id);

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
 * @brief Enable WAL persistence for a workflow definition.
 * @param wf Workflow handle.
 * @param wal_dir Directory to store execution logs.
 */
void csilk_wf_set_persistence(csilk_wf_t* wf, const char* wal_dir);

/**
 * @brief Run the workflow asynchronously.
 * @param wf       Workflow handle.
 * @param input    Initial input data.
 * @param callback Callback invoked when the workflow completes or exits.
 * @return Unique Execution ID (string). Caller must not free.
 */
const char* csilk_wf_run(csilk_wf_t* wf, csilk_data_t* input,
                         void (*callback)(csilk_data_t* result));

/**
 * @brief Resume an interrupted workflow execution from a WAL file.
 * @param wf Workflow definition.
 * @param exec_id The execution ID to resume.
 * @param callback Callback for when the resumed workflow finishes.
 */
void csilk_wf_resume(csilk_wf_t* wf, const char* exec_id,
                     void (*callback)(csilk_data_t* result));

/**
 * @brief Run workflow and generate a trace.
 * @param wf Workflow handle.
 * @param input Initial input.
 * @param callback Callback receiving final result and the full trace.
 */
void csilk_wf_run_traced(csilk_wf_t* wf, csilk_data_t* input,
                         void (*callback)(csilk_data_t* result,
                                          csilk_wf_trace_t* trace));

/**
 * @brief Convert a trace object to a JSON string.
 * @return JSON string (caller must free).
 */
char* csilk_wf_trace_to_json(const csilk_wf_trace_t* trace);

/**
 * @brief Free a trace object.
 */
void csilk_wf_trace_free(csilk_wf_trace_t* trace);

/* --- Declarative API --- */

/**
 * @brief Register a global handler for use in declarative workflows.
 * @param name    Unique name (matches 'handler' key in YAML/JSON).
 * @param handler Function pointer.
 */
void csilk_wf_register_handler(const char* name, csilk_wf_handler_t handler);

/**
 * @brief Load a workflow definition from a YAML file.
 * @param path Path to the .yaml or .yml file.
 * @return Workflow handle, or NULL on failure.
 */
csilk_wf_t* csilk_wf_load_yaml(const char* path);

/**
 * @brief Create a workflow from a JSON string.
 */
csilk_wf_t* csilk_wf_from_json(const char* json);

/* --- Visualization --- */

/**
 * @brief Export the workflow graph as a Mermaid string.
 * @param wf Workflow handle.
 * @return Heap-allocated Mermaid code (caller must free).
 */
char* csilk_wf_to_mermaid(csilk_wf_t* wf);

/* --- Monitoring --- */

/**
 * @brief Register a WebSocket connection to receive live workflow updates.
 * @param wf Workflow handle.
 * @param c  Request context (must be upgraded to WebSocket).
 */
void csilk_wf_register_monitor(csilk_wf_t* wf, csilk_ctx_t* c);

/**
 * @brief Set a maximum token budget for the workflow.
 * @param wf         Workflow handle.
 * @param max_tokens Maximum total tokens (prompt + completion) allowed.
 */
void csilk_wf_set_budget(csilk_wf_t* wf, int max_tokens);

#endif /* CSILK_WORKFLOW_H */
