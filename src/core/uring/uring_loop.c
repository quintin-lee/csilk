/**
 * @file uring_loop.c
 * @brief io_uring event loop lifecycle: init, close, stop, now, default_loop.
 */

#include "csilk/core/sys_io.h"

#ifdef CSILK_USE_URING

#include <string.h>
#include <stdlib.h>
#include "uring_internal.h"

/** @brief Number of slots in the shared UDP provided-buffer ring.
 * 8 slots are sufficient for the burst pattern of QUIC/UDP traffic; the
 * ring auto-recycles each slot as the kernel consumes it via
 * IORING_CQE_F_BUFFER, and a handle that finds no free slot simply
 * defers to the next CQE.
 */
#define CSILK_UDP_NBUFS 8

/* Each slot holds a full datagram payload (65536) plus the
 * io_uring_recvmsg_out header (16 bytes) and max sender address
 * (sizeof(sockaddr_storage) = 128 bytes).  256 bytes of extra headroom
 * covers the CMSG ancillary data that recvmsg may append. */
#define CSILK_UDP_BUFSZ (65536 + 256)

/** @brief Set up the shared UDP provided-buffer ring for the loop.
 *
 * Called lazily on the first csilk_io_udp_recv_start() that needs it.
 * All UDP handles on the loop share buf_group 0.  On success the
 * ring is fully registered and all CSILK_UDP_NBUFS slots are queued.
 * Returns 0 on success, or a negative errno on failure.
 *
 * Note: CSILK_UDP_BUFSZ must be at least 65536 (datagram max)
 * + sizeof(io_uring_recvmsg_out) + sizeof(sockaddr_storage).
 * We compute it as 65792 = 65536 + 256 for a comfortable margin. */
int
csilk_io_loop_setup_udp_buf_ring(csilk_io_loop_t* loop)
{
    if (!loop || loop->udp_buf_ring) {
        return 0; /* already set up */
    }

    int                       err = 0;
    struct io_uring_buf_ring* br =
        io_uring_setup_buf_ring(&loop->ring, CSILK_UDP_NBUFS, 0, 0, &err);
    if (!br) {
        return err < 0 ? err : -1;
    }

    /*
     * Allocate backing storage for all ring slots.  The kernel writes the
     * io_uring_recvmsg_out header + sender address + payload into each
     * registered slot, so the storage must be at least CSILK_UDP_BUFSZ
     * bytes per slot.  We keep it in a single contiguous allocation and
     * store it in loop->udp_storage for csilk_io_loop_close to free.
     */
    char* storage = calloc((size_t)CSILK_UDP_NBUFS * (size_t)CSILK_UDP_BUFSZ, 1);
    if (!storage) {
        io_uring_free_buf_ring(&loop->ring, br, CSILK_UDP_NBUFS, 0);
        return -1;
    }

    int mask = io_uring_buf_ring_mask(CSILK_UDP_NBUFS);
    for (int i = 0; i < CSILK_UDP_NBUFS; i++) {
        io_uring_buf_ring_add(br,
                              storage + (size_t)i * CSILK_UDP_BUFSZ,
                              (unsigned)CSILK_UDP_BUFSZ,
                              (unsigned short)i,
                              mask,
                              i);
    }
    io_uring_buf_ring_advance(br, CSILK_UDP_NBUFS);

    loop->udp_storage = storage;
    loop->udp_buf_ring = br;
    loop->udp_nbufs = CSILK_UDP_NBUFS;
    loop->udp_bufsz = CSILK_UDP_BUFSZ;
    return 0;
}

_Thread_local csilk_io_loop_t  g_default_loop;
_Thread_local int              g_default_pending = 0;
_Thread_local int              g_default_loop_inited = 0;
_Thread_local struct io_uring* g_default_ring_ptr = NULL;

/* ====================================================================
 * Loop lifecycle
 * ==================================================================== */

int
csilk_io_loop_init(csilk_io_loop_t* loop)
{
    if (!loop) {
        return -1;
    }
    memset(loop, 0, sizeof(*loop));

    int entries = 1024;
    int rc = -1;
    while (entries >= 64) {
        rc = io_uring_queue_init((unsigned)entries, &loop->ring, 0);
        if (rc == 0) {
            break;
        }
        entries /= 2;
    }
    if (rc < 0) {
        return rc;
    }

    loop->op_pool_capacity = (uint32_t)(entries * 2);
    loop->op_pool = calloc(loop->op_pool_capacity, sizeof(uring_op_context_t));
    loop->op_free_stack = malloc(loop->op_pool_capacity * sizeof(uint32_t));
    if (loop->op_pool && loop->op_free_stack) {
        for (uint32_t i = 0; i < loop->op_pool_capacity; i++) {
            loop->op_free_stack[i] = loop->op_pool_capacity - 1 - i;
            loop->op_pool[i].slot_idx = i;
        }
        loop->op_free_head = loop->op_pool_capacity;
    }
    return 0;
}

int
csilk_io_loop_close(csilk_io_loop_t* loop)
{
    if (!loop) {
        return -1;
    }
    _uring_deferred_drain_all();
    struct io_uring_cqe* cqe = NULL;
    while (io_uring_peek_cqe(&loop->ring, &cqe) == 0 && cqe) {
        uring_op_context_t* op_ctx = (uring_op_context_t*)(uintptr_t)cqe->user_data;
        if (op_ctx && op_ctx->slot_idx == UINT32_MAX) {
            free(op_ctx);
        }
        io_uring_cqe_seen(&loop->ring, cqe);
        cqe = NULL;
    }
    /* Release the shared UDP provided-buffer ring before exiting the ring. */
    if (loop->udp_buf_ring) {
        io_uring_free_buf_ring(&loop->ring, loop->udp_buf_ring, loop->udp_nbufs, 0);
        loop->udp_buf_ring = NULL;
        if (loop->udp_storage) {
            free(loop->udp_storage);
            loop->udp_storage = NULL;
        }
    }
    io_uring_queue_exit(&loop->ring);
    if (loop->op_pool) {
        free(loop->op_pool);
        loop->op_pool = NULL;
    }
    if (loop->op_free_stack) {
        free(loop->op_free_stack);
        loop->op_free_stack = NULL;
    }
    if (loop == &g_default_loop) {
        g_default_loop_inited = 0;
        g_default_ring_ptr = NULL;
    }
    return 0;
}

void
csilk_io_stop(csilk_io_loop_t* loop)
{
    if (loop) {
        loop->stop_flag = 1;
    }
}

uint64_t
csilk_io_now(const csilk_io_loop_t* loop)
{
    (void)loop;
    return csilk_io_hrtime() / 1000000ULL;
}

void
csilk_io_update_time(csilk_io_loop_t* loop)
{
    (void)loop;
}

csilk_io_loop_t*
csilk_io_default_loop(void)
{
    if (!g_default_loop_inited) {
        g_default_loop_inited = 1;
        if (csilk_io_loop_init(&g_default_loop) != 0) {
            return NULL;
        }
        g_default_ring_ptr = &g_default_loop.ring;
    }
    return &g_default_loop;
}

#endif
