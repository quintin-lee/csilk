/**
 * @file example_sse.c
 * @brief Standalone Server-Sent Events (SSE) example using csilk.
 *        Demonstrates periodic event streaming using a libuv timer.
 *
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

#include "csilk/csilk.h"

/* Structure to track state for each SSE stream */
typedef struct {
    pthread_t    thread;
    csilk_ctx_t* ctx;
    int          count;
} sse_stream_t;

/**
 * @brief Thread that streams SSE events to the client.
 */
static void*
sse_worker_thread(void* arg)
{
    sse_stream_t* stream = (sse_stream_t*)arg;

    while (stream->count < 10) {
        sleep(1);
        stream->count++;

        /* Format the event payload */
        char payload[128];
        snprintf(payload,
                 sizeof(payload),
                 "{\"tick\": %d, \"timestamp\": %ld}",
                 stream->count,
                 (long)time(NULL));

        printf("[SSE Stream] Sending event #%d to client\n", stream->count);

        /* Send event of type "message" (default) or "ticker" */
        csilk_sse_send(stream->ctx, "ticker", payload);
    }

    printf("[SSE Stream] Stream completed, closing connection.\n");
    csilk_sse_send(stream->ctx, "done", "Stream finished");
    csilk_sse_close(stream->ctx);

    free(stream);
    return NULL;
}

/**
 * @brief Handler for GET /events stream.
 */
static void
events_handler(csilk_ctx_t* c)
{
    /* Initialize the Server-Sent Events response */
    csilk_sse_init(c);

    /* Send a welcome/connection message immediately */
    csilk_sse_send(c, "welcome", "Connected to the SSE live stream!");

    /* Set up a background thread to push events every second */
    sse_stream_t* stream = malloc(sizeof(sse_stream_t));
    if (!stream) {
        csilk_sse_close(c);
        return;
    }

    stream->ctx = c;
    stream->count = 0;

    /* Start thread */
    pthread_create(&stream->thread, NULL, sse_worker_thread, stream);
    pthread_detach(stream->thread);

    printf("[SSE System] Stream connection initialized.\n");
}

int
main(void)
{
    /* Initialize router */
    csilk_router_t* router = csilk_router_new();
    csilk_group_t*  root = csilk_group_new(router, "");

    /* Middlewares */
    csilk_group_use(root, csilk_logger_handler);
    csilk_group_use(root, csilk_recovery_handler);

    /* Register SSE Route */
    csilk_GET(root, "/events", events_handler);

    /* Server setup */
    csilk_server_t* server = csilk_server_new(router);
    if (!server) {
        fprintf(stderr, "Failed to create csilk server\n");
        return 1;
    }

    printf("\n🚀 Starting Server-Sent Events (SSE) server on http://localhost:8080/events\n");
    printf("   - Listen with curl: 'curl -N http://localhost:8080/events'\n");

    /* Run server */
    csilk_server_run(server, 8080);

    /* Cleanup */
    csilk_server_free(server);
    csilk_group_free(root);
    csilk_router_free(router);

    return 0;
}
