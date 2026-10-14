/**
 * @file mcp_stdio_pool.c
 * @brief Reusable stdio MCP client connection pool with LRU and TTL eviction.
 *
 * @copyright MIT License
 */

#include "csilk/protocols/mcp.h"
#include "csilk/core/secret.h"
#include "csilk/core/sync.h"
#include "mcp_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CSILK_MCP_STDIO_POOL_CAP 64
#define CSILK_MCP_STDIO_DEFAULT_MAX 32
#define CSILK_MCP_STDIO_DEFAULT_TTL 60

typedef struct {
    char                key[128];
    csilk_mcp_client_t* client;
    time_t              last_used;
    bool                in_use;
    bool                pool;
    bool                in_progress;
} pool_entry_t;

static pool_entry_t  s_pool[CSILK_MCP_STDIO_POOL_CAP];
static size_t        s_pool_count = 0;
static csilk_mutex_t s_pool_lock;
static csilk_once_t  s_pool_once = CSILK_ONCE_INIT;

static void
pool_lock_init(void)
{
    csilk_mutex_init(&s_pool_lock);
}

static void
ensure_pool_init(void)
{
    csilk_once(&s_pool_once, pool_lock_init);
}

static int
get_max_procs(void)
{
    char        buf[32];
    const char* val = csilk_env_get("CSILK_MCP_STDIO_MAX_PROCS", buf, sizeof(buf), NULL);
    if (!val) {
        val = csilk_env_get("MINEFOLIO_MCP_STDIO_MAX_PROCS", buf, sizeof(buf), "32");
    }
    int max = atoi(val);
    if (max > CSILK_MCP_STDIO_POOL_CAP) {
        max = CSILK_MCP_STDIO_POOL_CAP;
    }
    return max;
}

static size_t
find_idle_slot(const char* key)
{
    for (size_t i = 0; i < s_pool_count; i++) {
        if (!s_pool[i].in_use && !s_pool[i].in_progress && strcmp(s_pool[i].key, key) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

static size_t
find_in_progress_slot(const char* key)
{
    for (size_t i = 0; i < s_pool_count; i++) {
        if (s_pool[i].in_progress && strcmp(s_pool[i].key, key) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

csilk_mcp_client_t*
csilk_mcp_stdio_pool_acquire_keyed(const char* key, const char* command, char* const argv[])
{
    if (!key || !command) {
        return NULL;
    }

    ensure_pool_init();
    int max = get_max_procs();
    if (max <= 0) {
        /* Pool disabled: return unpooled short-lived client */
        return csilk_mcp_client_connect_stdio(command, argv);
    }

    csilk_mutex_lock(&s_pool_lock);

    /* Phase 1: check for idle slot */
    size_t slot = find_idle_slot(key);
    if (slot != SIZE_MAX) {
        s_pool[slot].in_use = true;
        s_pool[slot].last_used = time(NULL);
        csilk_mcp_client_t* got = s_pool[slot].client;
        csilk_mutex_unlock(&s_pool_lock);
        return got;
    }

    /* If initialization is already in progress by another thread, fall back to short-lived */
    if (find_in_progress_slot(key) != SIZE_MAX) {
        csilk_mutex_unlock(&s_pool_lock);
        return csilk_mcp_client_connect_stdio(command, argv);
    }

    /* If pool is full, evict oldest idle entry */
    if (s_pool_count >= (size_t)max || s_pool_count >= CSILK_MCP_STDIO_POOL_CAP) {
        size_t victim = SIZE_MAX;
        time_t oldest = 0;
        for (size_t i = 0; i < s_pool_count; i++) {
            if (s_pool[i].in_use || s_pool[i].in_progress) {
                continue;
            }
            if (victim == SIZE_MAX || s_pool[i].last_used < oldest) {
                oldest = s_pool[i].last_used;
                victim = i;
            }
        }

        if (victim == SIZE_MAX) {
            /* All slots busy: fall back to short-lived client */
            csilk_mutex_unlock(&s_pool_lock);
            return csilk_mcp_client_connect_stdio(command, argv);
        }

        csilk_mcp_client_t* dead = s_pool[victim].client;
        s_pool[victim] = s_pool[--s_pool_count];
        csilk_mcp_client_free(dead);
    }

    /* Reserve in-progress slot */
    if (s_pool_count >= CSILK_MCP_STDIO_POOL_CAP) {
        csilk_mutex_unlock(&s_pool_lock);
        return csilk_mcp_client_connect_stdio(command, argv);
    }

    size_t reserve = s_pool_count++;
    strncpy(s_pool[reserve].key, key, sizeof(s_pool[reserve].key) - 1);
    s_pool[reserve].key[sizeof(s_pool[reserve].key) - 1] = '\0';
    s_pool[reserve].client = NULL;
    s_pool[reserve].last_used = time(NULL);
    s_pool[reserve].in_use = true;
    s_pool[reserve].pool = true;
    s_pool[reserve].in_progress = true;
    csilk_mutex_unlock(&s_pool_lock);

    /* Phase 2: spawn outside pool lock */
    csilk_mcp_client_t* c = csilk_mcp_client_connect_stdio(command, argv);
    if (!c) {
        csilk_mutex_lock(&s_pool_lock);
        s_pool[reserve] = s_pool[--s_pool_count];
        csilk_mutex_unlock(&s_pool_lock);
        return NULL;
    }

    /* Phase 3: populate slot inside lock */
    csilk_mutex_lock(&s_pool_lock);
    s_pool[reserve].client = c;
    s_pool[reserve].in_progress = false;
    s_pool[reserve].last_used = time(NULL);
    csilk_mutex_unlock(&s_pool_lock);

    return c;
}

csilk_mcp_client_t*
csilk_mcp_stdio_pool_acquire(const char* command, char* const argv[])
{
    return csilk_mcp_stdio_pool_acquire_keyed(command, command, argv);
}

void
csilk_mcp_stdio_pool_release(csilk_mcp_client_t* client)
{
    if (!client) {
        return;
    }

    ensure_pool_init();
    csilk_mutex_lock(&s_pool_lock);

    for (size_t i = 0; i < s_pool_count; i++) {
        if (s_pool[i].client == client) {
            s_pool[i].in_use = false;
            s_pool[i].last_used = time(NULL);
            csilk_mutex_unlock(&s_pool_lock);
            return;
        }
    }

    csilk_mutex_unlock(&s_pool_lock);

    /* Unpooled short-lived client */
    csilk_mcp_client_free(client);
}

int
csilk_mcp_stdio_pool_evict_idle(int ttl_seconds)
{
    ensure_pool_init();
    int ttl = ttl_seconds;
    if (ttl < 0) {
        char        buf[32];
        const char* val = csilk_env_get("CSILK_MCP_STDIO_CACHE_TTL", buf, sizeof(buf), NULL);
        if (!val) {
            val = csilk_env_get("MINEFOLIO_MCP_CACHE_TTL", buf, sizeof(buf), "60");
        }
        ttl = atoi(val);
        if (ttl <= 0) {
            ttl = CSILK_MCP_STDIO_DEFAULT_TTL;
        }
    }

    time_t now = time(NULL);
    int    evicted = 0;

    csilk_mutex_lock(&s_pool_lock);
    size_t i = 0;
    while (i < s_pool_count) {
        if (s_pool[i].in_progress) {
            i++;
            continue;
        }
        if (!s_pool[i].in_use && (now - s_pool[i].last_used) >= (time_t)ttl) {
            csilk_mcp_client_t* dead = s_pool[i].client;
            s_pool[i] = s_pool[--s_pool_count];
            csilk_mcp_client_free(dead);
            evicted++;
        } else {
            i++;
        }
    }
    csilk_mutex_unlock(&s_pool_lock);

    return evicted;
}

int
csilk_mcp_stdio_pool_shutdown(void)
{
    ensure_pool_init();
    int count = 0;

    csilk_mutex_lock(&s_pool_lock);
    while (s_pool_count > 0) {
        s_pool_count--;
        csilk_mcp_client_t* dead = s_pool[s_pool_count].client;
        csilk_mcp_client_free(dead);
        count++;
    }
    csilk_mutex_unlock(&s_pool_lock);

    return count;
}

size_t
csilk_mcp_stdio_pool_get_count(void)
{
    ensure_pool_init();
    csilk_mutex_lock(&s_pool_lock);
    size_t count = s_pool_count;
    csilk_mutex_unlock(&s_pool_lock);
    return count;
}

size_t
csilk_mcp_stdio_pool_get_idle_count(void)
{
    ensure_pool_init();
    csilk_mutex_lock(&s_pool_lock);
    size_t idle = 0;
    for (size_t i = 0; i < s_pool_count; i++) {
        if (!s_pool[i].in_use && !s_pool[i].in_progress) {
            idle++;
        }
    }
    csilk_mutex_unlock(&s_pool_lock);
    return idle;
}
