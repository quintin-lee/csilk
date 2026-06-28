#ifndef CSILK_URING_INTERNAL_H
#define CSILK_URING_INTERNAL_H

#include "csilk/csilk.h"
#include "csilk/core/sys_io.h"

// Define opcodes for user_data
typedef enum {
	URING_OP_ACCEPT,
	URING_OP_READ,
	URING_OP_WRITE,
	URING_OP_TIMEOUT,
	URING_OP_WAKEUP,
	URING_OP_CLOSE
} uring_op_type_t;

typedef struct {
	uring_op_type_t op;
	void* ptr; // pointer to client or server
} uring_sqe_data_t;

// Helper to encode type and ptr into __u64
static inline __u64
uring_encode_data(uring_op_type_t op, void* ptr)
{
	// Top 8 bits for op, lower 56 bits for ptr (safe on x86_64 and aarch64)
	uint64_t val = (uint64_t)ptr;
	val |= ((uint64_t)op) << 56;
	return val;
}

static inline void
uring_decode_data(__u64 val, uring_op_type_t* op, void** ptr)
{
	*op = (uring_op_type_t)(val >> 56);
	*ptr = (void*)(val & 0x00FFFFFFFFFFFFFFULL);
}

int csilk_uring_loop_init(csilk_io_loop_t* loop);
void csilk_uring_loop_run(csilk_io_loop_t* loop);
void csilk_uring_loop_close(csilk_io_loop_t* loop);

#endif
