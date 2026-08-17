#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "csilk/core/sys_io.h"

/* ====================================================================
 * Test 1: Loop Lifecycle & Time
 * ==================================================================== */

static void
test_loop_lifecycle(void)
{
    printf("Testing loop lifecycle and time functions...\n");
    csilk_io_loop_t loop;
    int             rc = csilk_io_loop_init(&loop);
    assert(rc == 0);

    uint64_t t1 = csilk_io_now(&loop);
    assert(t1 > 0);

    uint64_t hr = csilk_io_hrtime();
    assert(hr > 0);

    csilk_io_update_time(&loop);
    assert(csilk_io_loop_alive(&loop) == 0);

    rc = csilk_io_loop_close(&loop);
    assert(rc == 0);
    printf("test_loop_lifecycle: PASS\n");
}

/* ====================================================================
 * Test 2: TCP Socket Options & Queries
 * ==================================================================== */

static void
test_tcp_socket_options(void)
{
    printf("Testing TCP socket options and address queries...\n");
    csilk_io_loop_t loop;
    int             rc = csilk_io_loop_init(&loop);
    assert(rc == 0);

    csilk_io_tcp_t server;
    rc = csilk_io_tcp_init(&loop, &server);
    assert(rc == 0);

    struct sockaddr_in bind_addr;
    rc = csilk_io_ip4_addr("127.0.0.1", 0, &bind_addr);
    assert(rc == 0);

    rc = csilk_io_tcp_bind(&server, (const struct sockaddr*)&bind_addr, 0);
    assert(rc == 0);

    rc = csilk_io_tcp_nodelay(&server, 1);
    assert(rc == 0);

    rc = csilk_io_tcp_keepalive(&server, 1, 60);
    assert(rc == 0);

    csilk_io_os_fd_t fd = -1;
    rc = csilk_io_fileno((const csilk_io_handle_t*)&server, &fd);
    assert(rc == 0);
    assert(fd >= 0);

    struct sockaddr_in sock_addr;
    int                sock_len = sizeof(sock_addr);
    rc = csilk_io_tcp_getsockname(&server, (struct sockaddr*)&sock_addr, &sock_len);
    assert(rc == 0);
    assert(ntohs(sock_addr.sin_port) > 0);

    char ip_str[64];
    rc = csilk_io_ip4_name(&sock_addr, ip_str, sizeof(ip_str));
    assert(rc == 0);
    assert(strcmp(ip_str, "127.0.0.1") == 0);

    csilk_io_close((csilk_io_handle_t*)&server, NULL);
    csilk_io_run(&loop, CSILK_IO_RUN_NOWAIT);
    csilk_io_loop_close(&loop);
    printf("test_tcp_socket_options: PASS\n");
}

/* ====================================================================
 * Test 3: Timer Lifecycle
 * ==================================================================== */

static int g_timer_fired = 0;

static void
on_test_timer(csilk_io_timer_t* handle)
{
    g_timer_fired++;
    csilk_io_stop(handle->loop);
}

static void
test_timer_lifecycle(void)
{
    printf("Testing timer initialization, start, and expiration...\n");
    csilk_io_loop_t loop;
    int             rc = csilk_io_loop_init(&loop);
    assert(rc == 0);

    csilk_io_timer_t timer;
    rc = csilk_io_timer_init(&loop, &timer);
    assert(rc == 0);

    g_timer_fired = 0;
    rc = csilk_io_timer_start(&timer, on_test_timer, 10, 0);
    assert(rc == 0);
    assert(csilk_io_is_active((const csilk_io_handle_t*)&timer));

    csilk_io_run(&loop, CSILK_IO_RUN_DEFAULT);
    assert(g_timer_fired == 1);

    csilk_io_timer_stop(&timer);
    assert(!csilk_io_is_active((const csilk_io_handle_t*)&timer));

    csilk_io_close((csilk_io_handle_t*)&timer, NULL);
    csilk_io_run(&loop, CSILK_IO_RUN_NOWAIT);
    csilk_io_loop_close(&loop);
    printf("test_timer_lifecycle: PASS\n");
}

/* ====================================================================
 * Test 4: Async Cross-Thread Wakeup
 * ==================================================================== */

static int g_async_fired = 0;

static void
on_test_async(csilk_io_async_t* handle)
{
    g_async_fired++;
    csilk_io_stop(handle->loop);
}

static void*
async_sender_thread(void* arg)
{
    csilk_io_async_t* async = (csilk_io_async_t*)arg;
    csilk_io_sleep(10);
    csilk_io_async_send(async);
    return NULL;
}

static void
test_async_cross_thread(void)
{
    printf("Testing async cross-thread wakeup...\n");
    csilk_io_loop_t loop;
    int             rc = csilk_io_loop_init(&loop);
    assert(rc == 0);

    csilk_io_async_t async;
    rc = csilk_io_async_init(&loop, &async, on_test_async);
    assert(rc == 0);

    g_async_fired = 0;
    pthread_t th;
    pthread_create(&th, NULL, async_sender_thread, &async);

    csilk_io_run(&loop, CSILK_IO_RUN_DEFAULT);
    pthread_join(th, NULL);

    assert(g_async_fired >= 1);

    csilk_io_close((csilk_io_handle_t*)&async, NULL);
    csilk_io_run(&loop, CSILK_IO_RUN_NOWAIT);
    csilk_io_loop_close(&loop);
    printf("test_async_cross_thread: PASS\n");
}

/* ====================================================================
 * Test 5: Error and Memory Utilities
 * ==================================================================== */

static void
test_error_and_memory(void)
{
    printf("Testing error formatting and memory queries...\n");
    const char* str = csilk_io_strerror(0);
    assert(str != NULL);

    const char* name = csilk_io_err_name(0);
    assert(name != NULL);

    size_t rss = 0;
    int    rc = csilk_io_resident_set_memory(&rss);
    assert(rc == 0);

    csilk_io_rusage_t ru;
    rc = csilk_io_getrusage(&ru);
    assert(rc == 0);

    printf("test_error_and_memory: PASS\n");
}

/* ====================================================================
 * Main
 * ==================================================================== */

int
main(void)
{
    printf("=== Running Backend-Neutral Csilk IO Tests ===\n\n");
    test_loop_lifecycle();
    test_tcp_socket_options();
    test_timer_lifecycle();
    test_async_cross_thread();
    test_error_and_memory();
    printf("\n=== All Backend-Neutral Csilk IO Tests Passed! ===\n");
    return 0;
}
