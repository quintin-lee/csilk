/**
 * @file example_file_io.c
 * @brief File upload, download, range requests, and streaming response with backpressure.
 *
 * Demonstrates chunked response writing, file serving, Range header support,
 * and outbound backpressure handling via csilk_response_on_drain.
 *
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csilk/csilk.h"
#include "csilk/core/middleware.h"

/* ====================================================================
 *  Streaming response helpers
 * ==================================================================== */

static void
drain_resume_callback(csilk_ctx_t* drain_c, void* arg)
{
    (void)drain_c;
    printf("[drain] resumed at %lld bytes\n", (long long)(intptr_t)arg);
}

/**
 * @brief Stream a large JSON payload in chunks to demonstrate backpressure.
 *
 * When csilk_response_write() returns 0, the outbound queue has reached
 * the high watermark. The handler should pause and resume via the drain
 * callback registered with csilk_response_on_drain().
 */
static void
stream_large_response_handler(csilk_ctx_t* c)
{
    csilk_response_on_drain(c, drain_resume_callback, (void*)(intptr_t)12345);

    csilk_set_header(c, "Content-Type", "application/json");
    csilk_set_header(c, "X-Stream-Id", "chunked-001");

    int   chunk_size = 1024;
    char* buffer = malloc(chunk_size + 1);
    if (!buffer) {
        csilk_json_error(c, 500, "OOM");
        return;
    }

    /* Write the JSON opening */
    int rc = csilk_response_write(c, (const uint8_t*)"{\"data\":[", 9);
    if (rc < 0) {
        free(buffer);
        csilk_json_error(c, 500, "write failed");
        return;
    }

    for (int i = 0; i < 50; i++) {
        int written = snprintf(
            buffer, chunk_size, "{\"id\":%d,\"value\":\"chunk-%d\"}%s", i, i, i < 49 ? "," : "");
        if (written < 0 || written >= chunk_size) {
            continue;
        }

        rc = csilk_response_write(c, (const uint8_t*)buffer, (size_t)written);
        if (rc == 0) {
            printf("[backpressure] paused at chunk %d\n", i);
            break;
        }
        if (rc < 0) {
            free(buffer);
            csilk_json_error(c, 500, "write failed");
            return;
        }
    }

    csilk_response_write(c, (const uint8_t*)" ]}", 3);
    free(buffer);
}

/**
 * @brief Demonstrate Range header support for partial content downloads.
 */
static void
range_download_handler(csilk_ctx_t* c)
{
    const char* filename = csilk_get_param(c, "filename");
    if (!filename) {
        csilk_json_error(c, 400, "filename required");
        return;
    }

    /* Generate deterministic "file" content for demo purposes */
    const char* content = "This is sample file content for range request testing.\n"
                          "Line 2: More content here.\n"
                          "Line 3: Almost there.\n"
                          "Line 4: Final line.\n";
    size_t      total_len = strlen(content);

    /* Parse Range header: "bytes=START-END" or "bytes=START-" */
    const char* range_hdr = csilk_get_header(c, "Range");
    if (!range_hdr || strncmp(range_hdr, "bytes=", 6) != 0) {
        /* No range — send full content */
        csilk_set_header(c, "Accept-Ranges", "bytes");
        csilk_string(c, 200, content);
        return;
    }

    const char* spec = range_hdr + 6; /* skip "bytes=" */
    long        start = 0, end = (long)total_len - 1;

    const char* dash = strchr(spec, '-');
    if (dash) {
        if (dash == spec) {
            long suffix = atol(dash + 1);
            start = suffix > 0 ? (long)total_len - suffix : 0;
            if (start < 0) {
                start = 0;
            }
        } else {
            start = atol(spec);
            end = atol(dash + 1);
        }
    } else {
        start = atol(spec);
    }

    if (start < 0) {
        start = 0;
    }
    if (end < 0 || end >= (long)total_len) {
        end = (long)total_len - 1;
    }
    if (start > end) {
        start = end;
    }

    size_t length = (size_t)(end - start + 1);

    csilk_set_header(c, "Accept-Ranges", "bytes");
    char range_val[64];
    snprintf(range_val, sizeof(range_val), "bytes %ld-%ld/%ld", start, end, (long)total_len);
    csilk_set_header(c, "Content-Range", range_val);

    csilk_set_status(c, 206);
    csilk_string(c, 206, content + start);
}

/**
 * @brief Handle file upload via URL-encoded form data.
 */
static void
upload_handler(csilk_ctx_t* c)
{
    csilk_parse_form_urlencoded(c);

    const char* filename = csilk_get_form_field(c, "filename");
    const char* content = csilk_get_form_field(c, "content");

    if (!filename || !content) {
        csilk_json_error(c, 400, "filename and content required");
        return;
    }

    size_t len = strlen(content);

    csilk_json_t* obj = csilk_json_object();
    csilk_json_add_string(obj, "filename", filename);
    csilk_json_add_int(obj, "size", (int64_t)len);
    csilk_json_add_string(obj, "content_hash", "demo");
    csilk_json_add_bool(obj, "uploaded", true);
    csilk_json(c, 201, obj);
}

/**
 * @brief Demonstrate arena-backed file download with custom headers.
 */
static void
file_read_handler(csilk_ctx_t* c)
{
    const char* content = "Hello from server-generated file!\nSecond line of demo content.\n";
    size_t      len = strlen(content);

    csilk_set_header(c, "Content-Type", "text/plain");
    csilk_set_header(c, "Content-Disposition", "attachment; filename=\"demo.txt\"");

    /* Use arena-backed body for efficient lifecycle management */
    csilk_arena_t* arena = csilk_get_arena(c);
    char*          arena_body = csilk_arena_strndup(arena, content, len);
    if (!arena_body) {
        csilk_json_error(c, 500, "OOM");
        return;
    }

    csilk_set_response_body_ex(c, arena_body, len, CSILK_OWN_ARENA);
}

static void
file_health_handler(csilk_ctx_t* c)
{
    (void)c;
    csilk_json_string(c, 200, "{\"status\":\"ok\"}");
}

/* ====================================================================
 *  Main
 * ==================================================================== */

int
main(void)
{
    csilk_router_t* router = csilk_router_new();
    csilk_group_t*  root = csilk_group_new(router, "");

    csilk_group_use(root, csilk_recovery_handler);
    csilk_group_use(root, csilk_logger_handler);
    csilk_group_use(root, csilk_gzip_middleware);

    csilk_GET(root, "/stream", stream_large_response_handler);
    csilk_GET(root, "/download/:filename", range_download_handler);
    csilk_POST(root, "/upload", upload_handler);
    csilk_GET(root, "/file", file_read_handler);
    csilk_GET(root, "/health", file_health_handler);

    csilk_server_t* server = csilk_server_new(router);
    if (!server) {
        fprintf(stderr, "Failed to create server\n");
        return 1;
    }

    printf("\n=== File I/O & Streaming Example ===\n");
    printf("GET  /stream              — Chunked JSON with backpressure demo\n");
    printf("GET  /download/:filename  — Range header partial content (206)\n");
    printf("POST /upload              — URL-encoded form upload\n");
    printf("GET  /file                — Arena-backed file download\n");
    printf("\nRange test:\n");
    printf("  curl -H 'Range: bytes=0-9' http://localhost:8080/download/demo\n");
    printf("  curl -H 'Range: bytes=-5' http://localhost:8080/download/demo\n");
    printf("Listen: http://localhost:8080\n");

    csilk_server_run(server, 8080);

    csilk_server_free(server);
    csilk_group_free(root);
    csilk_router_free(router);
    return 0;
}
