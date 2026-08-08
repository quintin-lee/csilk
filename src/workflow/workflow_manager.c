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

csilk_wf_manager_t*
csilk_wf_manager_new(void)
{
    csilk_wf_manager_t* mgr = (csilk_wf_manager_t*)calloc(1, sizeof(csilk_wf_manager_t));
    if (!mgr) {
        return nullptr;
    }

    csilk_mutex_init(&mgr->mutex);
    return mgr;
}

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

int
csilk_wf_manager_reload(csilk_wf_manager_t* mgr, const char* name, csilk_wf_t* new_wf)
{
    if (!mgr || !name || !new_wf) {
        return -1;
    }

    csilk_mutex_lock(&mgr->mutex);
    csilk_wf_managed_entry_t* target = nullptr;
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

csilk_wf_t*
csilk_wf_manager_get(csilk_wf_manager_t* mgr, const char* name)
{
    if (!mgr || !name) {
        return nullptr;
    }

    csilk_mutex_lock(&mgr->mutex);
    csilk_wf_t* result = nullptr;
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

int
csilk_wf_manager_enable_debug_server(csilk_wf_manager_t* mgr,
                                     csilk_app_t*        app,
                                     const char*         route_path)
{
    return csilk_wf_manager_enable_debug_server_impl(mgr, app, route_path);
}
