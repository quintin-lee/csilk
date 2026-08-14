/**
 * @file workflow_manager.c
 * @brief Workflow manager: a registry of named, versioned workflows that
 *        supports hot reload and thread-safe lookup.
 *
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/app/workflow_dsl.h"
#include "csilk/core/sync.h"
#include "workflow_internal.h"

typedef struct {
    char          name[128];
    uint32_t      version;
    csilk_wf_t*   active_wf;
    csilk_mutex_t mutex;
} csilk_wf_managed_entry_t;

struct csilk_wf_manager_s {
    csilk_wf_managed_entry_t entries[64];
    size_t                   count;
    csilk_mutex_t            mutex;
};

/**
 * @brief Creates a new, empty workflow manager.
 *
 * Allocates and zero-initializes the manager and its lock.
 *
 * @return A new csilk_wf_manager_t, or NULL on allocation failure.
 */
csilk_wf_manager_t*
csilk_wf_manager_new(void)
{
    csilk_wf_manager_t* mgr = (csilk_wf_manager_t*)calloc(1, sizeof(csilk_wf_manager_t));
    if (!mgr) {
        return NULL;
    }

    csilk_mutex_init(&mgr->mutex);
    return mgr;
}

/**
 * @brief Destroys a workflow manager and frees all registered workflows.
 *
 * Tears down each entry's workflow (csilk_wf_free) and destroys all mutexes.
 *
 * @param mgr The workflow manager to free (may be NULL).
 */
void
csilk_wf_manager_free(csilk_wf_manager_t* mgr)
{
    if (!mgr) {
        return;
    }

    csilk_mutex_lock(&mgr->mutex);
    for (size_t i = 0; i < mgr->count; i++) {
        csilk_mutex_lock(&mgr->entries[i].mutex);
        if (mgr->entries[i].active_wf) {
            csilk_wf_free(mgr->entries[i].active_wf);
        }
        csilk_mutex_unlock(&mgr->entries[i].mutex);
        csilk_mutex_destroy(&mgr->entries[i].mutex);
    }
    csilk_mutex_unlock(&mgr->mutex);
    csilk_mutex_destroy(&mgr->mutex);
    free(mgr);
}

/**
 * @brief Registers a workflow under a unique name in the manager.
 *
 * Stores wf as the active workflow for name, starting at version 1. Names must
 * be unique; the fixed 64-entry capacity is enforced.
 *
 * @param mgr The workflow manager (must not be NULL).
 * @param name Unique workflow name (must not be NULL).
 * @param wf  Workflow instance to register (must not be NULL).
 * @return 0 on success, or -1 if an argument is NULL, the manager is full, or
 *         the name already exists.
 * @note Thread-safe (guarded by mgr->mutex).
 */
int
csilk_wf_manager_register(csilk_wf_manager_t* mgr, const char* name, csilk_wf_t* wf)
{
    if (!mgr || !name || !wf) {
        return -1;
    }

    csilk_mutex_lock(&mgr->mutex);
    if (mgr->count >= 64) {
        csilk_mutex_unlock(&mgr->mutex);
        return -1;
    }

    for (size_t i = 0; i < mgr->count; i++) {
        if (strcmp(mgr->entries[i].name, name) == 0) {
            csilk_mutex_unlock(&mgr->mutex);
            return -1;
        }
    }

    csilk_wf_managed_entry_t* entry = &mgr->entries[mgr->count++];
    snprintf(entry->name, sizeof(entry->name), "%s", name);
    entry->version = 1;
    entry->active_wf = wf;
    csilk_mutex_init(&entry->mutex);

    csilk_mutex_unlock(&mgr->mutex);
    return 0;
}

/**
 * @brief Hot-swaps the active workflow for a registered name.
 *
 * Replaces the existing workflow with new_wf, increments the entry version,
 * and frees the previously active workflow. The name must already be registered.
 *
 * @param mgr    The workflow manager (must not be NULL).
 * @param name   Name of the workflow to reload (must not be NULL).
 * @param new_wf Replacement workflow instance (must not be NULL).
 * @return 0 on success, or -1 if an argument is NULL or the name is unknown.
 * @note Thread-safe; the swap and old-workflow teardown are guarded by the
 *       manager and entry mutexes.
 */
int
csilk_wf_manager_reload(csilk_wf_manager_t* mgr, const char* name, csilk_wf_t* new_wf)
{
    if (!mgr || !name || !new_wf) {
        return -1;
    }

    csilk_mutex_lock(&mgr->mutex);
    csilk_wf_managed_entry_t* target = NULL;
    for (size_t i = 0; i < mgr->count; i++) {
        if (strcmp(mgr->entries[i].name, name) == 0) {
            target = &mgr->entries[i];
            break;
        }
    }

    if (!target) {
        csilk_mutex_unlock(&mgr->mutex);
        return -1;
    }

    csilk_mutex_lock(&target->mutex);
    csilk_wf_t* old_wf = target->active_wf;
    target->active_wf = new_wf;
    target->version++;
    csilk_mutex_unlock(&target->mutex);

    if (old_wf) {
        csilk_wf_free(old_wf);
    }

    csilk_mutex_unlock(&mgr->mutex);
    return 0;
}

/**
 * @brief Looks up the active workflow registered under a name.
 *
 * @param mgr  The workflow manager (must not be NULL).
 * @param name Name to look up (must not be NULL).
 * @return The active csilk_wf_t, or NULL if not found.
 * @note Thread-safe (guarded by mgr->mutex and the entry mutex). The returned
 *       pointer is borrowed; it remains valid until a reload/free of that entry.
 */
csilk_wf_t*
csilk_wf_manager_get(csilk_wf_manager_t* mgr, const char* name)
{
    if (!mgr || !name) {
        return NULL;
    }

    csilk_mutex_lock(&mgr->mutex);
    csilk_wf_t* result = NULL;
    for (size_t i = 0; i < mgr->count; i++) {
        if (strcmp(mgr->entries[i].name, name) == 0) {
            csilk_mutex_lock(&mgr->entries[i].mutex);
            result = mgr->entries[i].active_wf;
            csilk_mutex_unlock(&mgr->entries[i].mutex);
            break;
        }
    }
    csilk_mutex_unlock(&mgr->mutex);
    return result;
}

extern int csilk_wf_manager_enable_debug_server_impl(csilk_wf_manager_t* mgr,
                                                     csilk_app_t*        app,
                                                     const char*         route_path);

/**
 * @brief Enables the workflow debug server route for the manager.
 *
 * Thin wrapper that forwards to csilk_wf_manager_enable_debug_server_impl().
 *
 * @param mgr        The workflow manager (may be NULL).
 * @param app        The csilk application to register the route on (may be NULL).
 * @param route_path Optional custom route path (may be NULL).
 * @return 0 on success, or -1 on failure.
 */
int
csilk_wf_manager_enable_debug_server(csilk_wf_manager_t* mgr,
                                     csilk_app_t*        app,
                                     const char*         route_path)
{
    return csilk_wf_manager_enable_debug_server_impl(mgr, app, route_path);
}
