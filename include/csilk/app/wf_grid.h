/**
 * @file wf_grid.h
 * @brief Decentralized Multi-Agent Task Grid and Work-Stealing Coordinator.
 * @copyright MIT License
 */

#ifndef CSILK_APP_WF_GRID_H
#define CSILK_APP_WF_GRID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
struct csilk_raft_node_s;
typedef struct csilk_raft_node_s csilk_raft_node_t;
struct csilk_mq_s;
typedef struct csilk_mq_s csilk_mq_t;

typedef struct csilk_wf_grid_s csilk_wf_grid_t;

/**
 * @brief Allocate and initialize a decentralized workflow grid node.
 *
 * @param node_id Unique cluster node identifier string.
 * @param raft Optional Raft consensus node handle for topology sync (or NULL).
 * @param mq Optional MQ handle for remote message exchange (or NULL).
 * @return Allocated grid handle, or NULL on failure.
 */
csilk_wf_grid_t* csilk_wf_grid_new(const char* node_id, csilk_raft_node_t* raft, csilk_mq_t* mq);

/**
 * @brief Push a local task slice to the bottom of the node's work-stealing deque.
 *
 * @param grid Grid node handle.
 * @param task_id Unique task identifier string.
 * @param payload Task input data or serialized step.
 * @param len Byte length of payload.
 * @return 0 on success, -1 on invalid argument or queue full.
 */
int csilk_wf_grid_push_task(csilk_wf_grid_t* grid,
                            const char*      task_id,
                            const char*      payload,
                            size_t           len);

/**
 * @brief Pop a task from the bottom of the local deque (LIFO order for data locality).
 *
 * @param grid Grid node handle.
 * @param out_task_id Output buffer to receive the popped task ID.
 * @param max_id_len Size of out_task_id buffer.
 * @return 0 on success, -1 if queue is empty.
 */
int csilk_wf_grid_pop_task(csilk_wf_grid_t* grid, char* out_task_id, size_t max_id_len);

/**
 * @brief Steal a task slice from a peer victim grid node (FIFO order from Top of victim's deque).
 *
 * Transfers ownership to thief_grid with an active lease.
 *
 * @param thief_grid Grid node attempting the theft.
 * @param victim_grid Target peer node holding workload.
 * @param out_task_id Output buffer to receive the stolen task ID.
 * @param max_id_len Size of out_task_id buffer.
 * @return 0 on success, -1 if victim is empty or invalid.
 */
int csilk_wf_grid_steal_from_peer(csilk_wf_grid_t* thief_grid,
                                  csilk_wf_grid_t* victim_grid,
                                  char*            out_task_id,
                                  size_t           max_id_len);

/**
 * @brief Automatically steal from the most loaded registered peer and execute.
 *
 * @param grid Grid node handle.
 * @param out_task_id Output buffer to receive stolen task ID.
 * @param max_id_len Size of out_task_id buffer.
 * @return 0 on success, -1 if no stealable tasks found across peers.
 */
int csilk_wf_grid_steal_and_execute(csilk_wf_grid_t* grid, char* out_task_id, size_t max_id_len);

/**
 * @brief Rebalance tasks upon node failure or departure.
 *
 * Scans tasks leased to failed_node_id and resets them to available state.
 *
 * @param grid Grid node handle.
 * @param failed_node_id Identifier of the offline/failed node.
 * @return 0 on success, -1 on invalid arguments.
 */
int csilk_wf_grid_rebalance(csilk_wf_grid_t* grid, const char* failed_node_id);

/**
 * @brief Returns the number of pending tasks in this node's deque.
 *
 * @param grid Grid node handle.
 * @return Number of pending tasks.
 */
size_t csilk_wf_grid_task_count(const csilk_wf_grid_t* grid);

/**
 * @brief Free resources associated with a grid node.
 *
 * @param grid Grid node handle.
 */
void csilk_wf_grid_free(csilk_wf_grid_t* grid);

#ifdef __cplusplus
}
#endif

#endif /* CSILK_APP_WF_GRID_H */
