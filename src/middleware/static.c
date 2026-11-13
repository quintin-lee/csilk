/**
 * @file static.c
 * @brief Static file serving middleware implementation.
 * @copyright MIT License
 */

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "csilk/core/internal.h"
#include "core/ctx/ctx_internal.h"

typedef struct {
    csilk_ctx_t* ctx;
    char         root_dir[PATH_MAX];
    char         prefix[PATH_MAX];
    char         request_path[PATH_MAX];
    char         range_header[128];
    bool         has_range;
    bool         is_file_mode;

    /* Results filled by threadpool worker, consumed on event loop */
    int         status_code;
    const char* error_msg;
    int         fd;
    size_t      file_offset;
    size_t      file_length;
    size_t      total_size;
    char        content_range[128];
    char        mime_type[64];
    bool        is_range_response;
} static_work_data_t;

/**
 * @brief Internal helper to get MIME type from file path.
 *
 * Maps the file extension to a MIME type string. Currently supports .html,
 * .css, and .js. All other extensions (including files without an extension)
 * return application/octet-stream.
 *
 * @param path  The file path to evaluate.
 *
 * @return A static string pointer to the MIME type. Must NOT be freed.
 */
static const char*
get_mime_type(const char* path)
{
    const char* ext = strrchr(path, '.');
    if (!ext) {
        return "application/octet-stream";
    }
    if (strcmp(ext, ".html") == 0) {
        return "text/html";
    }
    if (strcmp(ext, ".css") == 0) {
        return "text/css";
    }
    if (strcmp(ext, ".js") == 0) {
        return "application/javascript";
    }
    return "application/octet-stream";
}

static void
compute_range_response(static_work_data_t* data)
{
    if (!data->has_range || strncmp(data->range_header, "bytes=", 6) != 0) {
        data->is_range_response = false;
        data->status_code = CSILK_STATUS_OK;
        data->file_offset = 0;
        data->file_length = data->total_size;
        return;
    }

    const char* range_val = data->range_header + 6;
    char*       dash = strchr((char*)range_val, '-');
    if (!dash) {
        data->status_code = CSILK_STATUS_RANGE_NOT_SATISFIABLE;
        return;
    }

    if (data->total_size == 0) {
        data->status_code = CSILK_STATUS_RANGE_NOT_SATISFIABLE;
        return;
    }

    long long range_start = 0;
    long long range_end = (long long)data->total_size - 1;

    if (range_val == dash) {
        /* Suffix range: bytes=-N */
        char*     endptr = NULL;
        long long suffix_len = strtoll(dash + 1, &endptr, 10);
        if (endptr == dash + 1 || suffix_len <= 0) {
            data->status_code = CSILK_STATUS_RANGE_NOT_SATISFIABLE;
            return;
        }
        if (suffix_len >= (long long)data->total_size) {
            range_start = 0;
        } else {
            range_start = (long long)data->total_size - suffix_len;
        }
        range_end = (long long)data->total_size - 1;
    } else {
        /* Prefix or closed range: bytes=N- or bytes=N-M */
        char* endptr = NULL;
        range_start = strtoll(range_val, &endptr, 10);
        if (endptr == range_val || range_start < 0 || range_start >= (long long)data->total_size) {
            data->status_code = CSILK_STATUS_RANGE_NOT_SATISFIABLE;
            return;
        }
        if (*(dash + 1) != '\0') {
            endptr = NULL;
            range_end = strtoll(dash + 1, &endptr, 10);
            if (endptr == dash + 1) {
                data->status_code = CSILK_STATUS_RANGE_NOT_SATISFIABLE;
                return;
            }
            if (range_end >= (long long)data->total_size) {
                range_end = (long long)data->total_size - 1;
            }
        }
    }

    if (range_start > range_end || range_start >= (long long)data->total_size) {
        data->status_code = CSILK_STATUS_RANGE_NOT_SATISFIABLE;
        return;
    }

    data->is_range_response = true;
    data->file_offset = (size_t)range_start;
    data->file_length = (size_t)(range_end - range_start + 1);
    snprintf(data->content_range,
             sizeof(data->content_range),
             "bytes %lld-%lld/%zu",
             range_start,
             range_end,
             data->total_size);
    data->status_code = CSILK_STATUS_PARTIAL_CONTENT;
}

/**
 * @brief Work callback: resolve and open the requested static file.
 *
 * Runs on the thread pool. Resolves the requested path relative to
 * the configured root directory, performs a path traversal check (ensuring
 * the resolved path stays within the root), opens the file, stats it,
 * determines the MIME type, and configures the response (including any
 * range handling). The actual file content is sent later in
 * static_after_work_cb via _csilk_send_response.
 *
 * @param req  I/O work request. req->data points to the csilk_ctx_t.
 *             The context must have "static_root" and "static_prefix"
 *             set via csilk_set().
 *
 * @note Path traversal protection uses realpath() to canonicalize both
 *       root and requested paths, then verifies the requested path starts
 *       with the root prefix.
 */
/** @brief Check if a string contains null bytes or ASCII control characters.
 *
 *  Sanitizes file paths by rejecting any string that contains a byte below
 *  0x20 (control characters: null, tab, newline, etc.).  A NULL input
 *  is treated as unsafe (returns 1).
 *
 *  @param str  The string to scan (may be NULL).
 *  @return 1 if unsafe (NULL or contains control chars), 0 if safe. */
static int
contains_null_or_control(const char* str)
{
    if (!str) {
        return 1;
    }
    while (*str) {
        if ((unsigned char)*str < 0x20) {
            return 1;
        }
        ++str;
    }
    return 0;
}

/** @brief Check if a path contains "../" or "..\" directory traversal
 *  sequences.
 *
 *  Scans the path string for literal ".." segments (with '/' or '\'
 *  separators).  This is a defense-in-depth check layered on top of the
 *  realpath()-based canonicalisation in the static file handler.  A
 *  NULL input is treated as unsafe (returns 1).
 *
 *  @param path  The file path to scan (may be NULL).
 *  @return 1 if a traversal sequence is detected, 0 if the path appears
 *          safe for this check. */
static int
contains_path_traversal(const char* path)
{
    if (!path) {
        return 1;
    }
    const char* p = path;
    while (*p) {
        if (p[0] == '.' && p[1] == '.') {
            char prev = (p == path) ? '/' : *(p - 1);
            char next = *(p + 2);
            int  is_prev_sep = (prev == '/' || prev == '\\' || prev == '\0');
            int  is_next_sep = (next == '/' || next == '\\' || next == '\0');
            if (is_prev_sep && is_next_sep) {
                return 1;
            }
        }
        ++p;
    }
    return 0;
}

static void
static_work_cb(csilk_io_work_t* req)
{
    static_work_data_t* data = (static_work_data_t*)req->data;
    char                full_path[PATH_MAX];
    char                resolved_root[PATH_MAX];
    char                resolved_file[PATH_MAX];

    if (contains_null_or_control(data->request_path)) {
        data->status_code = CSILK_STATUS_FORBIDDEN;
        data->error_msg = "Forbidden";
        return;
    }

    if (contains_path_traversal(data->request_path)) {
        data->status_code = CSILK_STATUS_FORBIDDEN;
        data->error_msg = "Forbidden";
        return;
    }

    if (realpath(data->root_dir, resolved_root) == NULL) {
        data->status_code = CSILK_STATUS_INTERNAL_SERVER_ERROR;
        data->error_msg = "Internal Server Error";
        return;
    }

    const char* relative_path = data->request_path;
    if (data->prefix[0] != '\0' &&
        strncmp(relative_path, data->prefix, strlen(data->prefix)) == 0) {
        relative_path += strlen(data->prefix);
        if (*relative_path == '/') {
            relative_path++;
        }
    }

    int nwritten = snprintf(full_path, sizeof(full_path), "%s/%s", data->root_dir, relative_path);
    if (nwritten < 0 || (size_t)nwritten >= sizeof(full_path)) {
        data->status_code = CSILK_STATUS_INTERNAL_SERVER_ERROR;
        data->error_msg = "Internal Server Error";
        return;
    }

    if (realpath(full_path, resolved_file) == NULL) {
        data->status_code = CSILK_STATUS_NOT_FOUND;
        data->error_msg = "Not Found";
        return;
    }

    size_t root_len = strlen(resolved_root);
    if (strncmp(resolved_root, resolved_file, root_len) != 0 ||
        (resolved_file[root_len] != '/' && resolved_file[root_len] != '\0')) {
        data->status_code = CSILK_STATUS_FORBIDDEN;
        data->error_msg = "Forbidden";
        return;
    }

    csilk_io_fs_t open_req;
    int           fd = csilk_io_fs_open(NULL, &open_req, resolved_file, O_RDONLY, 0, NULL);
    csilk_io_fs_req_cleanup(&open_req);
    if (fd < 0) {
        data->status_code = CSILK_STATUS_NOT_FOUND;
        data->error_msg = "Not Found";
        return;
    }

    csilk_io_fs_t stat_req;
    csilk_io_fs_fstat(NULL, &stat_req, fd, NULL);
    if (!S_ISREG(stat_req.statbuf.st_mode)) {
        csilk_io_fs_req_cleanup(&stat_req);
        csilk_io_fs_t close_req;
        csilk_io_fs_close(NULL, &close_req, fd, NULL);
        data->status_code = CSILK_STATUS_FORBIDDEN;
        data->error_msg = "Forbidden";
        return;
    }

    data->fd = fd;
    data->total_size = stat_req.statbuf.st_size;
    csilk_io_fs_req_cleanup(&stat_req);

    const char* mime = get_mime_type(resolved_file);
    strncpy(data->mime_type, mime, sizeof(data->mime_type) - 1);
    data->mime_type[sizeof(data->mime_type) - 1] = '\0';

    compute_range_response(data);
}

/** @brief Work callback: open and stat a specific file.
 *
 * Runs in the thread pool. Opens the file specified by data->root_dir
 * and sets up range response.
 *
 * @param req I/O work request. req->data points to static_work_data_t.
 */
static void
file_work_cb(csilk_io_work_t* req)
{
    static_work_data_t* data = (static_work_data_t*)req->data;

    csilk_io_fs_t open_req;
    int           fd = csilk_io_fs_open(NULL, &open_req, data->root_dir, O_RDONLY, 0, NULL);
    csilk_io_fs_req_cleanup(&open_req);
    if (fd < 0) {
        data->status_code = CSILK_STATUS_NOT_FOUND;
        data->error_msg = "Not Found";
        return;
    }

    csilk_io_fs_t stat_req;
    csilk_io_fs_fstat(NULL, &stat_req, fd, NULL);
    if (!S_ISREG(stat_req.statbuf.st_mode)) {
        csilk_io_fs_req_cleanup(&stat_req);
        csilk_io_fs_t close_req;
        csilk_io_fs_close(NULL, &close_req, fd, NULL);
        data->status_code = CSILK_STATUS_FORBIDDEN;
        data->error_msg = "Forbidden";
        return;
    }

    data->fd = fd;
    data->total_size = stat_req.statbuf.st_size;
    csilk_io_fs_req_cleanup(&stat_req);

    const char* mime = get_mime_type(data->root_dir);
    strncpy(data->mime_type, mime, sizeof(data->mime_type) - 1);
    data->mime_type[sizeof(data->mime_type) - 1] = '\0';

    compute_range_response(data);
}

/**
 * @brief After-work callback: apply response and send on event loop thread.
 */
static void
static_after_work_cb(csilk_io_work_t* req, int status)
{
    static_work_data_t* data = (static_work_data_t*)req->data;
    if (!data) {
        return;
    }
    csilk_ctx_t* c = data->ctx;
    req->data = c;

    if (status < 0) {
        if (data->fd >= 0) {
            csilk_io_fs_t close_req;
            csilk_io_fs_close(NULL, &close_req, data->fd, NULL);
        }
        free(data);
        _csilk_ctx_async_ref_decr(c);
        return;
    }

    if (data->status_code == CSILK_STATUS_OK) {
        csilk_set_header(c, "Content-Type", data->mime_type);
        csilk_set_header(c, "Accept-Ranges", "bytes");
        csilk_status(c, CSILK_STATUS_OK);
        csilk_set_file_response(c, data->fd, 0, data->total_size);
    } else if (data->status_code == CSILK_STATUS_PARTIAL_CONTENT) {
        csilk_set_header(c, "Content-Range", data->content_range);
        csilk_set_header(c, "Content-Type", data->mime_type);
        csilk_set_header(c, "Accept-Ranges", "bytes");
        csilk_status(c, CSILK_STATUS_PARTIAL_CONTENT);
        csilk_set_file_response(c, data->fd, data->file_offset, data->file_length);
    } else {
        if (data->fd >= 0) {
            csilk_io_fs_t close_req;
            csilk_io_fs_close(NULL, &close_req, data->fd, NULL);
        }
        if (data->error_msg) {
            csilk_string(c, data->status_code, data->error_msg);
        } else {
            csilk_status(c, data->status_code);
        }
    }

    _csilk_send_response(c);
    _csilk_ctx_async_ref_decr(c);
    free(data);
}

void
csilk_static(csilk_ctx_t* c, const char* root_dir)
{
    if (!c || !root_dir) {
        return;
    }
    static_work_data_t* data = calloc(1, sizeof(static_work_data_t));
    if (!data) {
        csilk_status(c, CSILK_STATUS_INTERNAL_SERVER_ERROR);
        _csilk_send_response(c);
        return;
    }
    data->ctx = c;
    data->fd = -1;
    strncpy(data->root_dir, root_dir, sizeof(data->root_dir) - 1);

    const char* prefix = (const char*)csilk_get(c, "static_prefix");
    if (prefix) {
        strncpy(data->prefix, prefix, sizeof(data->prefix) - 1);
    }
    const char* path = csilk_get_path(c);
    if (path) {
        strncpy(data->request_path, path, sizeof(data->request_path) - 1);
    }
    const char* range = csilk_get_header(c, "Range");
    if (range) {
        data->has_range = true;
        strncpy(data->range_header, range, sizeof(data->range_header) - 1);
    }

    csilk_ctx_set_async(c, 1);
    csilk_io_work_t* req = csilk_get_work_req(c);
    req->data = data;
    csilk_io_loop_t* loop = _csilk_ctx_loop(c);
    _csilk_ctx_async_ref_incr(c);
    int rc = csilk_io_queue_work(loop, req, static_work_cb, static_after_work_cb);
    if (rc != 0) {
        CSILK_LOG_E("Static: failed to queue work: %d", rc);
        _csilk_ctx_async_ref_decr(c);
        free(data);
        req->data = c;
        csilk_status(c, CSILK_STATUS_INTERNAL_SERVER_ERROR);
        _csilk_send_response(c);
    }
}

void
csilk_file(csilk_ctx_t* c, const char* file_path)
{
    if (!c || !file_path) {
        return;
    }
    static_work_data_t* data = calloc(1, sizeof(static_work_data_t));
    if (!data) {
        csilk_status(c, CSILK_STATUS_INTERNAL_SERVER_ERROR);
        _csilk_send_response(c);
        return;
    }
    data->ctx = c;
    data->fd = -1;
    data->is_file_mode = true;
    strncpy(data->root_dir, file_path, sizeof(data->root_dir) - 1);

    const char* range = csilk_get_header(c, "Range");
    if (range) {
        data->has_range = true;
        strncpy(data->range_header, range, sizeof(data->range_header) - 1);
    }

    csilk_ctx_set_async(c, 1);
    csilk_io_work_t* req = csilk_get_work_req(c);
    req->data = data;
    csilk_io_loop_t* loop = _csilk_ctx_loop(c);
    _csilk_ctx_async_ref_incr(c);
    int rc = csilk_io_queue_work(loop, req, file_work_cb, static_after_work_cb);
    if (rc != 0) {
        CSILK_LOG_E("Static: failed to queue work: %d", rc);
        _csilk_ctx_async_ref_decr(c);
        free(data);
        req->data = c;
        csilk_status(c, CSILK_STATUS_INTERNAL_SERVER_ERROR);
        _csilk_send_response(c);
    }
}
