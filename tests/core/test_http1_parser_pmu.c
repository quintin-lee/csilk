/**
 * @file test_http1_parser_pmu.c
 * @brief Independent PMU measurements for HTTP/1 parser workloads.
 *
 * Linux-only: requires perf_event_open. On other platforms, prints skip.
 */

#ifndef __linux__
#include <stdio.h>
int
main(void)
{
    printf("HTTP1_PMU: skipped (Linux-only perf_event_open)\n");
    return 0;
}
#else

#include <errno.h>
#include <inttypes.h>
#include <linux/perf_event.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <llhttp.h>

#define PMU_ITERS 100000U

typedef struct {
    int fd_cycles;
    int fd_instructions;
    int fd_branches;
    int fd_branch_misses;
    int fd_cache_misses;
} pmu_t;

typedef struct {
    uint64_t cycles;
    uint64_t instructions;
    uint64_t branches;
    uint64_t branch_misses;
    uint64_t cache_misses;
} result_t;

static volatile size_t sink;

static int
open_event(uint32_t type, uint64_t config)
{
    struct perf_event_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.type = type;
    attr.size = sizeof(attr);
    attr.config = config;
    attr.disabled = 1;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    return (int)syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0);
}

static void
pmu_init(pmu_t* pmu)
{
    pmu->fd_cycles = open_event(PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES);
    pmu->fd_instructions = open_event(PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS);
    pmu->fd_branches = open_event(PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_INSTRUCTIONS);
    pmu->fd_branch_misses = open_event(PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES);
    pmu->fd_cache_misses = open_event(PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES);
}

static void
pmu_start(pmu_t* pmu)
{
    int fds[] = {pmu->fd_cycles,
                 pmu->fd_instructions,
                 pmu->fd_branches,
                 pmu->fd_branch_misses,
                 pmu->fd_cache_misses};
    for (size_t i = 0; i < sizeof(fds) / sizeof(fds[0]); i++) {
        if (fds[i] >= 0) {
            ioctl(fds[i], PERF_EVENT_IOC_RESET, 0);
            ioctl(fds[i], PERF_EVENT_IOC_ENABLE, 0);
        }
    }
}

static uint64_t
read_event(int fd)
{
    uint64_t value = 0;
    if (fd >= 0 && read(fd, &value, sizeof(value)) == sizeof(value)) {
        return value;
    }
    return 0;
}

static result_t
pmu_stop(pmu_t* pmu)
{
    result_t result = {
        .cycles = read_event(pmu->fd_cycles),
        .instructions = read_event(pmu->fd_instructions),
        .branches = read_event(pmu->fd_branches),
        .branch_misses = read_event(pmu->fd_branch_misses),
        .cache_misses = read_event(pmu->fd_cache_misses),
    };
    int fds[] = {pmu->fd_cycles,
                 pmu->fd_instructions,
                 pmu->fd_branches,
                 pmu->fd_branch_misses,
                 pmu->fd_cache_misses};
    for (size_t i = 0; i < sizeof(fds) / sizeof(fds[0]); i++) {
        if (fds[i] >= 0) {
            ioctl(fds[i], PERF_EVENT_IOC_DISABLE, 0);
        }
    }
    return result;
}

static void
pmu_close(pmu_t* pmu)
{
    int fds[] = {pmu->fd_cycles,
                 pmu->fd_instructions,
                 pmu->fd_branches,
                 pmu->fd_branch_misses,
                 pmu->fd_cache_misses};
    for (size_t i = 0; i < sizeof(fds) / sizeof(fds[0]); i++) {
        if (fds[i] >= 0) {
            close(fds[i]);
        }
    }
}

static int
on_data(llhttp_t* parser, const char* data, size_t length)
{
    (void)parser;
    if (data && length > 0) {
        sink += (unsigned char)data[0];
    }
    sink += length;
    return 0;
}

static int
on_complete(llhttp_t* parser)
{
    (void)parser;
    sink++;
    return 0;
}

static size_t
build_header_workload(char* buffer, size_t capacity, size_t header_count)
{
    int written = snprintf(buffer, capacity, "GET /headers HTTP/1.1\r\nHost: localhost\r\n");
    if (written < 0 || (size_t)written >= capacity) {
        return 0;
    }
    size_t length = (size_t)written;
    for (size_t i = 0; i < header_count; i++) {
        written = snprintf(buffer + length, capacity - length, "X-Header-%zu: value\r\n", i);
        if (written < 0 || (size_t)written >= capacity - length) {
            return 0;
        }
        length += (size_t)written;
    }
    if (capacity - length < 3) {
        return 0;
    }
    memcpy(buffer + length, "\r\n", 3);
    return length + 2;
}

static void
run_workload(const char* name, const char* request, size_t length, int fragmented)
{
    llhttp_settings_t settings;
    llhttp_settings_init(&settings);
    settings.on_url = on_data;
    settings.on_header_field = on_data;
    settings.on_header_value = on_data;
    settings.on_body = on_data;
    settings.on_message_complete = on_complete;
    pmu_t pmu;
    pmu_init(&pmu);
    pmu_start(&pmu);
    for (size_t i = 0; i < PMU_ITERS; i++) {
        llhttp_t parser;
        llhttp_init(&parser, HTTP_REQUEST, &settings);
        if (fragmented) {
            size_t offset = 0;
            while (offset < length) {
                size_t chunk = (length - offset > 11) ? 11 : length - offset;
                if (llhttp_execute(&parser, request + offset, chunk) != HPE_OK) {
                    abort();
                }
                offset += chunk;
            }
        } else if (llhttp_execute(&parser, request, length) != HPE_OK) {
            abort();
        }
    }
    result_t result = pmu_stop(&pmu);
    printf("HTTP1_PMU workload=%s operations=%u cycles_per_op=%.2f instructions_per_op=%.2f"
           " branches_per_op=%.2f branch_misses_per_op=%.2f cache_misses_per_op=%.2f\n",
           name,
           PMU_ITERS,
           (double)result.cycles / PMU_ITERS,
           (double)result.instructions / PMU_ITERS,
           (double)result.branches / PMU_ITERS,
           (double)result.branch_misses / PMU_ITERS,
           (double)result.cache_misses / PMU_ITERS);
    pmu_close(&pmu);
}

int
main(void)
{
    static const char   tiny[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    static const char   json[] = "POST /api HTTP/1.1\r\nHost: localhost\r\n"
                                 "Content-Type: application/json\r\nContent-Length: 13\r\n\r\n"
                                 "{\"ok\":true}";
    static const char   body[] = "POST /upload HTTP/1.1\r\nHost: localhost\r\n"
                                 "Content-Length: 64\r\n\r\n"
                                 "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    static const char   headers[] = "GET /headers HTTP/1.1\r\nHost: localhost\r\n"
                                    "X-One: 1\r\nX-Two: 2\r\nX-Three: 3\r\nX-Four: 4\r\n"
                                    "X-Five: 5\r\nX-Six: 6\r\nX-Seven: 7\r\nX-Eight: 8\r\n\r\n";
    static const size_t header_counts[] = {5, 10, 20, 50};
    char                header_matrix[4096];

    run_workload("tiny", tiny, sizeof(tiny) - 1, 0);
    run_workload("json", json, sizeof(json) - 1, 0);
    run_workload("body", body, sizeof(body) - 1, 0);
    run_workload("fragmented", headers, sizeof(headers) - 1, 1);
    run_workload("headers_8", headers, sizeof(headers) - 1, 0);
    for (size_t i = 0; i < sizeof(header_counts) / sizeof(header_counts[0]); i++) {
        size_t length =
            build_header_workload(header_matrix, sizeof(header_matrix), header_counts[i]);
        if (length == 0) {
            return 1;
        }
        char name[32];
        snprintf(name, sizeof(name), "headers_%zu", header_counts[i]);
        run_workload(name, header_matrix, length, 0);
    }
    printf("HTTP1_PMU sink=%zu\n", sink);
    return 0;
}

#endif /* __linux__ */
