#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#include "csilk/csilk.h"
#include "csilk/core/internal.h"
#include "core/internal/srv_internal.h"
#include "core/internal/srv_impl.h"
#include "csilk/http/h2.h"
#ifdef CSILK_POOL_STATS
#include "core/internal/pool_stats.h"
#endif

static uint64_t
now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static long
rss_kib(void)
{
#ifdef __linux__
    FILE* status = fopen("/proc/self/status", "r");
    if (!status) {
        return -1;
    }
    char line[128];
    long rss = -1;
    while (fgets(line, sizeof(line), status)) {
        if (sscanf(line, "VmRSS: %ld kB", &rss) == 1) {
            break;
        }
    }
    fclose(status);
    return rss;
#else
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return -1;
    }
    return usage.ru_maxrss;
#endif
}
static void
print_pool_counts(const worker_pool_t* wp, const char* phase)
{
    int read_total = 0;
    for (int tier = 0; tier < CSILK_READ_BUF_TIER_COUNT; tier++) {
        read_total += atomic_load_explicit(&wp->read_buf_counts[tier], memory_order_relaxed);
    }
    printf("POOL phase=%s client_retained=%d arena_retained=%d read_retained=%d "
           "read_tiers=[%d,%d,%d] rss_max_kib=%ld\n",
           phase,
           atomic_load_explicit(&wp->client_pool_count, memory_order_relaxed),
           atomic_load_explicit(&wp->arena_pool_count, memory_order_relaxed),
           read_total,
           atomic_load_explicit(&wp->read_buf_counts[0], memory_order_relaxed),
           atomic_load_explicit(&wp->read_buf_counts[1], memory_order_relaxed),
           atomic_load_explicit(&wp->read_buf_counts[2], memory_order_relaxed),
           rss_kib());
}

static void
free_worker_pool_storage(worker_pool_t* wp)
{
    int client_count = atomic_load_explicit(&wp->client_pool_count, memory_order_relaxed);
    for (int i = 0; i < client_count; i++) {
        free(wp->client_pool[i]);
        wp->client_pool[i] = NULL;
    }
    atomic_store_explicit(&wp->client_pool_count, 0, memory_order_relaxed);
    int arena_count = atomic_load_explicit(&wp->arena_pool_count, memory_order_relaxed);
    for (int i = 0; i < arena_count; i++) {
        if (wp->arena_pool[i]) {
            csilk_arena_free(wp->arena_pool[i]);
            wp->arena_pool[i] = NULL;
        }
    }
    atomic_store_explicit(&wp->arena_pool_count, 0, memory_order_relaxed);
    for (int tier = 0; tier < CSILK_READ_BUF_TIER_COUNT; tier++) {
        int count = atomic_load_explicit(&wp->read_buf_counts[tier], memory_order_relaxed);
        for (int i = 0; i < count; i++) {
            free(wp->read_buf_tiers[tier][i]);
            wp->read_buf_tiers[tier][i] = NULL;
        }
        atomic_store_explicit(&wp->read_buf_counts[tier], 0, memory_order_relaxed);
    }
}

static void
run_stream_workload(int cycles, int concurrent)
{
    csilk_client_t client;
    memset(&client, 0, sizeof(client));
    csilk_ctx_t** streams = calloc((size_t)concurrent, sizeof(*streams));
    assert(streams != NULL);

    int      peak_active = 0;
    int64_t  hit_count = 0;
    int64_t  miss_count = 0;
    uint64_t start = now_ns();
    for (int cycle = 0; cycle < cycles; cycle++) {
        for (int i = 0; i < concurrent; i++) {
            bool pool_was_nonempty = client.h2_stream_map.pool_count > 0;
            streams[i] = csilk_h2_get_or_create_stream(&client, cycle * concurrent + i + 1);
            assert(streams[i] != NULL);
            if (pool_was_nonempty) {
                hit_count++;
            } else {
                miss_count++;
            }
            if ((int)client.h2_stream_map.count > peak_active) {
                peak_active = (int)client.h2_stream_map.count;
            }
        }
        for (int i = 0; i < concurrent; i++) {
            assert(csilk_h2_remove_stream(&client, streams[i]->stream_id) == 0);
        }
    }
    uint64_t elapsed = now_ns() - start;
    printf("STREAM cycles=%d concurrent=%d peak_active=%d retained=%u hit_observations=%" PRId64
           " miss_observations=%" PRId64 " elapsed_ns=%" PRIu64 "\n",
           cycles,
           concurrent,
           peak_active,
           client.h2_stream_map.pool_count,
           hit_count,
           miss_count,
           elapsed);
    csilk_h2_free_streams(&client);
    free(streams);
}

static void
run_arena_workload(worker_pool_t* wp, int requests, size_t allocation_size)
{
    size_t   peak_retained = 0;
    size_t   peak_used = 0;
    uint64_t start = now_ns();
    for (int i = 0; i < requests; i++) {
        csilk_arena_t* arena = pool_get_arena(wp);
        assert(arena != NULL);
        assert(csilk_arena_alloc(arena, allocation_size) != NULL);
        size_t total = 0;
        size_t used = 0;
        csilk_arena_get_stats(arena, &total, &used);
        if (total > peak_retained) {
            peak_retained = total;
        }
        if (used > peak_used) {
            peak_used = used;
        }
        pool_put_arena(wp, arena);
    }
    printf("ARENA requests=%d alloc=%zu peak_retained=%zu peak_used=%zu "
           "retained_count=%d elapsed_ns=%" PRIu64 "\n",
           requests,
           allocation_size,
           peak_retained,
           peak_used,
           atomic_load_explicit(&wp->arena_pool_count, memory_order_relaxed),
           now_ns() - start);
}

static void
run_body_workload(int requests, size_t request_size)
{
    size_t   peak_capacity = 0;
    size_t   total_waste = 0;
    uint64_t start = now_ns();
    for (int i = 0; i < requests; i++) {
        size_t capacity = 0;
        void*  body = csilk_body_alloc(request_size, &capacity);
        assert(body != NULL);
        assert(capacity >= request_size);
        peak_capacity = capacity > peak_capacity ? capacity : peak_capacity;
        total_waste += capacity - request_size;
        csilk_body_free(body, capacity);
    }
    printf("BODY requests=%d requested=%zu peak_capacity=%zu avg_waste=%zu "
           "elapsed_ns=%" PRIu64 "\n",
           requests,
           request_size,
           peak_capacity,
           total_waste / (size_t)requests,
           now_ns() - start);
    csilk_body_pool_cleanup();
}

int
main(int argc, char** argv)
{
    int workers = 1;
    if (argc == 3 && strcmp(argv[1], "--workers") == 0) {
        workers = atoi(argv[2]);
    }
    if (workers < 1 || workers > 256) {
        fprintf(stderr, "usage: %s [--workers N]\n", argv[0]);
        return EXIT_FAILURE;
    }

    printf("=== CSilk Pool Economics Baseline ===\n");
#ifdef CSILK_POOL_STATS
    csilk_pool_stats_reset();
    csilk_pool_stats_enable(true);
#endif
    printf("CONFIG stream_pool_max=%d client_pool_size=%d read_pool_size=%d "
           "body_tiers=%d body_max_per_tier=%d\n",
           CSILK_H2_STREAM_POOL_MAX,
           CSILK_CLIENT_POOL_SIZE,
           CSILK_READ_BUF_POOL_SIZE,
           CSILK_BODY_POOL_TIER_COUNT,
           CSILK_BODY_POOL_MAX_PER_TIER);
    size_t read_retention_per_worker =
        (size_t)CSILK_READ_BUF_POOL_SIZE *
        (CSILK_READ_BUF_4KB + CSILK_READ_BUF_16KB + CSILK_READ_BUF_64KB);
    size_t body_retention_per_thread =
        (size_t)CSILK_BODY_POOL_MAX_PER_TIER *
        (CSILK_BODY_POOL_64KB + CSILK_BODY_POOL_128KB + CSILK_BODY_POOL_256KB +
         CSILK_BODY_POOL_512KB + CSILK_BODY_POOL_1MB);
    printf("RETENTION workers=%d read_max_per_worker=%zu read_max_total=%zu "
           "body_max_per_thread=%zu body_max_total=%zu\n",
           workers,
           read_retention_per_worker,
           read_retention_per_worker * (size_t)workers,
           body_retention_per_thread,
           body_retention_per_thread * (size_t)workers);

    csilk_server_t server;
    memset(&server, 0, sizeof(server));
    worker_pool_t worker;
    memset(&worker, 0, sizeof(worker));
    _csilk_worker_pool_atomics_init(&worker, &server, 0);
    _csilk_worker_set_current_pool(&worker);

    print_pool_counts(&worker, "cold");
    run_stream_workload(1000, 1);
    run_stream_workload(1000, 64);
    run_arena_workload(&worker, 1000, 1024);
    run_arena_workload(&worker, 100, 1024 * 1024);
    run_arena_workload(&worker, 1000, 1024);
    run_body_workload(1000, 1024);
    run_body_workload(1000, 4096);
    run_body_workload(1000, 16384);
    run_body_workload(1000, 32768);
    run_body_workload(1000, 65536);
    run_body_workload(1000, 65537);
    run_body_workload(1000, 131072);
    run_body_workload(1000, 262144);
    run_body_workload(1000, 524288);
    run_body_workload(1000, 1048576);
    run_body_workload(100, 1048577);
    print_pool_counts(&worker, "warm");
    _csilk_worker_init_arena_pool(&worker);
    _csilk_worker_init_read_buf_pool(&worker);
    print_pool_counts(&worker, "preallocated");
    free_worker_pool_storage(&worker);
    print_pool_counts(&worker, "after_cleanup");
    /* Clear the TLS worker-pool pointer before the stack frame dies — otherwise the
     * address of the just-destroyed stack `worker` remains latched in TLS and would be a
     * use-after-free if another test in the same process later reads it. Also release the
     * arena chunk free-list cached on this thread. */
    _csilk_worker_set_current_pool(NULL);
    csilk_arena_flush_free_list();
#ifdef CSILK_POOL_STATS
    csilk_pool_stats_print(stdout);
#endif
    printf("=== Pool Economics Complete ===\n");
    return 0;
}
