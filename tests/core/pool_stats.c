#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "core/internal/pool_stats.h"

static _Atomic bool     g_enabled;
static _Atomic uint64_t g_values[CSILK_POOL_STAT_COUNT][6];

void
csilk_pool_stats_reset(void)
{
    memset(g_values, 0, sizeof(g_values));
}

void
csilk_pool_stats_enable(bool enabled)
{
    atomic_store_explicit(&g_enabled, enabled, memory_order_relaxed);
}

void
csilk_pool_stats_record_get(csilk_pool_stat_kind_t kind, bool hit)
{
    if (!atomic_load_explicit(&g_enabled, memory_order_relaxed) || kind >= CSILK_POOL_STAT_COUNT) {
        return;
    }
    if (hit) {
        atomic_fetch_add_explicit(&g_values[kind][0], 1, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&g_values[kind][1], 1, memory_order_relaxed);
    }
}

void
csilk_pool_stats_record_alloc(csilk_pool_stat_kind_t kind)
{
    if (atomic_load_explicit(&g_enabled, memory_order_relaxed) && kind < CSILK_POOL_STAT_COUNT) {
        atomic_fetch_add_explicit(&g_values[kind][2], 1, memory_order_relaxed);
    }
}

void
csilk_pool_stats_record_free(csilk_pool_stat_kind_t kind)
{
    if (atomic_load_explicit(&g_enabled, memory_order_relaxed) && kind < CSILK_POOL_STAT_COUNT) {
        atomic_fetch_add_explicit(&g_values[kind][3], 1, memory_order_relaxed);
    }
}

void
csilk_pool_stats_set_retained(csilk_pool_stat_kind_t kind, uint64_t count)
{
    if (atomic_load_explicit(&g_enabled, memory_order_relaxed) && kind < CSILK_POOL_STAT_COUNT) {
        atomic_store_explicit(&g_values[kind][4], count, memory_order_relaxed);
        uint64_t peak = atomic_load_explicit(&g_values[kind][5], memory_order_relaxed);
        while (count > peak &&
               !atomic_compare_exchange_weak_explicit(
                   &g_values[kind][5], &peak, count, memory_order_relaxed, memory_order_relaxed)) {
        }
    }
}

void
csilk_pool_stats_print(FILE* out)
{
    static const char* names[CSILK_POOL_STAT_COUNT] = {
        "stream", "arena", "read_buffer", "body_buffer", "connection", "dispatch"};
    for (int i = 0; i < CSILK_POOL_STAT_COUNT; i++) {
        fprintf(out,
                "POOL_STATS kind=%s hit=%" PRIu64 " miss=%" PRIu64 " alloc=%" PRIu64
                " os_free=%" PRIu64 " retained=%" PRIu64 " retained_peak=%" PRIu64 "\n",
                names[i],
                atomic_load_explicit(&g_values[i][0], memory_order_relaxed),
                atomic_load_explicit(&g_values[i][1], memory_order_relaxed),
                atomic_load_explicit(&g_values[i][2], memory_order_relaxed),
                atomic_load_explicit(&g_values[i][3], memory_order_relaxed),
                atomic_load_explicit(&g_values[i][4], memory_order_relaxed),
                atomic_load_explicit(&g_values[i][5], memory_order_relaxed));
    }
}
