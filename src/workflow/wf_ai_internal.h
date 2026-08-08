/**
 * @file wf_ai_internal.h
 * @brief Internal declarations for cross-file AI workflow functions.
 *
 * Shared between wf_ai_nodes.c and wf_ai_agents.c.
 *
 * @copyright MIT License
 */

#ifndef CSILK_WF_AI_INTERNAL_H
#define CSILK_WF_AI_INTERNAL_H

#include "csilk/csilk.h"

char* _csilk_json_get_path(csilk_wf_ctx_t* ctx, cJSON* root, const char* path);

csilk_data_t* ai_node_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data);

#endif
