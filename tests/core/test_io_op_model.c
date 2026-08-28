/**
 * @file test_io_op_model.c
 * @brief Tests for explicit async I/O operation lifetime model.
 */

#include "csilk/core/sys_io.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef CSILK_USE_URING

static int complete_count = 0;
static int cancel_count = 0;

static void
on_complete(csilk_io_op_t* op, int status)
{
    (void)status;
    assert(op != NULL);
    assert(atomic_load(&op->state) == CSILK_IO_OP_STATE_COMPLETED);
    complete_count++;
}

static void
on_cancel(csilk_io_op_t* op)
{
    (void)op;
    cancel_count++;
}

static void
test_io_op_lifecycle(void)
{
    complete_count = 0;
    cancel_count = 0;
    csilk_io_op_t op;
    csilk_io_op_init(&op, CSILK_IO_OP_WRITE, (void*)0x1, 10);

    assert(csilk_io_op_submit(&op) == 0);
    assert(atomic_load(&op.state) == CSILK_IO_OP_STATE_SUBMITTED);

    assert(csilk_io_op_complete(&op, 0) == 0);
    assert(atomic_load(&op.state) == CSILK_IO_OP_STATE_COMPLETED);

    assert(csilk_io_op_retire(&op) == 0);
    assert(atomic_load(&op.state) == CSILK_IO_OP_STATE_RETIRED);

    assert(csilk_io_op_cancel(&op) == -1);
    assert(csilk_io_op_complete(&op, -1) == -1);
    assert(csilk_io_op_retire(&op) == -1);
}

static void
test_io_op_stale_and_double_complete(void)
{
    csilk_io_op_t op;
    csilk_io_op_init(&op, CSILK_IO_OP_READ, (void*)0x2, 7);
    assert(csilk_io_op_submit(&op) == 0);

    assert(csilk_io_op_is_stale(&op, 8) == 1);
    assert(csilk_io_op_is_stale(&op, 7) == 0);

    assert(csilk_io_op_complete(&op, 0) == 0);
    assert(csilk_io_op_complete(&op, 0) == -1);

    assert(csilk_io_op_cancel(&op) == -1);
    assert(csilk_io_op_retire(&op) == 0);

    csilk_io_op_init(&op, CSILK_IO_OP_TIMER, NULL, 0);
    assert(csilk_io_op_is_stale(&op, 0) == 0);
    assert(csilk_io_op_is_stale(&op, 1) == 1);
}

static void
test_io_op_cancel_cleanup(void)
{
    complete_count = 0;
    cancel_count = 0;
    csilk_io_op_t op;
    csilk_io_op_init(&op, CSILK_IO_OP_WRITE, (void*)0x3, 5);
    op.complete = on_complete;
    op.cancel = on_cancel;

    assert(csilk_io_op_submit(&op) == 0);
    assert(csilk_io_op_cancel(&op) == 0);
    assert(atomic_load(&op.state) == CSILK_IO_OP_STATE_CANCEL_REQUESTED);
    assert(cancel_count == 1);

    assert(csilk_io_op_complete(&op, -1) == -1);
    assert(atomic_load(&op.state) == CSILK_IO_OP_STATE_CANCEL_REQUESTED);
    assert(complete_count == 0);

    assert(csilk_io_op_retire(&op) == -1);

    csilk_io_op_init(&op, CSILK_IO_OP_CLOSE, (void*)0x4, 1);
    op.cancel = on_cancel;
    assert(csilk_io_op_submit(&op) == 0);
    assert(csilk_io_op_cancel(&op) == 0);
    assert(csilk_io_op_retire(&op) == -1);
}

static void
test_io_op_null_safety(void)
{
    assert(csilk_io_op_init(NULL, CSILK_IO_OP_NONE, NULL, 0) == -1);
    assert(csilk_io_op_submit(NULL) == -1);
    assert(csilk_io_op_complete(NULL, -1) == -1);
    assert(csilk_io_op_cancel(NULL) == -1);
    assert(csilk_io_op_retire(NULL) == -1);
    assert(csilk_io_op_is_stale(NULL, 1) == 0);
}

static void
test_io_op_invalid_transitions(void)
{
    csilk_io_op_t op;
    csilk_io_op_init(&op, CSILK_IO_OP_FS, (void*)0x5, 2);

    assert(csilk_io_op_retire(&op) == -1);
    assert(csilk_io_op_cancel(&op) == -1);
    assert(atomic_load(&op.state) == CSILK_IO_OP_STATE_CREATED);

    assert(csilk_io_op_submit(&op) == 0);
    assert(csilk_io_op_submit(&op) == -1);
    assert(csilk_io_op_retire(&op) == -1);
    assert(csilk_io_op_complete(&op, 0) == 0);
    assert(csilk_io_op_cancel(&op) == -1);
    assert(csilk_io_op_retire(&op) == 0);
}

#else

static void
test_io_op_lifecycle(void)
{
    printf("Skipping io_uring-only operation lifecycle tests.\n");
}

static void
test_io_op_stale_and_double_complete(void)
{
    printf("Skipping io_uring-only stale operation tests.\n");
}

static void
test_io_op_cancel_cleanup(void)
{
    printf("Skipping io_uring-only operation cancel tests.\n");
}

static void
test_io_op_null_safety(void)
{
    printf("Skipping io_uring-only operation null-safety tests.\n");
}

static void
test_io_op_invalid_transitions(void)
{
    printf("Skipping io_uring-only operation transition tests.\n");
}

#endif

int
main(void)
{
    test_io_op_lifecycle();
    test_io_op_stale_and_double_complete();
    test_io_op_cancel_cleanup();
    test_io_op_null_safety();
    test_io_op_invalid_transitions();
    printf("io operation model tests passed.\n");
    return EXIT_SUCCESS;
}
