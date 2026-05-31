/**
 * @file workflow.h
 * @brief AI Workflow engine for the csilk framework.
 *
 * Provides a graph-based orchestration engine for AI pipelines and agents.
 * Supports sequential execution, parallel fan-out (via multi-input join
 * policies), conditional routing, agentic loops with feedback edges, and
 * built-in AI node handlers with LLM tool calling.
 *
 * ## Graph Model
 * The workflow is a directed acyclic (or cyclic) graph of nodes.  Each node
 * is a handler function (or a built-in AI node) that receives data from its
 * predecessors and emits data to its successors.  Edges can be:
 *   - Sequential (csilk_wf_bind): default routing, always fires.
 *   - Conditional (csilk_wf_on): fires only when output matches a string.
 *   - Loop-back (csilk_wf_on_loop): cycles but does NOT increment the target's
 *     incoming-edge counter (avoiding deadlock in AND-join logic).
 *   - Error fallback (csilk_wf_on_error): fires on handler failure.
 *   - Dynamic (csilk_wf_route): programmatic router callback decides the next
 *     node at runtime.
 *
 * ## Execution
 * csilk_wf_run starts execution asynchronously.  Entry nodes (nodes with 0
 * incoming edges or explicitly marked) fire immediately.  Results propagate
 * through the graph via the event loop.  WAL persistence enables crash
 * recovery via csilk_wf_resume.
 *
 * ## Thread Safety
 * Workflow definitions are read-only during execution (all mutation must
 * happen before csilk_wf_run).  The runtime is single-threaded on the
 * libuv event loop.
 *
 * @copyright MIT License
 */

#ifndef CSILK_WORKFLOW_H
#define CSILK_WORKFLOW_H

#include "csilk/app/app.h"
#include "csilk/csilk.h"

/**
 * @brief Register a default route to serve the workflow dashboard.
 * @param app  Application handle.
 * @param path URL path (e.g., "/admin/workflow").
 */
void csilk_wf_serve_ui(csilk_app_t* app, const char* path);

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
 *
 * Every node receives a csilk_data_t as input and returns one as output.
 * The @p type string allows downstream nodes to switch behaviour based on
 * content type.  The optional @p free_fn callback lets the workflow engine
 * clean up heap-allocated values when the data is no longer needed.
 * The @p meta field is a catch-all for extra information (e.g., AI token
 * counts, timing metrics) and is NOT freed by the engine.
 */
typedef struct csilk_data_s {
	char* type;		/**< MIME-like type identifier (e.g., "text/plain",
                             "application/json"). */
	void* value;		/**< Opaque data pointer.  Ownership semantics
                             depend on @p free_fn. */
	void (*free_fn)(void*); /**< Optional: called by the engine to free @p value
                             when the data is consumed.  nullptr means the
                             engine does NOT take ownership. */
	void* meta;		/**< Optional metadata pointer (not freed by the
                             engine).  Common use: AI token usage stats. */
} csilk_data_t;

/** @brief Trace record for a single node execution. */
typedef struct {
	char* node_id;
	uint64_t start_time; /**< Microseconds (uv_hrtime) */
	uint64_t end_time;   /**< Microseconds */
	char* input_dump;    /**< String representation of input */
	char* output_dump;   /**< String representation of output */
	char* model;	     /**< AI model used (if applicable) */
	int prompt_tokens;
	int completion_tokens;
	char* error; /**< Error message if failed */
} csilk_wf_trace_node_t;

/** @brief Complete execution trace of a workflow. */
typedef struct {
	char* exec_id; /**< Unique execution ID */
	uint64_t start_time;
	uint64_t end_time;
	csilk_wf_trace_node_t** nodes;
	size_t node_count;
} csilk_wf_trace_t;

/**
 * @brief Dynamic router function signature.
 * @param input Data from the node that just finished.
 * @return ID of the next node to trigger, or nullptr for default routing.
 */
typedef const char* (*csilk_wf_router_t)(csilk_data_t* input);

/**
 * @brief Configuration for built-in AI nodes.
 */
typedef struct {
	const char* model;	/**< AI model identifier. */
	const char* system_msg; /**< Optional system prompt. */
	const char* prompt;	/**< User prompt (supports {{node.value}} templates). */
	double temperature;
	int max_tokens;
	int stream;		  /**< Enable token streaming to monitors. */
	int max_history_messages; /**< Max messages to keep in history (0 =
                               unlimited). */
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
csilk_data_t* csilk_wf_data_new(csilk_wf_ctx_t* ctx, const char* type, void* value);

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
 * @return New workflow handle, or nullptr on failure.
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
 * @return Handle to the created node, or nullptr on failure.
 */
csilk_wf_node_t*
csilk_wf_add(csilk_wf_t* wf, const char* id, csilk_wf_handler_t handler, void* user_data);

/**
 * @brief Add a built-in AI node with template support.
 * @param wf     Workflow handle.
 * @param id     Unique ID.
 * @param config AI configuration (copied internally).
 * @return Node handle.
 */
csilk_wf_node_t* csilk_wf_add_ai(csilk_wf_t* wf, const char* id, const csilk_ai_config_t* config);

/* --- Tool Calling --- */

/**
 * @brief Function signature for a workflow tool.
 * @param args_json JSON string of arguments provided by the LLM.
 * @param user_data Opaque pointer passed during registration.
 * @return JSON string or text result (caller will free).
 */
typedef char* (*csilk_wf_tool_fn)(const char* args_json, void* user_data);

/**
 * @brief A single tool entry — corresponds to one OpenAI-compatible
 *        "function" tool.  Used both for static registration and dynamic
 *        discovery.
 *
 * For dynamically-discovered tools, the name and description strings must
 * be heap-allocated; the workflow engine takes ownership and will free them
 * after the AI node completes.
 */
typedef struct csilk_wf_tool_entry_s {
	char* name;	       /**< Tool name exposed to the LLM. */
	char* description;     /**< Human-readable description (for tool schema). */
	char* parameters_json; /**< JSON Schema string for the tool's parameters. */
	csilk_wf_tool_fn fn;   /**< C callback invoked for tool execution. */
	void* user_data;       /**< Opaque context forwarded to @p fn. */
} csilk_wf_tool_entry_t;

/**
 * @brief Register a tool that AI nodes can call.
 * @param wf Workflow handle.
 * @param name Function name exposed to the LLM.
 * @param description Description of what the tool does.
 * @param parameters_json JSON Schema string for the arguments.
 * @param fn C function to execute.
 * @param user_data Context for the function.
 */
void csilk_wf_register_tool(csilk_wf_t* wf,
			    const char* name,
			    const char* description,
			    const char* parameters_json,
			    csilk_wf_tool_fn fn,
			    void* user_data);

/**
 * @brief Dynamic tool discovery callback (MCP-like protocol).
 *
 * Invoked each time an AI node builds its tool list. The workflow's
 * statically-registered tools are provided as read-only context; the
 * callback can augment them with dynamically-discovered tools (e.g.,
 * from remote MCP servers, plugins, or service registries).
 *
 * Implementation notes:
 * - The @p discovered_out array and its name/description strings must be
 *   heap-allocated. They are freed by the workflow engine after the AI
 *   node completes.
 * - On name collision, statically-registered tools take precedence.
 * - The callback may return 0 tools (set @p discovered_count_out to 0
 *   and @p discovered_out to nullptr).
 * - Runs on the thread pool — blocking I/O (HTTP calls, DB queries) is
 *   acceptable here.
 *
 * @param wf             Workflow handle.
 * @param static_tools   Statically-registered tools (read-only).
 * @param static_count   Number of static tools.
 * @param[out] discovered_out      Heap-allocated array of discovered tools.
 * @param[out] discovered_count_out Number of discovered tools.
 * @param user_data      Opaque pointer from csilk_wf_set_tool_discovery.
 * @return 0 on success, -1 on failure (static tools still work). */
typedef int (*csilk_wf_tool_discovery_fn)(csilk_wf_t* wf,
					  const csilk_wf_tool_entry_t* static_tools,
					  size_t static_count,
					  csilk_wf_tool_entry_t** discovered_out,
					  size_t* discovered_count_out,
					  void* user_data);

/**
 * @brief Set a dynamic tool discovery callback.
 *
 * When set, every AI node in the workflow will call @p discovery before
 * sending the tool list to the LLM.  This enables MCP-server integration,
 * dynamic plugin loading, and other late-binding tool scenarios.
 *
 * @param wf        Workflow handle.
 * @param discovery Discovery callback (nullptr to disable).
 * @param user_data Opaque pointer passed to the callback on each invocation.
 */
void
csilk_wf_set_tool_discovery(csilk_wf_t* wf, csilk_wf_tool_discovery_fn discovery, void* user_data);

/**
 * @brief Get a node by ID.
 * @param wf Workflow handle.
 * @param id Node ID.
 * @return Node handle or nullptr if not found.
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
void csilk_wf_on(csilk_wf_node_t* from, const char* condition, csilk_wf_node_t* to);

/**
 * @brief Add a loop-back / feedback route between nodes.
 * @param from      Source node.
 * @param condition Result string that triggers this route.
 * @param to        Destination node (typically an earlier node).
 * @note Unlike csilk_wf_on, this does NOT increment the 'to' node's
 *       incoming dependency count, preventing deadlocks in join logic.
 */
void csilk_wf_on_loop(csilk_wf_node_t* from, const char* condition, csilk_wf_node_t* to);

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

/**
 * @brief Mark a node as interactive (requires human signal to proceed).
 * @param node           Node handle.
 * @param is_interactive Non-zero to enable interactive mode.
 */
void csilk_wf_node_set_interactive(csilk_wf_node_t* node, int is_interactive);

/**
 * @brief Set an expected JSON Schema for a node's output.
 * @param node   Node handle.
 * @param schema JSON Schema string (nullptr to disable).
 */
void csilk_wf_node_set_schema(csilk_wf_node_t* node, const char* schema);

/**
 * @brief Signal a paused workflow to continue.
 * @param wf       Workflow definition.
 * @param exec_id  Execution ID of the paused workflow.
 * @param input    Optional replacement input (e.g., human-edited prompt).
 * @param callback Callback for when the resumed workflow finishes.
 */
void csilk_wf_signal_continue(csilk_wf_t* wf,
			      const char* exec_id,
			      csilk_data_t* input,
			      void (*callback)(csilk_data_t* result));

/**
 * @brief Set a timeout for a specific node.
 * @param node       Node handle.
 * @param timeout_ms Timeout in milliseconds (0 for no timeout).
 */
void csilk_wf_node_set_timeout(csilk_wf_node_t* node, int timeout_ms);

/**
 * @brief Set a global TTL for the workflow execution.
 * @param wf      Workflow handle.
 * @param ttl_sec TTL in seconds (0 for no limit).
 */
void csilk_wf_set_ttl(csilk_wf_t* wf, int ttl_sec);

/**
 * @brief Set automatic retry policy for a specific node.
 * @param node           Node handle.
 * @param max_retries    Maximum number of retry attempts.
 * @param retry_delay_ms Delay before each retry.
 */
void csilk_wf_node_set_retry(csilk_wf_node_t* node, int max_retries, int retry_delay_ms);

/**
 * @brief Mark a node for remote execution via MQ.
 * @param node      Node handle.
 * @param is_remote Non-zero to offload to remote worker.
 */
void csilk_wf_node_set_remote(csilk_wf_node_t* node, int is_remote);

/**
 * @brief Enable distributed execution by bridging workflow with an MQ.
 * @param wf Workflow handle.
 * @param mq MQ handle for task distribution.
 */
void csilk_wf_enable_distributed(csilk_wf_t* wf, csilk_mq_t* mq);

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
const char*
csilk_wf_run(csilk_wf_t* wf, csilk_data_t* input, void (*callback)(csilk_data_t* result));

/**
 * @brief Resume an interrupted workflow execution from a WAL file.
 * @param wf Workflow definition.
 * @param exec_id The execution ID to resume.
 * @param callback Callback for when the resumed workflow finishes.
 */
void csilk_wf_resume(csilk_wf_t* wf, const char* exec_id, void (*callback)(csilk_data_t* result));

/**
 * @brief Run workflow and generate a trace.
 * @param wf Workflow handle.
 * @param input Initial input.
 * @param callback Callback receiving final result and the full trace.
 */
void csilk_wf_run_traced(csilk_wf_t* wf,
			 csilk_data_t* input,
			 void (*callback)(csilk_data_t* result, csilk_wf_trace_t* trace));

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
 * @return Workflow handle, or nullptr on failure.
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
