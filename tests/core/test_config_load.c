#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "csilk/core/internal.h"
#include "csilk/csilk.h"

#define CONFIG_LOAD_PORT 8098
#define TEST_YAML "test_config_load_tmp.yaml"

static _Atomic int     server_ready = 0;
static csilk_server_t* g_server = nullptr;

static void
hello_handler(csilk_ctx_t* c)
{
    csilk_string(c, CSILK_STATUS_OK, "config_load_ok");
}

static void*
run_server(void* arg)
{
    (void)arg;
    csilk_router_t* router = csilk_router_new();
    csilk_handler_t h[] = {hello_handler};
    csilk_router_add(router, "GET", "/", h, 1);

    csilk_config_t config;
    int            ret = csilk_load_config(TEST_YAML, &config);
    if (ret != 0) {
        fprintf(stderr, "FAIL: csilk_load_config returned %d\n", ret);
        return nullptr;
    }

    const char* err = nullptr;
    ret = csilk_config_validate(&config, &err);
    if (ret != 0) {
        fprintf(stderr, "FAIL: config validation: %s\n", err ? err : "unknown");
        csilk_config_free(&config);
        return nullptr;
    }

    /* Verify key fields were parsed correctly */
    assert(config.port == CONFIG_LOAD_PORT);
    assert(config.server.idle_timeout_ms == 3000);
    assert(config.server.max_header_size == 16384);
    assert(config.server.max_connections == 100);
    assert(config.server.worker_threads == 1);
    assert(config.logger.level == CSILK_LOG_DEBUG);

    g_server = csilk_server_new(router);
    csilk_server_set_config(g_server, &config.server);
    csilk_config_free(&config);

    server_ready = 1;
    csilk_server_run(g_server, CONFIG_LOAD_PORT);
    return nullptr;
}

static int
do_request(const char* req, char* buf, size_t bufsize)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    }
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(CONFIG_LOAD_PORT);
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    send(sock, req, strlen(req), 0);
    fd_set         fds;
    struct timeval tv = {5, 0};
    FD_ZERO(&fds);
    FD_SET(sock, &fds);
    int ret = select(sock + 1, &fds, nullptr, nullptr, &tv);
    int n = 0;
    if (ret > 0) {
        n = recv(sock, buf, bufsize - 1, 0);
    }
    if (n > 0) {
        buf[n] = '\0';
    }
    close(sock);
    return n;
}

int
main()
{
    const char* yaml = "port: 8098\n"
                       "server:\n"
                       "  idle_timeout_ms: 3000\n"
                       "  max_body_size: 1048576\n"
                       "  max_header_size: 16384\n"
                       "  max_url_size: 4096\n"
                       "  max_headers_count: 50\n"
                       "  max_connections: 100\n"
                       "  listen_backlog: 64\n"
                       "  tcp_nodelay: 1\n"
                       "  tcp_keepalive: 30\n"
                       "  worker_threads: 1\n"
                       "logger:\n"
                       "  level: DEBUG\n"
                       "middleware:\n"
                       "  enable_logger: 1\n"
                       "  enable_recovery: 1\n";

    FILE* f = fopen(TEST_YAML, "w");
    if (!f) {
        perror("fopen");
        return 1;
    }
    fputs(yaml, f);
    fclose(f);
    csilk_log_config_t log_cfg;
    memset(&log_cfg, 0, sizeof(log_cfg));
    log_cfg.level = CSILK_LOG_DEBUG;
    csilk_log_init(log_cfg);

    pthread_t thread;
    pthread_create(&thread, nullptr, run_server, nullptr);

    int retries = 0;
    while (!server_ready && retries < 20) {
        usleep(100000);
        retries++;
    }
    if (!server_ready) {
        printf("FAIL: server failed to start\n");
        pthread_join(thread, nullptr);
        remove(TEST_YAML);
        return 1;
    }
    usleep(50000);

    char buf[8192] = {0};
    int  n = do_request(
        "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", buf, sizeof(buf));
    int passed = 1;
    if (n <= 0) {
        printf("FAIL: no response (n=%d)\n", n);
        passed = 0;
    } else if (!strstr(buf, "200 OK") || !strstr(buf, "config_load_ok")) {
        printf("FAIL: unexpected response: %s\n", buf);
        passed = 0;
    } else {
        printf("PASS: config_load test\n");
    }

    csilk_server_stop(g_server);
    pthread_join(thread, nullptr);
    remove(TEST_YAML);

    if (passed) {
        return 0;
    }

    printf("test_config_load: FAILED\n");
    return 1;
}
