/**
 * @file workflow_dsl.h
 * @brief Declarative JSON/YAML Workflow DSL & Hot-Reloading Manager interfaces.
 *
 * Provides functions to parse, validate, and serialize workflows using JSON/YAML
 * formats, as well as zero-downtime hot reloading and WebSocket live debugging.
 */

#ifndef CSILK_WORKFLOW_DSL_H
#define CSILK_WORKFLOW_DSL_H

#include <stddef.h>

#include "csilk/app/workflow.h"
#include "csilk/csilk.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct csilk_wf_manager_s csilk_wf_manager_t;

/* --- DSL Parsing & Export API --- */

/**
 * @brief Parses a declarative JSON DSL string into a csilk_wf_t instance.
 * @param json_str Null-terminated JSON DSL string.
 * @param err_buf Buffer to store error description on failure (optional).
 * @param err_len Size of err_buf.
 * @return Pointer to constructed workflow, or NULL on failure.
 */
csilk_wf_t* csilk_wf_from_json_ext(const char* json_str, char* err_buf, size_t err_len);

/**
 * @brief Loads and parses a declarative JSON or YAML file into a csilk_wf_t instance.
 * @param filepath Path to JSON/YAML file.
 * @param err_buf Buffer to store error description on failure (optional).
 * @param err_len Size of err_buf.
 * @return Pointer to constructed workflow, or NULL on failure.
 */
csilk_wf_t* csilk_wf_from_file(const char* filepath, char* err_buf, size_t err_len);

/**
 * @brief Exports a runtime csilk_wf_t instance back into a JSON DSL string.
 * @param wf The workflow instance to serialize.
 * @return Dynamically allocated JSON string (must be freed with free()), or NULL on failure.
 */
char* csilk_wf_to_json(csilk_wf_t* wf);

/* --- Workflow Manager & Hot Reloading API --- */

/**
 * @brief Creates a new Workflow Manager instance.
 * @return Pointer to newly allocated manager, or NULL on failure.
 */
csilk_wf_manager_t* csilk_wf_manager_new(void);

/**
 * @brief Frees a Workflow Manager instance and its managed workflows.
 * @param mgr The manager instance to free.
 */
void csilk_wf_manager_free(csilk_wf_manager_t* mgr);

/**
 * @brief Registers a workflow under a specific name in the manager.
 * @param mgr The manager instance.
 * @param name Workflow registry key name.
 * @param wf The workflow instance.
 * @return 0 on success, non-zero on failure.
 */
int csilk_wf_manager_register(csilk_wf_manager_t* mgr, const char* name, csilk_wf_t* wf);

/**
 * @brief Atomically reloads and swaps a workflow definition without interrupting active executions.
 * @param mgr The manager instance.
 * @param name Workflow registry key name.
 * @param new_wf The newly compiled replacement workflow.
 * @return 0 on success, non-zero on failure.
 */
int csilk_wf_manager_reload(csilk_wf_manager_t* mgr, const char* name, csilk_wf_t* new_wf);

/**
 * @brief Retrieves the active workflow instance by name.
 * @param mgr The manager instance.
 * @param name Workflow registry key name.
 * @return Pointer to active workflow instance, or NULL if not found.
 */
csilk_wf_t* csilk_wf_manager_get(csilk_wf_manager_t* mgr, const char* name);

/**
 * @brief Binds WebSocket live step debugging endpoints to a csilk app.
 * @param mgr The manager instance.
 * @param app The csilk application instance.
 * @param route_path Route path for debugging WebSocket (e.g. "/api/v1/workflows/debug").
 * @return 0 on success, non-zero on failure.
 */
int csilk_wf_manager_enable_debug_server(csilk_wf_manager_t* mgr,
                                         csilk_app_t*        app,
                                         const char*         route_path);

#ifdef __cplusplus
}
#endif

#endif /* CSILK_WORKFLOW_DSL_H */
