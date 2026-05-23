/**
 * @file ratelimit.c
 * @brief Simple IP-based rate limiting middleware implementation.
 * MIT License
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include "csilk.h"

/** @brief Maximum number of IP addresses to track. */
#define MAX_IP_ENTRIES 1024
/** @brief Rate limiting window size in seconds. */
#define WINDOW_SIZE 60

typedef struct {
    char ip[46];       /**< Client IP address string. */
    int count;         /**< Request count in current window. */
    time_t last_reset; /**< Timestamp when the window started. */
} ip_entry_t;

static ip_entry_t ip_table[MAX_IP_ENTRIES];
static int ip_count = 0;

/** @brief Simple IP-based rate limiting middleware.
 * Limits the number of requests per IP within a rolling time window.
 * @param c The request context.
 * @param limit Maximum requests allowed per window. */
void csilk_rate_limit_middleware(csilk_ctx_t* c, int limit) {
    const char* ip = csilk_get_client_ip(c);
    if (!ip) {
        csilk_next(c);
        return;
    }

    time_t now = time(NULL);
    ip_entry_t* entry = NULL;

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
            strncpy(entry->ip, ip, sizeof(entry->ip));
            entry->count = 0;
            entry->last_reset = now;
        } else {
            // Table full, just allow for now (or implement eviction)
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

    if (entry->count > limit) {
        csilk_set_header(c, "Retry-After", "60");
        csilk_json_error(c, 429, "Too Many Requests");
        csilk_abort(c);
    } else {
        csilk_next(c);
    }
}
