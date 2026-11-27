#ifndef CSILK_OBS_REGISTRY_H
#define CSILK_OBS_REGISTRY_H

/**
 * @file obs_registry.h
 * @brief Internal runtime registry for optional observability collectors and
 *        admin stats/monitor callbacks.
 *
 * Lives in csilk_runtime so optional domain modules (db, ai, mq) can
 * self-register without upward link dependencies on csilk_middleware or
 * csilk_observability, and Web Core / admin handlers can query registered
 * subsystems without #ifdef or __attribute__((weak)) probes.
 *
 * @copyright MIT License
 */

#include <stddef.h>

#include "csilk/core/json/json.h"
#include "csilk/core/types.h"

/** @brief Maximum number of optional subsystem collector/admin slots. */
enum { CSILK_OBS_MAX_COLLECTORS = 8 };

#if defined(__GNUC__) || defined(__clang__)
#define CSILK_OBS_PRINTF_LIKE(fmt_idx, first_arg)                                                  \
    __attribute__((format(printf, fmt_idx, first_arg)))
#else
#define CSILK_OBS_PRINTF_LIKE(fmt_idx, first_arg)
#endif

/** @brief Writer callback passed to Prometheus metrics collectors. */
typedef void (*csilk_metrics_emit_fn)(void* writer, const char* fmt, ...)
    CSILK_OBS_PRINTF_LIKE(2, 3);

/** @brief Collector callback invoked by /metrics for a registered subsystem. */
typedef void (*csilk_metrics_collect_fn)(csilk_ctx_t* c, csilk_metrics_emit_fn emit, void* writer);

/** @brief Stats snapshot callback invoked by /admin/stats for a subsystem. */
typedef csilk_json_t* (*csilk_admin_stats_fn)(csilk_ctx_t* c);

/** @brief Visitor callback for iterating registered admin stats providers. */
typedef void (*csilk_admin_stats_visit_fn)(const char*          name,
                                           csilk_admin_stats_fn fn,
                                           void*                user_data);

/** @brief WebSocket monitor lifecycle callback invoked by /admin/ws. */
typedef void (*csilk_admin_monitor_fn)(csilk_ctx_t* c);

/**
 * @brief Register (idempotently by @p name) a Prometheus metrics collector.
 *
 * @param name Static subsystem identifier (e.g. "mq", "ai", "db").
 * @param fn   Collector callback.
 * @return 0 on success, -1 on invalid arguments or when all slots are full.
 */
CSILK_INTERNAL int _csilk_metrics_register_collector(const char* name, csilk_metrics_collect_fn fn);

/**
 * @brief Invoke all registered Prometheus metrics collectors.
 */
CSILK_INTERNAL void
_csilk_metrics_collect_all(csilk_ctx_t* c, csilk_metrics_emit_fn emit, void* writer);

/**
 * @brief Register (idempotently by @p name) an admin /stats JSON provider.
 */
CSILK_INTERNAL int _csilk_admin_register_stats(const char* name, csilk_admin_stats_fn fn);

/**
 * @brief Invoke @p visitor for each registered admin /stats JSON provider.
 */
CSILK_INTERNAL void _csilk_admin_foreach_stats(csilk_admin_stats_visit_fn visitor, void* user_data);

/**
 * @brief Register (idempotently by @p name) an admin WebSocket monitor hook.
 */
CSILK_INTERNAL int _csilk_admin_register_monitor(const char*            name,
                                                 csilk_admin_monitor_fn on_open,
                                                 csilk_admin_monitor_fn on_close);

/**
 * @brief Notify all registered subsystem monitors when an admin WS opens.
 */
CSILK_INTERNAL void _csilk_admin_notify_monitor_open(csilk_ctx_t* c);

/**
 * @brief Notify all registered subsystem monitors when an admin WS closes.
 */
CSILK_INTERNAL void _csilk_admin_notify_monitor_close(csilk_ctx_t* c);

#endif /* CSILK_OBS_REGISTRY_H */
