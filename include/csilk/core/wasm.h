/**
 * @file wasm.h
 * @brief Native WASM/WASI plugin sandbox and execution engine.
 * @copyright MIT License
 */

#ifndef CSILK_WASM_H
#define CSILK_WASM_H

#include <stddef.h>
#include <stdint.h>

#include "csilk/app/workflow_dsl.h"
#include "csilk/protocols/mcp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct csilk_wasm_plugin_s csilk_wasm_plugin_t;

typedef enum {
    CSILK_WASM_OK = 0,
    CSILK_WASM_ERR_INVALID_MAGIC = -1,
    CSILK_WASM_ERR_INVALID_VERSION = -2,
    CSILK_WASM_TRAP_OUT_OF_BOUNDS = -3,
    CSILK_WASM_TRAP_FUEL_EXHAUSTED = -4,
    CSILK_WASM_TRAP_EXECUTION_ERR = -5
} csilk_wasm_status_t;

typedef struct {
    uint32_t max_memory_pages; /* Max pages (default 1024 = 64MB) */
    uint64_t fuel_limit;       /* Max fuel limit (default 1,000,000) */
} csilk_wasm_config_t;

/**
 * @brief Loads a WASM plugin module from a file path.
 * @param filepath Path to the .wasm file.
 * @param config Optional sandbox configuration (NULL for defaults).
 * @return Pointer to allocated csilk_wasm_plugin_t or NULL on error.
 */
csilk_wasm_plugin_t* csilk_wasm_plugin_load_file(const char*                filepath,
                                                 const csilk_wasm_config_t* config);

/**
 * @brief Frees a WASM plugin module instance.
 * @param plugin Plugin handle to free.
 */
void csilk_wasm_plugin_free(csilk_wasm_plugin_t* plugin);

/**
 * @brief Executes an exported function in the WASM plugin.
 * @param plugin Plugin handle.
 * @param func_name Name of the exported function.
 * @param json_input Input JSON payload string.
 * @param err_buf Buffer to store error message.
 * @param err_len Size of error buffer.
 * @return Dynamically allocated JSON result string (caller must free), or NULL on failure.
 */
char* csilk_wasm_plugin_exec(csilk_wasm_plugin_t* plugin,
                             const char*          func_name,
                             const char*          json_input,
                             char*                err_buf,
                             size_t               err_len);

/**
 * @brief Registers a WASM plugin step to a Workflow DAG.
 * @param wf Workflow handle.
 * @param node_id Unique node ID.
 * @param wasm_filepath Path to the .wasm file.
 * @return 0 on success, negative value on failure.
 */
int csilk_wf_add_wasm_node(csilk_wf_t* wf, const char* node_id, const char* wasm_filepath);

/**
 * @brief Registers a WASM plugin as an MCP Tool.
 * @param server MCP server handle.
 * @param wasm_filepath Path to the .wasm file.
 * @param tool_name Tool name.
 * @param description Tool description.
 * @return 0 on success, negative value on failure.
 */
int csilk_mcp_server_register_wasm_tool(csilk_mcp_server_t* server,
                                        const char*         wasm_filepath,
                                        const char*         tool_name,
                                        const char*         description);

#ifdef __cplusplus
}
#endif

#endif /* CSILK_WASM_H */
