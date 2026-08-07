/**
 * @file app_internal.h
 * @brief Internal definition of struct csilk_app_s for cross-file use.
 *
 * Shared between app.c and app_routes.c.
 *
 * @copyright MIT License
 */

#ifndef CSILK_APP_INTERNAL_H
#define CSILK_APP_INTERNAL_H

#include "csilk/csilk.h"

/** @brief Maximum number of route groups per app. */
enum { CSILK_MAX_GROUPS = 32 };

/** @brief Internal: cached route group lookup entry for fast prefix-to-group
 * mapping. */
typedef struct {
    char           prefix[128];
    csilk_group_t* group;
} cached_group_t;

/** @brief Main application structure containing config, router, server, and
 * groups.
 *
 * Lifecycle: created in csilk_app_new(), destroyed in csilk_app_free().
 * Ownership: owns everything except the OpenAPI router pointer (global). */
struct csilk_app_s {
    csilk_config_t  config;
    csilk_router_t* router;
    csilk_server_t* server;
    csilk_group_t*  root_group;
    cached_group_t  groups[CSILK_MAX_GROUPS];
    int             group_count;
};

csilk_group_t* find_or_create_group(csilk_app_t* app, const char* prefix);

#endif
