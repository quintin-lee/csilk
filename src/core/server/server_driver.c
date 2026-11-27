/**
 * @file server_driver.c
 * @brief Pluggable driver injection & runtime observability collector registry.
 *
 * Provides the public setter API for injecting driver implementations into a
 * server instance, plus the internal process-level registry used by optional
 * subsystems (MQ, AI, DB) to register Prometheus metrics collectors and admin
 * stats/monitor hooks without upward link dependencies.
 *
 * @copyright MIT License
 */

#include <stdlib.h>
#include <string.h>

#include "csilk/core/server/server.h"
#include "csilk/core/sync.h"
#include "../internal/srv_internal.h"

typedef struct {
    const char*              name;
    csilk_metrics_collect_fn fn;
} obs_metrics_entry_t;

typedef struct {
    const char*          name;
    csilk_admin_stats_fn fn;
} obs_admin_stats_entry_t;

typedef struct {
    const char*            name;
    csilk_admin_monitor_fn on_open;
    csilk_admin_monitor_fn on_close;
} obs_admin_monitor_entry_t;

typedef struct {
    const char*          name;
    csilk_ready_check_fn fn;
} obs_ready_check_entry_t;

static csilk_once_t                            g_obs_reg_once = CSILK_ONCE_INIT;
static csilk_mutex_t                           g_obs_reg_mutex;
static obs_metrics_entry_t                     g_metrics_collectors[CSILK_OBS_MAX_COLLECTORS];
static size_t                                  g_metrics_collector_count = 0;
static obs_admin_stats_entry_t                 g_admin_stats[CSILK_OBS_MAX_COLLECTORS];
static size_t                                  g_admin_stats_count = 0;
static obs_admin_monitor_entry_t               g_admin_monitors[CSILK_OBS_MAX_COLLECTORS];
static size_t                                  g_admin_monitor_count = 0;
static obs_ready_check_entry_t                 g_ready_checks[CSILK_OBS_MAX_COLLECTORS];
static size_t                                  g_ready_check_count = 0;
static _Atomic(const csilk_h3_response_ops_t*) g_h3_response_ops = nullptr;
static _Atomic(const csilk_mq_runtime_ops_t*)  g_mq_runtime_ops = nullptr;

static void
obs_reg_init_once(void)
{
    csilk_mutex_init(&g_obs_reg_mutex);
}

static inline void
obs_reg_ensure_init(void)
{
    csilk_once(&g_obs_reg_once, obs_reg_init_once);
}

/* --- Driver injection --- */

/** @brief Set the pluggable storage driver. */
void
csilk_server_set_storage_driver(csilk_server_t* server, csilk_storage_driver_t* driver)
{
    if (server) {
        server->storage_driver = driver;
    }
}

/** @brief Set the pluggable cryptographic driver. */
void
csilk_server_set_crypto_driver(csilk_server_t* server, csilk_crypto_driver_t* driver)
{
    if (server) {
        server->crypto_driver = driver;
    }
}

/** @brief Set the pluggable cipher driver. */
void
csilk_server_set_cipher_driver(csilk_server_t* server, csilk_cipher_driver_t* driver)
{
    if (server) {
        server->cipher_driver = driver;
    }
}

/* --- Runtime observability collector registry --- */

CSILK_INTERNAL int
_csilk_metrics_register_collector(const char* name, csilk_metrics_collect_fn fn)
{
    if (!name || !name[0] || !fn) {
        return -1;
    }
    obs_reg_ensure_init();
    csilk_mutex_lock(&g_obs_reg_mutex);
    for (size_t i = 0; i < g_metrics_collector_count; i++) {
        if (strcmp(g_metrics_collectors[i].name, name) == 0) {
            g_metrics_collectors[i].fn = fn;
            csilk_mutex_unlock(&g_obs_reg_mutex);
            return 0;
        }
    }
    if (g_metrics_collector_count >= CSILK_OBS_MAX_COLLECTORS) {
        csilk_mutex_unlock(&g_obs_reg_mutex);
        return -1;
    }
    g_metrics_collectors[g_metrics_collector_count++] = (obs_metrics_entry_t){
        .name = name,
        .fn = fn,
    };
    csilk_mutex_unlock(&g_obs_reg_mutex);
    return 0;
}

CSILK_INTERNAL void
_csilk_metrics_collect_all(csilk_ctx_t* c, csilk_metrics_emit_fn emit, void* writer)
{
    if (!emit || !writer) {
        return;
    }
    obs_reg_ensure_init();
    obs_metrics_entry_t snap[CSILK_OBS_MAX_COLLECTORS];
    csilk_mutex_lock(&g_obs_reg_mutex);
    size_t count = g_metrics_collector_count;
    if (count > 0) {
        memcpy(snap, g_metrics_collectors, count * sizeof(snap[0]));
    }
    csilk_mutex_unlock(&g_obs_reg_mutex);

    for (size_t i = 0; i < count; i++) {
        if (snap[i].fn) {
            snap[i].fn(c, emit, writer);
        }
    }
}

CSILK_INTERNAL int
_csilk_admin_register_stats(const char* name, csilk_admin_stats_fn fn)
{
    if (!name || !name[0] || !fn) {
        return -1;
    }
    obs_reg_ensure_init();
    csilk_mutex_lock(&g_obs_reg_mutex);
    for (size_t i = 0; i < g_admin_stats_count; i++) {
        if (strcmp(g_admin_stats[i].name, name) == 0) {
            g_admin_stats[i].fn = fn;
            csilk_mutex_unlock(&g_obs_reg_mutex);
            return 0;
        }
    }
    if (g_admin_stats_count >= CSILK_OBS_MAX_COLLECTORS) {
        csilk_mutex_unlock(&g_obs_reg_mutex);
        return -1;
    }
    g_admin_stats[g_admin_stats_count++] = (obs_admin_stats_entry_t){
        .name = name,
        .fn = fn,
    };
    csilk_mutex_unlock(&g_obs_reg_mutex);
    return 0;
}

CSILK_INTERNAL void
_csilk_admin_foreach_stats(csilk_admin_stats_visit_fn visitor, void* user_data)
{
    if (!visitor) {
        return;
    }
    obs_reg_ensure_init();
    obs_admin_stats_entry_t snap[CSILK_OBS_MAX_COLLECTORS];
    csilk_mutex_lock(&g_obs_reg_mutex);
    size_t count = g_admin_stats_count;
    if (count > 0) {
        memcpy(snap, g_admin_stats, count * sizeof(snap[0]));
    }
    csilk_mutex_unlock(&g_obs_reg_mutex);

    for (size_t i = 0; i < count; i++) {
        if (snap[i].name && snap[i].fn) {
            visitor(snap[i].name, snap[i].fn, user_data);
        }
    }
}

CSILK_INTERNAL int
_csilk_admin_register_monitor(const char*            name,
                              csilk_admin_monitor_fn on_open,
                              csilk_admin_monitor_fn on_close)
{
    if (!name || !name[0] || (!on_open && !on_close)) {
        return -1;
    }
    obs_reg_ensure_init();
    csilk_mutex_lock(&g_obs_reg_mutex);
    for (size_t i = 0; i < g_admin_monitor_count; i++) {
        if (strcmp(g_admin_monitors[i].name, name) == 0) {
            g_admin_monitors[i].on_open = on_open;
            g_admin_monitors[i].on_close = on_close;
            csilk_mutex_unlock(&g_obs_reg_mutex);
            return 0;
        }
    }
    if (g_admin_monitor_count >= CSILK_OBS_MAX_COLLECTORS) {
        csilk_mutex_unlock(&g_obs_reg_mutex);
        return -1;
    }
    g_admin_monitors[g_admin_monitor_count++] = (obs_admin_monitor_entry_t){
        .name = name,
        .on_open = on_open,
        .on_close = on_close,
    };
    csilk_mutex_unlock(&g_obs_reg_mutex);
    return 0;
}

CSILK_INTERNAL void
_csilk_admin_notify_monitor_open(csilk_ctx_t* c)
{
    if (!c) {
        return;
    }
    obs_reg_ensure_init();
    obs_admin_monitor_entry_t snap[CSILK_OBS_MAX_COLLECTORS];
    csilk_mutex_lock(&g_obs_reg_mutex);
    size_t count = g_admin_monitor_count;
    if (count > 0) {
        memcpy(snap, g_admin_monitors, count * sizeof(snap[0]));
    }
    csilk_mutex_unlock(&g_obs_reg_mutex);

    for (size_t i = 0; i < count; i++) {
        if (snap[i].on_open) {
            snap[i].on_open(c);
        }
    }
}

CSILK_INTERNAL void
_csilk_admin_notify_monitor_close(csilk_ctx_t* c)
{
    if (!c) {
        return;
    }
    obs_reg_ensure_init();
    obs_admin_monitor_entry_t snap[CSILK_OBS_MAX_COLLECTORS];
    csilk_mutex_lock(&g_obs_reg_mutex);
    size_t count = g_admin_monitor_count;
    if (count > 0) {
        memcpy(snap, g_admin_monitors, count * sizeof(snap[0]));
    }
    csilk_mutex_unlock(&g_obs_reg_mutex);

    for (size_t i = 0; i < count; i++) {
        if (snap[i].on_close) {
            snap[i].on_close(c);
        }
    }
}

/* --- Optional HTTP/3 streaming response ops registry --- */

CSILK_INTERNAL void
_csilk_register_h3_response_ops(const csilk_h3_response_ops_t* ops)
{
    atomic_store_explicit(&g_h3_response_ops, ops, memory_order_release);
}

CSILK_INTERNAL const csilk_h3_response_ops_t*
_csilk_get_h3_response_ops(void)
{
    return atomic_load_explicit(&g_h3_response_ops, memory_order_acquire);
}

/* --- Runtime readiness check & MQ ops registry --- */

CSILK_INTERNAL int
_csilk_ready_register_check(const char* name, csilk_ready_check_fn fn)
{
    if (!name || !name[0] || !fn) {
        return -1;
    }
    obs_reg_ensure_init();
    csilk_mutex_lock(&g_obs_reg_mutex);
    for (size_t i = 0; i < g_ready_check_count; i++) {
        if (strcmp(g_ready_checks[i].name, name) == 0) {
            g_ready_checks[i].fn = fn;
            csilk_mutex_unlock(&g_obs_reg_mutex);
            return 0;
        }
    }
    if (g_ready_check_count >= CSILK_OBS_MAX_COLLECTORS) {
        csilk_mutex_unlock(&g_obs_reg_mutex);
        return -1;
    }
    g_ready_checks[g_ready_check_count++] = (obs_ready_check_entry_t){
        .name = name,
        .fn = fn,
    };
    csilk_mutex_unlock(&g_obs_reg_mutex);
    return 0;
}

CSILK_INTERNAL int
_csilk_ready_run_checks(csilk_ctx_t* c, const char** reason_out)
{
    if (reason_out) {
        *reason_out = NULL;
    }
    obs_reg_ensure_init();
    obs_ready_check_entry_t snap[CSILK_OBS_MAX_COLLECTORS];
    csilk_mutex_lock(&g_obs_reg_mutex);
    size_t count = g_ready_check_count;
    if (count > 0) {
        memcpy(snap, g_ready_checks, count * sizeof(snap[0]));
    }
    csilk_mutex_unlock(&g_obs_reg_mutex);

    for (size_t i = 0; i < count; i++) {
        if (snap[i].fn && snap[i].fn(c, reason_out) != 0) {
            return -1;
        }
    }
    return 0;
}

CSILK_INTERNAL void
_csilk_register_mq_runtime_ops(const csilk_mq_runtime_ops_t* ops)
{
    atomic_store_explicit(&g_mq_runtime_ops, ops, memory_order_release);
}

CSILK_INTERNAL const csilk_mq_runtime_ops_t*
_csilk_get_mq_runtime_ops(void)
{
    return atomic_load_explicit(&g_mq_runtime_ops, memory_order_acquire);
}
