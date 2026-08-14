/**
 * @file test_uring_io.c
 * @brief Tests for the io_uring I/O backend (csilk_io abstraction).
 *
 * Covers: loop init/close, TCP handle lifecycle, IP address conversion,
 * null-safety, and state-query functions. Requires CSILK_USE_URING.
 */

#include <assert.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include "csilk/core/sys_io.h"

#ifdef CSILK_USE_URING

/* ---- Loop lifecycle ---- */

static void
test_loop_init_close(void)
{
    printf("Testing csilk_io_loop_init / csilk_io_loop_close...\n");
    csilk_io_loop_t loop;
    int             rc = csilk_io_loop_init(&loop);
    assert(rc == 0);

    rc = csilk_io_loop_close(&loop);
    assert(rc == 0);
    printf("  passed\n");
}

/* ---- TCP handle lifecycle ---- */

static void
test_tcp_init_null(void)
{
    printf("Testing csilk_io_tcp_init null safety...\n");
    csilk_io_loop_t loop;
    csilk_io_loop_init(&loop);

    /* NULL handle should return -1 */
    int rc = csilk_io_tcp_init(&loop, NULL);
    assert(rc == -1);

    /* Valid handle should succeed */
    csilk_io_tcp_t handle;
    rc = csilk_io_tcp_init(&loop, &handle);
    assert(rc == 0);
    assert(handle.fd == -1);

    csilk_io_loop_close(&loop);
    printf("  passed\n");
}

static void
test_tcp_open_null(void)
{
    printf("Testing csilk_io_tcp_open null safety...\n");
    int rc = csilk_io_tcp_open(NULL, 0);
    assert(rc == -1);

    csilk_io_tcp_t handle;
    handle.fd = -1;
    rc = csilk_io_tcp_open(&handle, 42);
    assert(rc == 0);
    assert(handle.fd == 42);
    printf("  passed\n");
}

static void
test_tcp_bind_null(void)
{
    printf("Testing csilk_io_tcp_bind null safety...\n");
    /* NULL handle should fail */
    struct sockaddr_in addr;
    int                rc = csilk_io_tcp_bind(NULL, (const struct sockaddr*)&addr, 0);
    assert(rc == -1);

    /* Handle with fd < 0 should fail */
    csilk_io_tcp_t handle;
    handle.fd = -1;
    rc = csilk_io_tcp_bind(&handle, (const struct sockaddr*)&addr, 0);
    assert(rc == -1);
    printf("  passed\n");
}

static void
test_tcp_listen_null(void)
{
    printf("Testing csilk_io_listen null safety...\n");
    int rc = csilk_io_listen(NULL, 128, NULL);
    assert(rc == -1);

    csilk_io_stream_t stream;
    stream.fd = -1;
    rc = csilk_io_listen(&stream, 128, NULL);
    assert(rc == -1);
    printf("  passed\n");
}

static void
test_tcp_accept_null(void)
{
    printf("Testing csilk_io_accept null safety...\n");
    int rc = csilk_io_accept(NULL, NULL);
    assert(rc == -1);

    csilk_io_stream_t server;
    server.fd = -1;
    csilk_io_stream_t client;
    rc = csilk_io_accept(&server, &client);
    assert(rc == -1);
    printf("  passed\n");
}

/* ---- Null pointer tests for query functions ---- */

static void
test_is_closing_null(void)
{
    printf("Testing csilk_io_is_closing null safety...\n");
    int rc = csilk_io_is_closing(NULL);
    /* Should not crash; returns 0 per implementation */
    assert(rc == 0);
    printf("  passed\n");
}

static void
test_is_active_null(void)
{
    printf("Testing csilk_io_is_active null safety...\n");
    int rc = csilk_io_is_active(NULL);
    /* Returns 0 (inactive) for NULL handle — no crash */
    assert(rc == 0);
    printf("  passed\n");
}

static void
test_fileno_null(void)
{
    printf("Testing csilk_io_fileno null safety...\n");
    csilk_io_os_fd_t fd;
    int              rc = csilk_io_fileno(NULL, &fd);
    assert(rc == -1);

    csilk_io_os_fd_t fd2;
    rc = csilk_io_fileno(NULL, NULL);
    assert(rc == -1);
    printf("  passed\n");
}

static void
test_tcp_nodelay_null(void)
{
    printf("Testing csilk_io_tcp_nodelay null safety...\n");
    int rc = csilk_io_tcp_nodelay(NULL, 1);
    assert(rc == -1);
    printf("  passed\n");
}

static void
test_tcp_keepalive_null(void)
{
    printf("Testing csilk_io_tcp_keepalive null safety...\n");
    int rc = csilk_io_tcp_keepalive(NULL, 1, 60);
    assert(rc == -1);
    printf("  passed\n");
}

static void
test_tcp_getpeername_null(void)
{
    printf("Testing csilk_io_tcp_getpeername null safety...\n");
    struct sockaddr_storage ss;
    int                     slen = sizeof(ss);
    int                     rc = csilk_io_tcp_getpeername(NULL, (struct sockaddr*)&ss, &slen);
    assert(rc == -1);

    csilk_io_tcp_t handle;
    handle.fd = -1;
    rc = csilk_io_tcp_getpeername(&handle, (struct sockaddr*)&ss, &slen);
    assert(rc == -1);
    printf("  passed\n");
}

/* ---- IP address conversion ---- */

static void
test_ip4_addr_valid(void)
{
    printf("Testing csilk_io_ip4_addr (valid)...\n");
    struct sockaddr_in addr;
    int                rc = csilk_io_ip4_addr("127.0.0.1", 8080, &addr);
    assert(rc == 0);
    assert(addr.sin_family == AF_INET);
    assert(addr.sin_port == htons((uint16_t)8080));
    printf("  passed\n");
}

static void
test_ip4_addr_invalid(void)
{
    printf("Testing csilk_io_ip4_addr (invalid IP)...\n");
    struct sockaddr_in addr;
    int                rc = csilk_io_ip4_addr("not-an-ip", 80, &addr);
    assert(rc == -1);
    printf("  passed\n");
}

static void
test_ip4_addr_null(void)
{
    printf("Testing csilk_io_ip4_addr null safety...\n");
    struct sockaddr_in addr;
    int                rc = csilk_io_ip4_addr(NULL, 80, &addr);
    assert(rc == -1);

    rc = csilk_io_ip4_addr("127.0.0.1", 80, NULL);
    assert(rc == -1);
    printf("  passed\n");
}

static void
test_ip4_name_roundtrip(void)
{
    printf("Testing csilk_io_ip4_name roundtrip...\n");
    struct sockaddr_in addr;
    csilk_io_ip4_addr("192.168.1.100", 443, &addr);

    char name[64];
    int  rc = csilk_io_ip4_name(&addr, name, sizeof(name));
    assert(rc == 0);
    assert(strcmp(name, "192.168.1.100") == 0);

    /* Buffer too small */
    char tiny[2];
    rc = csilk_io_ip4_name(&addr, tiny, sizeof(tiny));
    assert(rc == -1);
    printf("  passed\n");
}

static void
test_ip4_name_null(void)
{
    printf("Testing csilk_io_ip4_name null safety...\n");
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;

    int rc = csilk_io_ip4_name(NULL, "dst", 64);
    assert(rc == -1);

    rc = csilk_io_ip4_name(&addr, NULL, 64);
    assert(rc == -1);
    printf("  passed\n");
}

/* ---- Time query ---- */

static void
test_io_now(void)
{
    printf("Testing csilk_io_now...\n");
    csilk_io_loop_t loop;
    csilk_io_loop_init(&loop);
    uint64_t t = csilk_io_now(&loop);
    assert(t > 0);
    csilk_io_loop_close(&loop);
    printf("  passed (now=%llu ms)\n", (unsigned long long)t);
}

static void
test_io_stop(void)
{
    printf("Testing csilk_io_stop...\n");
    csilk_io_loop_t loop;
    csilk_io_loop_init(&loop);
    csilk_io_stop(&loop);
    /* Should not crash */
    csilk_io_loop_close(&loop);
    printf("  passed\n");
}

static void
test_io_update_time(void)
{
    printf("Testing csilk_io_update_time...\n");
    csilk_io_loop_t loop;
    csilk_io_loop_init(&loop);
    csilk_io_update_time(&loop);
    /* Should not crash */
    csilk_io_loop_close(&loop);
    printf("  passed\n");
}

/* ---- Main ---- */

int
main(void)
{
    test_loop_init_close();
    test_tcp_init_null();
    test_tcp_open_null();
    test_tcp_bind_null();
    test_tcp_listen_null();
    test_tcp_accept_null();
    test_is_closing_null();
    test_is_active_null();
    test_fileno_null();
    test_tcp_nodelay_null();
    test_tcp_keepalive_null();
    test_tcp_getpeername_null();
    test_ip4_addr_valid();
    test_ip4_addr_invalid();
    test_ip4_addr_null();
    test_ip4_name_roundtrip();
    test_ip4_name_null();
    test_io_now();
    test_io_stop();
    test_io_update_time();

    printf("\nAll test_uring_io tests passed!\n");
    return 0;
}

#else  /* !CSILK_USE_URING */

int
main(void)
{
    printf("Skipping test_uring_io (CSILK_USE_URING not enabled)\n");
    return 0;
}

#endif /* CSILK_USE_URING */
