/**
 * @file wf_grid.c
 * @brief Decentralized Multi-Agent Task Grid and Work-Stealing Coordinator.
 * @copyright MIT License
 */

#include "csilk/workflow/grid.h"
#include "csilk/core/sync.h"
#include "csilk/csilk.h"
#include "csilk/messaging/mq.h"
#include "csilk/messaging/raft.h"
#include "workflow_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CSILK_WF_GRID_DEQUE_CAP 256
#define CSILK_WF_GRID_MAX_PEERS 32

typedef struct {
    char     task_id[64];
    char     owner_node[64];
    char*    payload;
    size_t   payload_len;
    uint64_t lease_expire_ts;
    bool     in_progress;
} csilk_wf_task_slice_t;

struct csilk_wf_grid_s {
    char                  node_id[64];
    csilk_raft_node_t*    raft;
    csilk_mq_t*           mq;
    csilk_mutex_t         mutex;
    csilk_wf_task_slice_t tasks[CSILK_WF_GRID_DEQUE_CAP];
    size_t                head; /* Top of deque (remote steal point) */
    size_t                tail; /* Bottom of deque (local push/pop point) */
    size_t                count;
    char                  peer_ids[CSILK_WF_GRID_MAX_PEERS][64];
    char                  peer_addrs[CSILK_WF_GRID_MAX_PEERS][128];
    size_t                peer_count;
    bool                  raft_synced;
};

static csilk_wf_grid_t* g_registered_grids[CSILK_WF_GRID_MAX_PEERS];
static size_t           g_registered_grid_count = 0;
static csilk_mutex_t    g_grid_reg_mutex;
static csilk_once_t     g_grid_reg_once = CSILK_ONCE_INIT;

static void
_grid_reg_init(void)
{
    csilk_mutex_init(&g_grid_reg_mutex);
}

static uint64_t
_get_monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void
_lock_grid_pair(csilk_wf_grid_t* a, csilk_wf_grid_t* b)
{
    if (!a && !b) {
        return;
    }
    if (!a) {
        csilk_mutex_lock(&b->mutex);
        return;
    }
    if (!b || a == b) {
        csilk_mutex_lock(&a->mutex);
        return;
    }
    if ((uintptr_t)a < (uintptr_t)b) {
        csilk_mutex_lock(&a->mutex);
        csilk_mutex_lock(&b->mutex);
    } else {
        csilk_mutex_lock(&b->mutex);
        csilk_mutex_lock(&a->mutex);
    }
}

static void
_unlock_grid_pair(csilk_wf_grid_t* a, csilk_wf_grid_t* b)
{
    if (a) {
        csilk_mutex_unlock(&a->mutex);
    }
    if (b && b != a) {
        csilk_mutex_unlock(&b->mutex);
    }
}

static bool
_grid_has_peer_locked(const csilk_wf_grid_t* grid, const char* peer_id)
{
    if (!grid || !peer_id) {
        return false;
    }
    for (size_t i = 0; i < grid->peer_count; i++) {
        if (strcmp(grid->peer_ids[i], peer_id) == 0) {
            return true;
        }
    }
    return false;
}

static void
_grid_add_peer_locked(csilk_wf_grid_t* grid, const char* peer_id, const char* peer_addr)
{
    if (!grid || !peer_id || peer_id[0] == '\0' || strcmp(grid->node_id, peer_id) == 0) {
        return;
    }
    if (grid->raft) {
        char raft_local_id[64];
        snprintf(raft_local_id, sizeof(raft_local_id), "%u", csilk_raft_get_node_id(grid->raft));
        if (strcmp(peer_id, raft_local_id) == 0) {
            return;
        }
    }
    for (size_t i = 0; i < grid->peer_count; i++) {
        if (strcmp(grid->peer_ids[i], peer_id) == 0) {
            if (peer_addr && peer_addr[0] != '\0') {
                snprintf(grid->peer_addrs[i], sizeof(grid->peer_addrs[i]), "%s", peer_addr);
            }
            return;
        }
    }
    if (grid->peer_count < CSILK_WF_GRID_MAX_PEERS) {
        snprintf(grid->peer_ids[grid->peer_count], sizeof(grid->peer_ids[0]), "%s", peer_id);
        if (peer_addr) {
            snprintf(
                grid->peer_addrs[grid->peer_count], sizeof(grid->peer_addrs[0]), "%s", peer_addr);
        } else {
            grid->peer_addrs[grid->peer_count][0] = '\0';
        }
        grid->peer_count++;
    }
}

static void
_grid_remove_peer_locked(csilk_wf_grid_t* grid, const char* peer_id)
{
    if (!grid || !peer_id) {
        return;
    }
    for (size_t i = 0; i < grid->peer_count; i++) {
        if (strcmp(grid->peer_ids[i], peer_id) == 0) {
            for (size_t j = i; j + 1 < grid->peer_count; j++) {
                memcpy(grid->peer_ids[j], grid->peer_ids[j + 1], sizeof(grid->peer_ids[0]));
                memcpy(grid->peer_addrs[j], grid->peer_addrs[j + 1], sizeof(grid->peer_addrs[0]));
            }
            grid->peer_count--;
            break;
        }
    }
}

static void
_on_raft_topology_event(csilk_raft_node_t*          node,
                        csilk_raft_topology_event_t event,
                        const char*                 peer_id,
                        const char*                 peer_addr,
                        void*                       user_data)
{
    (void)node;
    csilk_wf_grid_t* grid = (csilk_wf_grid_t*)user_data;
    if (!grid) {
        return;
    }

    if (event == CSILK_RAFT_TOPOLOGY_NODE_JOINED && peer_id && peer_id[0] != '\0') {
        csilk_mutex_lock(&grid->mutex);
        _grid_add_peer_locked(grid, peer_id, peer_addr);
        csilk_mutex_unlock(&grid->mutex);
    } else if (event == CSILK_RAFT_TOPOLOGY_NODE_LEFT && peer_id && peer_id[0] != '\0') {
        csilk_mutex_lock(&grid->mutex);
        _grid_remove_peer_locked(grid, peer_id);
        csilk_mutex_unlock(&grid->mutex);

        /* Automatically rebalance tasks previously leased to the departed node */
        csilk_wf_grid_rebalance(grid, peer_id);
    }
}

csilk_wf_grid_t*
csilk_wf_grid_new(const char* node_id, csilk_raft_node_t* raft, csilk_mq_t* mq)
{
    if (!node_id || node_id[0] == '\0') {
        return NULL;
    }

    csilk_wf_grid_t* grid = calloc(1, sizeof(csilk_wf_grid_t));
    if (!grid) {
        return NULL;
    }

    strncpy(grid->node_id, node_id, sizeof(grid->node_id) - 1);
    grid->node_id[sizeof(grid->node_id) - 1] = '\0';
    grid->raft = raft;
    grid->mq = mq;
    csilk_mutex_init(&grid->mutex);

    /* Register to global peer directory */
    csilk_once(&g_grid_reg_once, _grid_reg_init);
    csilk_mutex_lock(&g_grid_reg_mutex);
    if (g_registered_grid_count < CSILK_WF_GRID_MAX_PEERS) {
        g_registered_grids[g_registered_grid_count++] = grid;
    }
    csilk_mutex_unlock(&g_grid_reg_mutex);

    return grid;
}

void
csilk_wf_grid_free(csilk_wf_grid_t* grid)
{
    if (!grid) {
        return;
    }

    if (grid->raft && grid->raft_synced) {
        csilk_raft_set_topology_cb(grid->raft, NULL, NULL);
    }

    /* Unregister from peer directory */
    csilk_once(&g_grid_reg_once, _grid_reg_init);
    csilk_mutex_lock(&g_grid_reg_mutex);
    for (size_t i = 0; i < g_registered_grid_count;) {
        if (g_registered_grids[i] == grid) {
            for (size_t j = i; j + 1 < g_registered_grid_count; j++) {
                g_registered_grids[j] = g_registered_grids[j + 1];
            }
            g_registered_grid_count--;
        } else {
            i++;
        }
    }
    csilk_mutex_unlock(&g_grid_reg_mutex);

    csilk_mutex_lock(&grid->mutex);
    for (size_t i = 0; i < CSILK_WF_GRID_DEQUE_CAP; i++) {
        if (grid->tasks[i].payload) {
            free(grid->tasks[i].payload);
            grid->tasks[i].payload = NULL;
        }
    }
    csilk_mutex_unlock(&grid->mutex);
    csilk_mutex_destroy(&grid->mutex);
    free(grid);
}

int
csilk_wf_grid_push_task(csilk_wf_grid_t* grid, const char* task_id, const char* payload, size_t len)
{
    if (!grid || !task_id || task_id[0] == '\0') {
        return -1;
    }

    csilk_mutex_lock(&grid->mutex);
    if (grid->count >= CSILK_WF_GRID_DEQUE_CAP) {
        csilk_mutex_unlock(&grid->mutex);
        return -1; /* Queue full */
    }

    csilk_wf_task_slice_t* slot = &grid->tasks[grid->tail];
    if (slot->payload) {
        free(slot->payload);
        slot->payload = NULL;
    }

    strncpy(slot->task_id, task_id, sizeof(slot->task_id) - 1);
    slot->task_id[sizeof(slot->task_id) - 1] = '\0';
    strncpy(slot->owner_node, grid->node_id, sizeof(slot->owner_node) - 1);
    slot->owner_node[sizeof(slot->owner_node) - 1] = '\0';

    if (payload && len > 0) {
        slot->payload = malloc(len + 1);
        if (slot->payload) {
            memcpy(slot->payload, payload, len);
            slot->payload[len] = '\0';
            slot->payload_len = len;
        }
    } else {
        slot->payload = NULL;
        slot->payload_len = 0;
    }

    slot->lease_expire_ts = 0;
    slot->in_progress = false;

    grid->tail = (grid->tail + 1) % CSILK_WF_GRID_DEQUE_CAP;
    grid->count++;

    csilk_mutex_unlock(&grid->mutex);
    return 0;
}

int
csilk_wf_grid_pop_task(csilk_wf_grid_t* grid, char* out_task_id, size_t max_id_len)
{
    if (!grid || !out_task_id || max_id_len == 0) {
        return -1;
    }

    csilk_mutex_lock(&grid->mutex);
    if (grid->count == 0) {
        csilk_mutex_unlock(&grid->mutex);
        return -1; /* Deque empty */
    }

    /* Pop from Bottom (tail - 1) in LIFO order for local cache locality */
    grid->tail = (grid->tail + CSILK_WF_GRID_DEQUE_CAP - 1) % CSILK_WF_GRID_DEQUE_CAP;
    csilk_wf_task_slice_t* slot = &grid->tasks[grid->tail];

    strncpy(out_task_id, slot->task_id, max_id_len - 1);
    out_task_id[max_id_len - 1] = '\0';

    if (slot->payload) {
        free(slot->payload);
        slot->payload = NULL;
    }
    slot->payload_len = 0;
    grid->count--;

    csilk_mutex_unlock(&grid->mutex);
    return 0;
}

int
csilk_wf_grid_steal_from_peer(csilk_wf_grid_t* thief_grid,
                              csilk_wf_grid_t* victim_grid,
                              char*            out_task_id,
                              size_t           max_id_len)
{
    if (!thief_grid || !victim_grid || thief_grid == victim_grid || !out_task_id ||
        max_id_len == 0) {
        return -1;
    }

    _lock_grid_pair(thief_grid, victim_grid);

    if (victim_grid->count == 0) {
        _unlock_grid_pair(thief_grid, victim_grid);
        return -1; /* Nothing to steal */
    }

    if (thief_grid->count >= CSILK_WF_GRID_DEQUE_CAP) {
        _unlock_grid_pair(thief_grid, victim_grid);
        return -1; /* Thief has no capacity */
    }

    /* Steal from Top (head) in FIFO order */
    csilk_wf_task_slice_t* victim_slot = &victim_grid->tasks[victim_grid->head];
    victim_grid->head = (victim_grid->head + 1) % CSILK_WF_GRID_DEQUE_CAP;
    victim_grid->count--;

    strncpy(out_task_id, victim_slot->task_id, max_id_len - 1);
    out_task_id[max_id_len - 1] = '\0';

    /* Insert into thief's deque with active lease */
    csilk_wf_task_slice_t* thief_slot = &thief_grid->tasks[thief_grid->tail];
    if (thief_slot->payload) {
        free(thief_slot->payload);
        thief_slot->payload = NULL;
    }

    strncpy(thief_slot->task_id, victim_slot->task_id, sizeof(thief_slot->task_id) - 1);
    thief_slot->task_id[sizeof(thief_slot->task_id) - 1] = '\0';
    strncpy(thief_slot->owner_node, thief_grid->node_id, sizeof(thief_slot->owner_node) - 1);
    thief_slot->owner_node[sizeof(thief_slot->owner_node) - 1] = '\0';

    thief_slot->payload = victim_slot->payload;
    thief_slot->payload_len = victim_slot->payload_len;
    victim_slot->payload = NULL;
    victim_slot->payload_len = 0;

    /* 5-second lease granted to thief */
    thief_slot->lease_expire_ts = _get_monotonic_ms() + 5000ULL;
    thief_slot->in_progress = false;

    thief_grid->tail = (thief_grid->tail + 1) % CSILK_WF_GRID_DEQUE_CAP;
    thief_grid->count++;

    _unlock_grid_pair(thief_grid, victim_grid);
    return 0;
}

int
csilk_wf_grid_steal_and_execute(csilk_wf_grid_t* grid, char* out_task_id, size_t max_id_len)
{
    if (!grid || !out_task_id || max_id_len == 0) {
        return -1;
    }

    csilk_once(&g_grid_reg_once, _grid_reg_init);
    csilk_mutex_lock(&g_grid_reg_mutex);

    csilk_wf_grid_t* richest_peer = NULL;
    size_t           max_tasks = 0;

    for (size_t i = 0; i < g_registered_grid_count; i++) {
        csilk_wf_grid_t* peer = g_registered_grids[i];
        if (peer && peer != grid) {
            size_t c = csilk_wf_grid_task_count(peer);
            if (c > max_tasks) {
                max_tasks = c;
                richest_peer = peer;
            }
        }
    }

    csilk_mutex_unlock(&g_grid_reg_mutex);

    if (!richest_peer || max_tasks == 0) {
        return -1; /* No tasks available across cluster */
    }

    return csilk_wf_grid_steal_from_peer(grid, richest_peer, out_task_id, max_id_len);
}

int
csilk_wf_grid_rebalance(csilk_wf_grid_t* grid, const char* failed_node_id)
{
    if (!grid || !failed_node_id || failed_node_id[0] == '\0') {
        return -1;
    }

    csilk_mutex_lock(&grid->mutex);
    uint64_t now = _get_monotonic_ms();

    for (size_t i = 0; i < CSILK_WF_GRID_DEQUE_CAP; i++) {
        csilk_wf_task_slice_t* slot = &grid->tasks[i];
        if (slot->task_id[0] != '\0') {
            if (strcmp(slot->owner_node, failed_node_id) == 0 ||
                (slot->lease_expire_ts > 0 && slot->lease_expire_ts <= now)) {
                /* Reset ownership to this surviving node */
                strncpy(slot->owner_node, grid->node_id, sizeof(slot->owner_node) - 1);
                slot->owner_node[sizeof(slot->owner_node) - 1] = '\0';
                slot->in_progress = false;
                slot->lease_expire_ts = 0;
            }
        }
    }

    csilk_mutex_unlock(&grid->mutex);
    return 0;
}

size_t
csilk_wf_grid_task_count(const csilk_wf_grid_t* grid)
{
    if (!grid) {
        return 0;
    }
    csilk_wf_grid_t* g = (csilk_wf_grid_t*)grid;
    csilk_mutex_lock(&g->mutex);
    size_t cnt = g->count;
    csilk_mutex_unlock(&g->mutex);
    return cnt;
}

int
csilk_wf_grid_sync_raft_topology(csilk_wf_grid_t* grid)
{
    if (!grid || !grid->raft) {
        return -1;
    }

    /* 1. Pull existing cluster members from Raft */
    csilk_raft_membership_t memb;
    if (csilk_raft_get_membership(grid->raft, &memb) == 0) {
        csilk_mutex_lock(&grid->mutex);
        for (size_t i = 0; i < memb.old_count; i++) {
            _grid_add_peer_locked(grid, memb.old_nodes[i].id, memb.old_nodes[i].addr);
        }
        for (size_t i = 0; i < memb.new_count; i++) {
            _grid_add_peer_locked(grid, memb.new_nodes[i].id, memb.new_nodes[i].addr);
        }
        grid->raft_synced = true;
        csilk_mutex_unlock(&grid->mutex);
    }

    /* 2. Register dynamic event listener callback */
    return csilk_raft_set_topology_cb(grid->raft, _on_raft_topology_event, grid);
}

size_t
csilk_wf_grid_peer_count(const csilk_wf_grid_t* grid)
{
    if (!grid) {
        return 0;
    }
    csilk_wf_grid_t* g = (csilk_wf_grid_t*)grid;
    csilk_mutex_lock(&g->mutex);
    size_t cnt = g->peer_count;
    csilk_mutex_unlock(&g->mutex);
    return cnt;
}

bool
csilk_wf_grid_has_peer(const csilk_wf_grid_t* grid, const char* peer_id)
{
    if (!grid || !peer_id) {
        return false;
    }
    csilk_wf_grid_t* g = (csilk_wf_grid_t*)grid;
    csilk_mutex_lock(&g->mutex);
    bool found = _grid_has_peer_locked(g, peer_id);
    csilk_mutex_unlock(&g->mutex);
    return found;
}

int
csilk_wf_set_grid(csilk_wf_t* wf, csilk_wf_grid_t* grid)
{
    if (!wf) {
        return -1;
    }
    csilk_mutex_lock(&wf->ctx_mutex);
    wf->grid = grid;
    csilk_mutex_unlock(&wf->ctx_mutex);
    return 0;
}

csilk_wf_grid_t*
csilk_wf_get_grid(const csilk_wf_t* wf)
{
    if (!wf) {
        return NULL;
    }
    csilk_wf_t* w = (csilk_wf_t*)wf;
    csilk_mutex_lock(&w->ctx_mutex);
    csilk_wf_grid_t* grid = w->grid;
    csilk_mutex_unlock(&w->ctx_mutex);
    return grid;
}

int
csilk_wf_submit_grid_task(csilk_wf_t* wf,
                          const char* task_id,
                          const char* payload,
                          size_t      payload_len)
{
    if (!wf || !task_id) {
        return -1;
    }
    csilk_wf_grid_t* grid = csilk_wf_get_grid(wf);
    if (!grid) {
        return -1;
    }
    return csilk_wf_grid_push_task(grid, task_id, payload, payload_len);
}

int
csilk_wf_poll_grid_task(csilk_wf_t* wf, char* out_task_id, size_t max_id_len)
{
    if (!wf || !out_task_id || max_id_len == 0) {
        return -1;
    }
    csilk_wf_grid_t* grid = csilk_wf_get_grid(wf);
    if (!grid) {
        return -1;
    }
    return csilk_wf_grid_pop_task(grid, out_task_id, max_id_len);
}
