#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include "gin.h"

// Simple handler that does nothing, keeping connection open
void idle_handler(gin_ctx_t* c) {
    (void)c;
}

void* run_server(void* arg) {
    gin_router_t* router = gin_router_new();
    gin_group_t* group = gin_group_new(router, "/");
    gin_GET(group, "/idle", idle_handler);
    
    gin_server_t* server = gin_server_new(router);
    // Setting a 1s timeout
    // gin_server_set_timeouts(server, 5000, 1000, 1000); 
    
    gin_server_run(server, 8080);
    
    gin_server_free(server);
    gin_group_free(group);
    gin_router_free(router);
    return NULL;
}

int main() {
    pthread_t thread;
    pthread_create(&thread, NULL, run_server, NULL);
    sleep(1); // wait for server to start

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(8080);
    
    connect(sock, (struct sockaddr*)&addr, sizeof(addr));
    send(sock, "GET /idle HTTP/1.1\r\nConnection: keep-alive\r\n\r\n", 45, 0);
    
    // Wait longer than the timeout (1000ms = 1s)
    sleep(2);
    
    char buf[1024];
    int n = recv(sock, buf, sizeof(buf), 0);
    
    // Should be closed by now
    if (n == 0) {
        printf("PASS: Connection closed by server\n");
        return 0;
    } else {
        printf("FAIL: Connection still open\n");
        return 1;
    }
}
