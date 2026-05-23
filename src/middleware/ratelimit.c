/**
 * @file ratelimit.c
 * @brief Simple IP-based rate limiting middleware implementation.
 * @copyright MIT License
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <uv.h>
#include "csilk.h"
#include "csilk_internal.h"

/** @brief Maximum number of IP addresses to track. */
#define MAX_IP_ENTRIES 1024
/** @brief Rate limiting window size in seconds. */
#define WINDOW_SIZE 60

/** @brief Rate-limit tracking entry for a single IP address. */
typedef struct {
    char ip[46];       /**< Client IP address string. */
    int count;         /**< Request count in current window. */
    time_t last_reset; /**< Timestamp when the window started. */
} ip_entry_t;

static ip_entry_t ip_table[MAX_IP_ENTRIES];
static int ip_count = 0;
static uv_mutex_t ratelimit_mutex;
static uv_once_t ratelimit_once = UV_ONCE_INIT;

/** @brief Initialize the rate-limiting mutex (called once via uv_once). */
static void init_ratelimit_mutex() {
    uv_mutex_init(&ratelimit_mutex);
}

/** @brief IP-based rate limiting middleware with sliding window. */
void csilk_rate_limit_middleware(csilk_ctx_t* c, int limit) {
    uv_once(&ratelimit_once, init_ratelimit_mutex);

    const char* ip = csilk_get_client_ip(c);
    if (!ip) {
        csilk_next(c);
        return;
    }

    time_t now = time(NULL);
    ip_entry_t* entry = NULL;

    uv_mutex_lock(&ratelimit_mutex);

    // Search for IP
    for (int i = 0; i < ip_count; i++) {
        if (strcmp(ip_table[i].ip, ip) == 0) {
            entry = &ip_table[i];
            break;
        }
    }

    if (!entry) {
        if (ip_count < MAX_IP_ENTRIES) {
            entry = &ip_table[ip_count++];
            strncpy(entry->ip, ip, sizeof(entry->ip) - 1);
            entry->ip[sizeof(entry->ip) - 1] = '\0';
            entry->count = 0;
            entry->last_reset = now;
        } else {
            // Table full, just allow for now (or implement eviction)
            uv_mutex_unlock(&ratelimit_mutex);
            csilk_next(c);
            return;
        }
    }

    // Check window
    if (now - entry->last_reset > WINDOW_SIZE) {
        entry->count = 1;
        entry->last_reset = now;
    } else {
        entry->count++;
    }

    int current_count = entry->count;
    uv_mutex_unlock(&ratelimit_mutex);

    if (current_count > limit) {
        csilk_set_header(c, "Retry-After", "60");
        csilk_json_error(c, CSILK_STATUS_TOO_MANY_REQUESTS, "Too Many Requests");
        csilk_abort(c);
    } else {
        csilk_next(c);
    }
}
