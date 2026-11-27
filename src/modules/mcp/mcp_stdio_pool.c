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

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CSILK_MCP_STDIO_POOL_CAP 64
#define CSILK_MCP_STDIO_DEFAULT_MAX 32
#define CSILK_MCP_STDIO_DEFAULT_TTL 60

typedef enum {
    MCP_SLOT_EMPTY = 0,
    MCP_SLOT_IN_PROGRESS,
    MCP_SLOT_IDLE,
    MCP_SLOT_IN_USE
} mcp_slot_state_t;

typedef struct {
    char                key[128];
    csilk_mcp_client_t* client;
    time_t              last_used;
    mcp_slot_state_t    state;
    uint32_t            slot_gen;
} pool_entry_t;

static pool_entry_t  s_pool[CSILK_MCP_STDIO_POOL_CAP];
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

/**
 * @brief Resolve the idle-eviction TTL in seconds (env override, else default).
 */
static int
get_ttl_seconds(void)
{
    char        buf[32];
    const char* val = csilk_env_get("CSILK_MCP_STDIO_CACHE_TTL", buf, sizeof(buf), NULL);
    if (!val) {
        val = csilk_env_get("MINEFOLIO_MCP_CACHE_TTL", buf, sizeof(buf), "60");
    }
    int ttl = atoi(val);
    return ttl > 0 ? ttl : CSILK_MCP_STDIO_DEFAULT_TTL;
}

/**
 * @brief Reap every idle slot whose inactivity exceeded @p ttl.
 *
 * @pre Caller holds @c s_pool_lock.
 * @warning Collects victims into @p out_list (which the caller must free) and
 *          frees the clients itself: the free path may block in waitpid.
 */
static size_t
collect_expired_idle_locked(int ttl, csilk_mcp_client_t** out_list, size_t out_cap)
{
    time_t now = time(NULL);
    size_t kept = 0;
    for (size_t i = 0; i < CSILK_MCP_STDIO_POOL_CAP && kept < out_cap; i++) {
        if (s_pool[i].state == MCP_SLOT_IDLE && (now - s_pool[i].last_used) >= (time_t)ttl) {
            out_list[kept++] = s_pool[i].client;
            s_pool[i].client = NULL;
            s_pool[i].state = MCP_SLOT_EMPTY;
            s_pool[i].key[0] = '\0';
            s_pool[i].slot_gen++;
        }
    }
    return kept;
}

/* --- Idle-client liveness probing --- */

/**
 * @brief Probe whether a pooled stdio child is still usable.
 *
 * kill(pid, 0) alone cannot answer this: it keeps succeeding while the child is
 * a zombie (exited but unreaped). combine it with a non-blocking waitpid,
 * which *does* report an exited child. A reap here is safe because the child is
 * being declared dead: the slot is dropped by the caller either way.
 */
static bool
_mcp_child_alive(const csilk_mcp_client_t* c)
{
    if (!c) {
        return false;
    }
    /* pid <= 0 means the child was never spawned (lazy startup): leave the
     * decision to the spawn path, never declare a fresh client dead. */
    if (c->child_pid <= 0) {
        return true;
    }
    if (kill((pid_t)c->child_pid, 0) != 0) {
        /* ESRCH: already reaped. EPERM: exists but not ours to signal, which
         * for our own fork'ed children cannot happen. */
        return errno == EPERM;
    }
    int st = 0;
    return waitpid((pid_t)c->child_pid, &st, WNOHANG) == 0;
}

/**
 * @brief Detect whether a pooled child has closed its end of the stdio pipe.
 *
 * A child that exited on its own closes stdout, so the parent's read end keeps
 * reporting readable while read() returns 0. This is the only reliable death
 * signal for a *zombie* child: kill(pid, 0) keeps succeeding until somebody
 * reaps it, so the process probe alone cannot see an exited-but-unreaped
 * subprocess.
 *
 * Only POLLHUP / POLLERR are treated as EOF. POLLIN deliberately is NOT: a
 * live child may have unsolicited notification frames buffered, and treating
 * "readable" as "dead" would reap perfectly healthy pooled clients.
 */
static bool
_mcp_pipe_eof(const csilk_mcp_client_t* c)
{
    if (!c || !c->is_stdio || c->pipe_fd[0] < 0) {
        return false;
    }
    struct pollfd pfd = {.fd = c->pipe_fd[0], .events = POLLIN};
    int           rc = poll(&pfd, 1, 0);
    return rc > 0 && (pfd.revents & (POLLHUP | POLLERR)) != 0;
}

/**
 * @brief Decide whether an idle pooled stdio client can still be reused.
 *
 * Combines the process liveness probe with an EOF check on the read pipe. A
 * dead-but-unreaped child (kill -> ESRCH) or a peer that closed its stdout end
 * makes the slot unusable: keeping it would hand the caller a client whose
 * every RPC fails, which surfaces as an opaque "tool returned NULL" far away
 * from the real cause.
 */
static bool
_mcp_slot_healthy(const csilk_mcp_client_t* c)
{
    return _mcp_child_alive(c) && !_mcp_pipe_eof(c);
}

static size_t
find_in_progress_slot(const char* key)
{
    for (size_t i = 0; i < CSILK_MCP_STDIO_POOL_CAP; i++) {
        if (s_pool[i].state == MCP_SLOT_IN_PROGRESS && strcmp(s_pool[i].key, key) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

static size_t
get_active_count_locked(void)
{
    size_t count = 0;
    for (size_t i = 0; i < CSILK_MCP_STDIO_POOL_CAP; i++) {
        if (s_pool[i].state != MCP_SLOT_EMPTY) {
            count++;
        }
    }
    return count;
}

static size_t
find_empty_slot_locked(void)
{
    for (size_t i = 0; i < CSILK_MCP_STDIO_POOL_CAP; i++) {
        if (s_pool[i].state == MCP_SLOT_EMPTY) {
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

    /* Phase 1: check for an idle, *healthy* slot. An idle entry whose
     * subprocess already exited (or whose pipe hit EOF) must never be handed
     * out: the caller would get a client whose every RPC fails, reported far
     * from the real cause. Dead entries are reaped on the spot and a fresh
     * client is spawned instead. The probing helpers never block, so this is
     * safe under the pool lock.
     *
     * Entries idle for longer than the TTL are treated as dead too: nobody
     * calls evict_idle() from inside the framework, so this scan is the only
     * place the TTL can be enforced without an external timer. */
    time_t now = time(NULL);
    int    ttl = get_ttl_seconds();
    size_t slot = SIZE_MAX;
    for (size_t i = 0; i < CSILK_MCP_STDIO_POOL_CAP; i++) {
        if (s_pool[i].state != MCP_SLOT_IDLE || strcmp(s_pool[i].key, key) != 0) {
            continue;
        }
        if ((now - s_pool[i].last_used) >= (time_t)ttl || !_mcp_slot_healthy(s_pool[i].client)) {
            csilk_mcp_client_t* dead = s_pool[i].client;
            s_pool[i].client = NULL;
            s_pool[i].state = MCP_SLOT_EMPTY;
            s_pool[i].key[0] = '\0';
            s_pool[i].slot_gen++;
            /* Free outside the lock: csilk_mcp_client_free may block in
             * waitpid, and holding s_pool_lock across it would stall every
             * other acquire. */
            csilk_mutex_unlock(&s_pool_lock);
            csilk_mcp_client_free(dead);
            csilk_mutex_lock(&s_pool_lock);
            continue;
        }
        slot = i;
        break;
    }
    if (slot != SIZE_MAX) {
        s_pool[slot].state = MCP_SLOT_IN_USE;
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

    size_t              active_count = get_active_count_locked();
    csilk_mcp_client_t* dead_to_free = NULL;

    /* If pool is full, evict oldest idle entry */
    if (active_count >= (size_t)max || active_count >= CSILK_MCP_STDIO_POOL_CAP) {
        size_t victim = SIZE_MAX;
        time_t oldest = 0;
        for (size_t i = 0; i < CSILK_MCP_STDIO_POOL_CAP; i++) {
            if (s_pool[i].state != MCP_SLOT_IDLE) {
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

        dead_to_free = s_pool[victim].client;
        s_pool[victim].client = NULL;
        s_pool[victim].state = MCP_SLOT_EMPTY;
        s_pool[victim].key[0] = '\0';
        s_pool[victim].slot_gen++;
    }

    /* Reserve in-progress slot */
    size_t reserve = find_empty_slot_locked();
    if (reserve == SIZE_MAX) {
        csilk_mutex_unlock(&s_pool_lock);
        if (dead_to_free) {
            csilk_mcp_client_free(dead_to_free);
        }
        return csilk_mcp_client_connect_stdio(command, argv);
    }

    s_pool[reserve].state = MCP_SLOT_IN_PROGRESS;
    s_pool[reserve].slot_gen++;
    uint32_t my_gen = s_pool[reserve].slot_gen;
    strncpy(s_pool[reserve].key, key, sizeof(s_pool[reserve].key) - 1);
    s_pool[reserve].key[sizeof(s_pool[reserve].key) - 1] = '\0';
    s_pool[reserve].client = NULL;
    s_pool[reserve].last_used = time(NULL);
    csilk_mutex_unlock(&s_pool_lock);

    /* Free evicted client outside pool lock */
    if (dead_to_free) {
        csilk_mcp_client_free(dead_to_free);
    }

    /* Phase 2: spawn outside pool lock */
    csilk_mcp_client_t* c = csilk_mcp_client_connect_stdio(command, argv);
    if (!c) {
        csilk_mutex_lock(&s_pool_lock);
        if (s_pool[reserve].slot_gen == my_gen && s_pool[reserve].state == MCP_SLOT_IN_PROGRESS) {
            s_pool[reserve].state = MCP_SLOT_EMPTY;
            s_pool[reserve].key[0] = '\0';
        }
        csilk_mutex_unlock(&s_pool_lock);
        return NULL;
    }

    /* Phase 3: populate slot inside lock */
    csilk_mutex_lock(&s_pool_lock);
    if (s_pool[reserve].slot_gen == my_gen && s_pool[reserve].state == MCP_SLOT_IN_PROGRESS) {
        s_pool[reserve].client = c;
        s_pool[reserve].state = MCP_SLOT_IN_USE;
        s_pool[reserve].last_used = time(NULL);
        csilk_mutex_unlock(&s_pool_lock);
        return c;
    }
    csilk_mutex_unlock(&s_pool_lock);

    /* Fallback if slot changed unexpectedly */
    csilk_mcp_client_free(c);
    return NULL;
}

static void
build_command_argv_key(const char* command, char* const argv[], char* out, size_t out_sz)
{
    if (!out || out_sz == 0) {
        return;
    }
    /* Compute FNV-1a 64-bit hash over command and every argv entry so distinct
     * argument lists never alias onto the same pooled subprocess slot. */
    uint64_t    h = 14695981039346656037ULL;
    const char* s = command ? command : "";
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        h ^= *p;
        h *= 1099511628211ULL;
    }
    if (argv) {
        for (size_t i = 0; argv[i]; i++) {
            h ^= 0xFFU;
            h *= 1099511628211ULL;
            for (const unsigned char* p = (const unsigned char*)argv[i]; *p; p++) {
                h ^= *p;
                h *= 1099511628211ULL;
            }
        }
    }
    snprintf(out, out_sz, "%.90s#%016llx", s, (unsigned long long)h);
}

csilk_mcp_client_t*
csilk_mcp_stdio_pool_acquire(const char* command, char* const argv[])
{
    if (!command) {
        return NULL;
    }
    char key[128];
    build_command_argv_key(command, argv, key, sizeof(key));
    return csilk_mcp_stdio_pool_acquire_keyed(key, command, argv);
}

void
csilk_mcp_stdio_pool_release(csilk_mcp_client_t* client)
{
    if (!client) {
        return;
    }

    ensure_pool_init();
    csilk_mutex_lock(&s_pool_lock);

    for (size_t i = 0; i < CSILK_MCP_STDIO_POOL_CAP; i++) {
        if (s_pool[i].state == MCP_SLOT_IN_USE && s_pool[i].client == client) {
            s_pool[i].state = MCP_SLOT_IDLE;
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
        ttl = get_ttl_seconds();
    }

    csilk_mcp_client_t* dead_list[CSILK_MCP_STDIO_POOL_CAP];
    size_t              dead_count = 0;

    csilk_mutex_lock(&s_pool_lock);
    dead_count = collect_expired_idle_locked(ttl, dead_list, CSILK_MCP_STDIO_POOL_CAP);
    csilk_mutex_unlock(&s_pool_lock);

    for (size_t i = 0; i < dead_count; i++) {
        csilk_mcp_client_free(dead_list[i]);
    }

    return (int)dead_count;
}

int
csilk_mcp_stdio_pool_shutdown(void)
{
    ensure_pool_init();
    csilk_mcp_client_t* dead_list[CSILK_MCP_STDIO_POOL_CAP];
    size_t              dead_count = 0;

    csilk_mutex_lock(&s_pool_lock);
    for (size_t i = 0; i < CSILK_MCP_STDIO_POOL_CAP; i++) {
        if (s_pool[i].state == MCP_SLOT_IDLE) {
            dead_list[dead_count++] = s_pool[i].client;
            s_pool[i].client = NULL;
            s_pool[i].state = MCP_SLOT_EMPTY;
            s_pool[i].key[0] = '\0';
            s_pool[i].slot_gen++;
        }
    }
    csilk_mutex_unlock(&s_pool_lock);

    for (size_t i = 0; i < dead_count; i++) {
        csilk_mcp_client_free(dead_list[i]);
    }

    return (int)dead_count;
}

size_t
csilk_mcp_stdio_pool_get_count(void)
{
    ensure_pool_init();
    csilk_mutex_lock(&s_pool_lock);
    size_t count = get_active_count_locked();
    csilk_mutex_unlock(&s_pool_lock);
    return count;
}

size_t
csilk_mcp_stdio_pool_get_idle_count(void)
{
    ensure_pool_init();
    csilk_mutex_lock(&s_pool_lock);
    size_t idle = 0;
    for (size_t i = 0; i < CSILK_MCP_STDIO_POOL_CAP; i++) {
        if (s_pool[i].state == MCP_SLOT_IDLE) {
            idle++;
        }
    }
    csilk_mutex_unlock(&s_pool_lock);
    return idle;
}
