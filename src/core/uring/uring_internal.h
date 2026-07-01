#ifndef CSILK_URING_INTERNAL_H
#define CSILK_URING_INTERNAL_H

#include "csilk/csilk.h"
#include "csilk/core/sys_io.h"

/* Define opcodes for user_data.
 * High bit selects between I/O ops and timer ops so the CQE dispatch
 * can quickly distinguish completion types without an indirect call. */
typedef enum {
	URING_OP_ACCEPT,
	URING_OP_READ,
	URING_OP_WRITE,
	URING_OP_TIMEOUT,	/* legacy — kept for migration; do NOT use for new timers */
	URING_OP_WAKEUP,
	URING_OP_CLOSE,
	URING_OP_UV_WRITE,
	/* Differentiated timer opcodes so on_timeout knows which timer fired */
	URING_OP_TMR_READ,
	URING_OP_TMR_WRITE,
	URING_OP_TMR_IDLE,
	URING_OP_TMR_REQ,
} uring_op_type_t;

typedef struct {
	csilk_client_t* client;
	void* data;
} uring_write_req_t;

typedef struct {
	uring_op_type_t op;
	void* ptr; // pointer to client or server
} uring_sqe_data_t;

// Helper to encode type and ptr into __u64
static inline __u64
uring_encode_data(uring_op_type_t op, csilk_client_t* client, void* ptr)
{
	uint64_t val = (uint64_t)ptr;
	val &= 0x0000FFFFFFFFFFFFULL;
	val |= ((uint64_t)op) << 56;
	if (client) {
		uint64_t gen = client->generation;
		val |= (gen << 48);
	}
	return val;
}

/* When the Submission Queue is full, submit pending entries to the kernel
 * to free SQE slots. Returns NULL only on ring-level failure. */
static inline struct io_uring_sqe*
uring_get_sqe_or_submit(struct io_uring* ring)
{
	struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		io_uring_submit(ring);
		sqe = io_uring_get_sqe(ring);
	}
	return sqe;
}

static inline void
uring_decode_data(__u64 val, uring_op_type_t* op, void** ptr, uint8_t* gen)
{
	*op = (uring_op_type_t)(val >> 56);
	*ptr = (void*)(val & 0x0000FFFFFFFFFFFFULL);
	if (gen) {
		*gen = (uint8_t)((val >> 48) & 0xFF);
	}
}

void csilk_uv_on_write_done(void* arg, ssize_t res);

/* Forward declarations for functions defined in uring_connection.c */
void on_read(csilk_client_t* client, ssize_t nread);
void on_write_done(void* arg, ssize_t res);
void on_timeout(csilk_client_t* client);
void client_destroy(csilk_client_t* client);
void csilk_client_close(csilk_client_t* client);
void on_close_done(csilk_client_t* client);

#endif
