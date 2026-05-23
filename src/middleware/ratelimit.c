#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include "gin.h"

#define MAX_IP_ENTRIES 1024
#define WINDOW_SIZE 60 // 1 minute

typedef struct {
    char ip[46];
    int count;
    time_t last_reset;
} ip_entry_t;

static ip_entry_t ip_table[MAX_IP_ENTRIES];
static int ip_count = 0;

void gin_rate_limit_middleware(gin_ctx_t* c, int limit) {
    const char* ip = gin_get_client_ip(c);
    if (!ip) {
        gin_next(c);
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
            gin_next(c);
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
        gin_set_header(c, "Retry-After", "60");
        gin_json_error(c, 429, "Too Many Requests");
        gin_abort(c);
    } else {
        gin_next(c);
    }
}
