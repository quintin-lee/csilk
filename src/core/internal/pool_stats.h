#ifndef CSILK_POOL_STATS_H
#define CSILK_POOL_STATS_H

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>

typedef enum {
    CSILK_POOL_STAT_STREAM = 0,
    CSILK_POOL_STAT_ARENA,
    CSILK_POOL_STAT_READ_BUFFER,
    CSILK_POOL_STAT_BODY_BUFFER,
    CSILK_POOL_STAT_CONNECTION,
    CSILK_POOL_STAT_DISPATCH,
    CSILK_POOL_STAT_COUNT
} csilk_pool_stat_kind_t;

typedef struct {
    uint64_t get_hit;
    uint64_t get_miss;
    uint64_t alloc_count;
    uint64_t free_count;
    uint64_t retained_count;
    uint64_t retained_peak;
} csilk_pool_stat_t;

void csilk_pool_stats_reset(void);
void csilk_pool_stats_enable(bool enabled);
void csilk_pool_stats_record_get(csilk_pool_stat_kind_t kind, bool hit);
void csilk_pool_stats_record_alloc(csilk_pool_stat_kind_t kind);
void csilk_pool_stats_record_free(csilk_pool_stat_kind_t kind);
void csilk_pool_stats_set_retained(csilk_pool_stat_kind_t kind, uint64_t count);
void csilk_pool_stats_print(FILE* out);

#endif
