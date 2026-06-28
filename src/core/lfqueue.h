/**
 * @file lfqueue.h
 * @brief Lock-free Multiple-Producer Single-Consumer (MPSC) queue.
 *
 * Implements an intrusive wait-free MPSC queue using C11 stdatomic.
 * Ideal for cross-thread task dispatching (many workers pushing to one event loop).
 */

#ifndef CSILK_LFQUEUE_H
#define CSILK_LFQUEUE_H

#include <stdatomic.h>
#include <stddef.h>

/** @brief Intrusive node for the lock-free queue. */
typedef struct csilk_lfq_node_s {
	_Atomic(struct csilk_lfq_node_s*) next;
} csilk_lfq_node_t;

/** @brief Lock-free MPSC queue state. */
typedef struct {
	_Atomic(csilk_lfq_node_t*) head;
	csilk_lfq_node_t* tail;
	csilk_lfq_node_t stub;
} csilk_lfqueue_t;

/** @brief Initialize the lock-free queue. */
static inline void
csilk_lfq_init(csilk_lfqueue_t* q)
{
	atomic_init(&q->stub.next, NULL);
	atomic_init(&q->head, &q->stub);
	q->tail = &q->stub;
}

/** @brief Enqueue a node (Wait-free, Multiple Producers). */
static inline void
csilk_lfq_enqueue(csilk_lfqueue_t* q, csilk_lfq_node_t* n)
{
	atomic_init(&n->next, NULL);
	csilk_lfq_node_t* prev = atomic_exchange_explicit(&q->head, n, memory_order_acq_rel);
	atomic_store_explicit(&prev->next, n, memory_order_release);
}

/** @brief Dequeue a node (Wait-free, Single Consumer).
 * @return The dequeued node, or NULL if empty. */
static inline csilk_lfq_node_t*
csilk_lfq_dequeue(csilk_lfqueue_t* q)
{
	csilk_lfq_node_t* tail = q->tail;
	csilk_lfq_node_t* next = atomic_load_explicit(&tail->next, memory_order_acquire);

	if (tail == &q->stub) {
		if (next == NULL) {
			return NULL;
		}
		q->tail = next;
		tail = next;
		next = atomic_load_explicit(&next->next, memory_order_acquire);
	}

	if (next != NULL) {
		q->tail = next;
		return tail;
	}

	csilk_lfq_node_t* head = atomic_load_explicit(&q->head, memory_order_acquire);
	if (tail != head) {
		return NULL;
	}

	csilk_lfq_enqueue(q, &q->stub);
	next = atomic_load_explicit(&tail->next, memory_order_acquire);
	if (next != NULL) {
		q->tail = next;
		return tail;
	}

	return NULL;
}

#endif /* CSILK_LFQUEUE_H */
