/**
 * @file ratelimit.c
 * @brief Simple IP-based rate limiting middleware implementation.
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <uv.h>

#include "context_internal.h"
#include "csilk.h"
#include "csilk_internal.h"

/** @brief Maximum number of IP addresses to track. */
#define MAX_IP_ENTRIES 1024
/** @brief Rate limiting window size in seconds. */
#define WINDOW_SIZE 60
/** @brief How often (in seconds) to evict stale entries. */
#define EVICT_INTERVAL 300

/** @brief Rate-limit tracking entry for a single IP address. */
typedef struct {
  char ip[46];       /**< Client IP address string. */
  int count;         /**< Request count in current window. */
  time_t last_reset; /**< Timestamp when the window started. */
  time_t last_seen;  /**< Timestamp of last request (for LRU eviction). */
} ip_entry_t;

static ip_entry_t ip_table[MAX_IP_ENTRIES];
static int ip_count = 0;
static time_t last_evict = 0;
static uv_mutex_t ratelimit_mutex;
static uv_once_t ratelimit_once = UV_ONCE_INIT;

/** @brief Initialize the rate-limiting mutex (called once via uv_once). */
static void init_ratelimit_mutex() { uv_mutex_init(&ratelimit_mutex); }

/** @brief Evict stale entries (entries older than WINDOW_SIZE since last_seen).
 */
static void evict_stale_entries(time_t now) {
  int write_idx = 0;
  for (int read_idx = 0; read_idx < ip_count; read_idx++) {
    if (now - ip_table[read_idx].last_seen <= WINDOW_SIZE) {
      if (write_idx != read_idx) {
        ip_table[write_idx] = ip_table[read_idx];
      }
      write_idx++;
    }
  }
  ip_count = write_idx;
}

/** @brief Evict the single oldest entry (LRU). */
static void evict_oldest_entry(void) {
  if (ip_count <= 0) return;
  int oldest = 0;
  for (int i = 1; i < ip_count; i++) {
    if (ip_table[i].last_seen < ip_table[oldest].last_seen) {
      oldest = i;
    }
  }
  ip_table[oldest] = ip_table[ip_count - 1];
  ip_count--;
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

  // Periodic eviction of stale entries
  if (now - last_evict > EVICT_INTERVAL) {
    evict_stale_entries(now);
    last_evict = now;
  }

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
      // Table full: evict oldest entry
      evict_oldest_entry();
      entry = &ip_table[ip_count++];
      strncpy(entry->ip, ip, sizeof(entry->ip) - 1);
      entry->ip[sizeof(entry->ip) - 1] = '\0';
      entry->count = 0;
      entry->last_reset = now;
    }
  }

  // Check window
  if (now - entry->last_reset > WINDOW_SIZE) {
    entry->count = 1;
    entry->last_reset = now;
  } else {
    entry->count++;
  }
  entry->last_seen = now;

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
