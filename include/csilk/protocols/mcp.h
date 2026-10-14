/**
 * @file mcp.h
 * @brief Model Context Protocol (MCP) Server and Client interfaces.
 *
 * Provides native support for Model Context Protocol (2024-11-05 specification),
 * allowing csilk workflows and tools to be exported to external LLMs/IDEs as
 * an MCP Server (via Stdio or SSE/HTTP), and consuming external MCP Servers as
 * an MCP Client.
 */

#ifndef CSILK_MCP_H
#define CSILK_MCP_H

#include "csilk/app/workflow.h"
#include "csilk/csilk.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct csilk_mcp_server_s csilk_mcp_server_t;
typedef struct csilk_mcp_client_s csilk_mcp_client_t;

/* --- MCP Server API --- */

/**
 * @brief Creates a new MCP Server instance.
 * @param name Server identification name.
 * @param version Server version string.
 * @return Pointer to newly allocated server, or NULL on failure.
 */
csilk_mcp_server_t* csilk_mcp_server_new(const char* name, const char* version);

/**
 * @brief Frees an MCP Server instance and its associated resources.
 * @param server The server instance to free.
 */
void csilk_mcp_server_free(csilk_mcp_server_t* server);

/**
 * @brief Registers a workflow tool to be exposed via MCP tools/list and tools/call.
 * @param server The server instance.
 * @param tool The workflow tool to register.
 * @return 0 on success, non-zero on failure.
 */
int csilk_mcp_server_register_tool(csilk_mcp_server_t* server, csilk_wf_tool_entry_t* tool);

/**
 * @brief Registers an AI workflow to be exposed via MCP tools/list and prompts.
 * @param server The server instance.
 * @param wf The workflow to register.
 * @return 0 on success, non-zero on failure.
 */
int csilk_mcp_server_register_workflow(csilk_mcp_server_t* server, csilk_wf_t* wf);

/**
 * @brief Registers a WASM module as an MCP server tool.
 * @param server         MCP server handle.
 * @param wasm_filepath  Path to the .wasm file backing the tool.
 * @param tool_name      Tool name.
 * @param description    Tool description.
 * @return 0 on success, negative on failure.
 */
int csilk_mcp_server_register_wasm_tool(csilk_mcp_server_t* server,
                                        const char*         wasm_filepath,
                                        const char*         tool_name,
                                        const char*         description);

/**
 * @brief Starts the MCP Server using non-blocking Stdio transport (stdin/stdout).
 * @param server The server instance.
 * @return 0 on success, non-zero on failure.
 */
int csilk_mcp_server_start_stdio(csilk_mcp_server_t* server);

/**
 * @brief Binds the MCP Server to a csilk web application using HTTP SSE transport.
 * @param server The server instance.
 * @param app The csilk application instance.
 * @param route_prefix Route prefix for SSE endpoints (e.g. "/mcp").
 * @return 0 on success, non-zero on failure.
 */
int
csilk_mcp_server_bind_app(csilk_mcp_server_t* server, csilk_app_t* app, const char* route_prefix);

/* --- MCP Client API --- */

/**
 * @brief Connects to an external MCP Server via Stdio pipe.
 * @param command Executable command path.
 * @param argv Command-line arguments array (NULL-terminated).
 * @return Pointer to connected MCP client, or NULL on failure.
 */
csilk_mcp_client_t* csilk_mcp_client_connect_stdio(const char* command, char* const argv[]);

/**
 * @brief Connects to an external MCP Server via HTTP SSE endpoint.
 * @param sse_url SSE endpoint URL.
 * @return Pointer to connected MCP client, or NULL on failure.
 */
csilk_mcp_client_t* csilk_mcp_client_connect_sse(const char* sse_url);

/**
 * @brief Frees an MCP Client instance and closes its connection.
 * @param client The client instance to free.
 */
void csilk_mcp_client_free(csilk_mcp_client_t* client);

/**
 * @brief Fetches tools from the remote MCP server and imports them into a workflow.
 * @param client The connected MCP client.
 * @param wf The target workflow to attach imported tools to.
 * @return Number of tools successfully imported, or negative error code on failure.
 */
int csilk_mcp_client_import_tools(csilk_mcp_client_t* client, csilk_wf_t* wf);

/* --- MCP Stdio Client Pool API --- */

/**
 * @brief Acquire an MCP stdio client associated with a specific key.
 *
 * If an idle client with matching key exists in the pool, it is reused.
 * Otherwise, a new client is spawned and pooled.
 *
 * @param key Unique key for client pooling (e.g. server command or "user:server").
 * @param command Executable command path.
 * @param argv Command-line arguments array (NULL-terminated).
 * @return Acquired MCP client pointer, or NULL on failure.
 */
csilk_mcp_client_t*
csilk_mcp_stdio_pool_acquire_keyed(const char* key, const char* command, char* const argv[]);

/**
 * @brief Acquire an MCP stdio client for a command from the pool.
 *
 * Equivalent to csilk_mcp_stdio_pool_acquire_keyed(command, command, argv).
 *
 * @param command Executable command path.
 * @param argv Command-line arguments array (NULL-terminated).
 * @return Acquired MCP client pointer, or NULL on failure.
 */
csilk_mcp_client_t* csilk_mcp_stdio_pool_acquire(const char* command, char* const argv[]);

/**
 * @brief Release an MCP stdio client back to the pool for reuse.
 *
 * If the client was pooled, marks it idle and keeps the process alive.
 * If the client was short-lived (not pooled), frees it immediately.
 *
 * @param client Client to release.
 */
void csilk_mcp_stdio_pool_release(csilk_mcp_client_t* client);

/**
 * @brief Evict idle clients whose inactivity exceeds ttl_seconds.
 *
 * @param ttl_seconds Inactivity threshold in seconds (<= 0 uses default 60s or env).
 * @return Number of evicted clients.
 */
int csilk_mcp_stdio_pool_evict_idle(int ttl_seconds);

/**
 * @brief Shut down all active and idle clients in the pool.
 *
 * @return Number of destroyed clients.
 */
int csilk_mcp_stdio_pool_shutdown(void);

/**
 * @brief Get total number of pooled clients.
 */
size_t csilk_mcp_stdio_pool_get_count(void);

/**
 * @brief Get number of idle pooled clients.
 */
size_t csilk_mcp_stdio_pool_get_idle_count(void);

#ifdef __cplusplus
}
#endif

#endif /* CSILK_MCP_H */
