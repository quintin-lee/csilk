#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include "gin.h"

static gin_server_t* g_server = NULL;

void idle_handler(gin_ctx_t* c) {
    gin_string(c, 200, "ok");
}

void* run_server(void* arg) {
    (void)arg;
    gin_router_t* router = gin_router_new();
    gin_group_t* group = gin_group_new(router, "/");
    gin_GET(group, "/idle", idle_handler);

    g_server = gin_server_new(router);
    gin_server_config_t cfg = { .idle_timeout_ms = 300, .max_body_size = 1048576, .listen_backlog = 128 };
    gin_server_set_config(g_server, cfg);
    gin_server_run(g_server, 8080);
    gin_server_free(g_server);
    gin_group_free(group);
    gin_router_free(router);
    return NULL;
}

int main() {
    pthread_t thread;
    pthread_create(&thread, NULL, run_server, NULL);
    usleep(200000);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(8080);
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        printf("FAIL: connect\n");
        gin_server_stop(g_server);
        pthread_join(thread, NULL);
        return 1;
    }
    send(sock, "GET /idle HTTP/1.1\r\nConnection: close\r\n\r\n", 44, 0);

    // Blocking read for response
    char buf[1024] = {0};
    int n = recv(sock, buf, sizeof(buf) - 1, 0);
    int passed = 1;
    if (n <= 0) {
        printf("FAIL: No response (n=%d)\n", n);
        passed = 0;
    } else {
        buf[n] = '\0';
        printf("  Response: %s", buf);
        if (!strstr(buf, "200 OK")) {
            printf("FAIL: Expected 200\n");
            passed = 0;
        } else {
            printf("  (got 200 OK)\n");
        }
    }

    close(sock);
    gin_server_stop(g_server);
    pthread_join(thread, NULL);
    return passed ? 0 : 1;
}
