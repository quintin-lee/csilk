#pragma once
/**
 * @file tool_registry.h
 * @brief Unified AI tool registry with JSON Schema parameter validation and dispatch.
 *
 * @copyright MIT License
 */

#include <stdbool.h>
#include <stddef.h>

#include "csilk/drivers/ai.h"
#include "csilk/core/json.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Tool execution callback function.
 *
 * @param args_json JSON string containing arguments passed by the model.
 * @param user_data User context forwarded from registration.
 * @return Heap-allocated result string (caller frees), or NULL on failure.
 */
typedef char* (*csilk_ai_tool_exec_fn)(const char* args_json, void* user_data);

typedef struct csilk_ai_tool_registry_s csilk_ai_tool_registry_t;

/**
 * @brief Create a new AI tool registry.
 * @return Registry handle, or NULL on failure.
 */
csilk_ai_tool_registry_t* csilk_ai_tool_registry_new(void);

/**
 * @brief Free a tool registry and all registered tool schemas.
 * @param reg Registry handle.
 */
void csilk_ai_tool_registry_free(csilk_ai_tool_registry_t* reg);

/**
 * @brief Register a tool in the registry.
 *
 * @param reg Registry handle.
 * @param name Tool name (e.g. "get_weather").
 * @param description Tool description for the model.
 * @param parameters_schema_json JSON schema string defining parameter structure.
 * @param fn Execution callback function.
 * @param user_data User context forwarded to fn.
 * @return 0 on success, -1 on failure (e.g. name duplicate or NULL).
 */
int csilk_ai_tool_registry_register(csilk_ai_tool_registry_t* reg,
                                    const char*               name,
                                    const char*               description,
                                    const char*               parameters_schema_json,
                                    csilk_ai_tool_exec_fn     fn,
                                    void*                     user_data);

/**
 * @brief Unregister a tool by name.
 * @param reg Registry handle.
 * @param name Tool name to remove.
 * @return 0 on success, -1 if not found.
 */
int csilk_ai_tool_registry_unregister(csilk_ai_tool_registry_t* reg, const char* name);

/**
 * @brief Build an array of csilk_ai_tool_t suitable for csilk_ai_chat_request_t.
 *
 * The returned array is allocated on heap and must be freed with csilk_ai_tools_free().
 *
 * @param reg Registry handle.
 * @param[out] out_count Number of tools in array.
 * @return Array of csilk_ai_tool_t or NULL if no tools registered.
 */
csilk_ai_tool_t* csilk_ai_tool_registry_export_tools(csilk_ai_tool_registry_t* reg,
                                                     size_t*                   out_count);

/**
 * @brief Free an exported tools array created by csilk_ai_tool_registry_export_tools.
 * @param tools Tools array to free.
 * @param count Number of tools in array.
 */
void csilk_ai_tools_free(csilk_ai_tool_t* tools, size_t count);

/**
 * @brief Execute a tool by name with JSON string arguments.
 *
 * Validates JSON arguments syntax and required properties, then executes the callback.
 *
 * @param reg Registry handle.
 * @param name Tool name to invoke.
 * @param args_json JSON arguments string.
 * @return Heap-allocated result string (caller must free with free()), or NULL on error.
 */
char*
csilk_ai_tool_dispatch(csilk_ai_tool_registry_t* reg, const char* name, const char* args_json);

/**
 * @brief Get number of registered tools.
 * @param reg Registry handle.
 * @return Count of registered tools.
 */
size_t csilk_ai_tool_registry_get_count(csilk_ai_tool_registry_t* reg);

#ifdef __cplusplus
}
#endif
