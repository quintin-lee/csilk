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
#include <uv.h>

#include "csilk/core/context_internal.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"

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
static const char* get_mime_type(const char* path) {
  const char* ext = strrchr(path, '.');
  if (!ext) return "application/octet-stream";
  if (strcmp(ext, ".html") == 0) return "text/html";
  if (strcmp(ext, ".css") == 0) return "text/css";
  if (strcmp(ext, ".js") == 0) return "application/javascript";
  return "application/octet-stream";
}

/**
 * @brief Setup a file response with optional 206 Partial Content for range
 *        requests.
 *
 * Examines the incoming "Range" header. If absent or malformed, the entire
 * file is served with a 200 OK status. If a valid "bytes=" range is present,
 * a 206 Partial Content response is configured with the appropriate
 * Content-Range header.
 *
 * In both cases, the file descriptor, offset, and size are stored on the
 * context for subsequent async sending.
 *
 * @param c         The request context.
 * @param fd        Open file descriptor for the requested file.
 * @param size      Total file size in bytes.
 * @param mime_type The MIME type string for Content-Type header.
 *
 * @note On range errors (non-numeric range, start > end, or unsatisfiable),
 *       a 416 Range Not Satisfiable status is set and the FD is closed.
 */
static void set_range_response(csilk_ctx_t* c, int fd, size_t size,
                               const char* mime_type) {
  const char* range_hdr = csilk_get_header(c, "Range");
  if (!range_hdr || strncmp(range_hdr, "bytes=", 6) != 0) {
    csilk_set_header(c, "Content-Type", mime_type);
    csilk_set_header(c, "Accept-Ranges", "bytes");
    csilk_status(c, CSILK_STATUS_OK);
    c->file_fd = fd;
    c->file_offset = 0;
    c->file_size = size;
    return;
  }

  const char* range_val = range_hdr + 6;
  char* dash = strchr(range_val, '-');
  if (!dash) {
    csilk_status(c, CSILK_STATUS_RANGE_NOT_SATISFIABLE);
    uv_fs_t close_req;
    uv_fs_close(NULL, &close_req, fd, NULL);
    return;
  }

  long long range_start = 0, range_end = (long long)size - 1;
  *dash = '\0';
  if (range_val[0] != '\0') {
    char* endptr = NULL;
    range_start = strtoll(range_val, &endptr, 10);
    if (endptr == range_val || range_start < 0) {
      *dash = '-';
      csilk_status(c, CSILK_STATUS_RANGE_NOT_SATISFIABLE);
      uv_fs_t close_req;
      uv_fs_close(NULL, &close_req, fd, NULL);
      return;
    }
  }
  if (*(dash + 1) != '\0') {
    char* endptr = NULL;
    range_end = strtoll(dash + 1, &endptr, 10);
    if (endptr == dash + 1 || range_end >= (long long)size)
      range_end = (long long)size - 1;
  }
  *dash = '-';

  if (range_start > range_end || range_start >= (long long)size) {
    csilk_status(c, CSILK_STATUS_RANGE_NOT_SATISFIABLE);
    uv_fs_t close_req;
    uv_fs_close(NULL, &close_req, fd, NULL);
    return;
  }
  if (range_end >= (long long)size) range_end = (long long)size - 1;

  size_t range_len = (size_t)(range_end - range_start + 1);

  char content_range[128];
  snprintf(content_range, sizeof(content_range), "bytes %lld-%lld/%zu",
           range_start, range_end, size);
  csilk_set_header(c, "Content-Range", content_range);
  csilk_set_header(c, "Content-Type", mime_type);
  csilk_set_header(c, "Accept-Ranges", "bytes");
  csilk_status(c, CSILK_STATUS_PARTIAL_CONTENT);

  c->file_fd = fd;
  c->file_offset = (size_t)range_start;
  c->file_size = range_len;
}

/**
 * @brief libuv work callback: resolve and open the requested static file.
 *
 * Runs on the libuv thread pool. Resolves the requested path relative to
 * the configured root directory, performs a path traversal check (ensuring
 * the resolved path stays within the root), opens the file, stats it,
 * determines the MIME type, and configures the response (including any
 * range handling). The actual file content is sent later in
 * static_after_work_cb via _csilk_send_response.
 *
 * @param req  libuv work request. req->data points to the csilk_ctx_t.
 *             The context must have "static_root" and "static_prefix"
 *             set via csilk_set().
 *
 * @note Path traversal protection uses realpath() to canonicalize both
 *       root and requested paths, then verifies the requested path starts
 *       with the root prefix.
 */
static void static_work_cb(uv_work_t* req) {
  csilk_ctx_t* c = (csilk_ctx_t*)req->data;
  const char* root_dir = (const char*)csilk_get(c, "static_root");
  const char* prefix = (const char*)csilk_get(c, "static_prefix");
  char full_path[PATH_MAX];
  char resolved_root[PATH_MAX];
  char resolved_file[PATH_MAX];

  if (realpath(root_dir, resolved_root) == NULL) {
    csilk_string(c, CSILK_STATUS_INTERNAL_SERVER_ERROR,
                 "Internal Server Error");
    return;
  }

  /* Strip the URL prefix (e.g. "/static") from the request path so the
     remaining path is resolved relative to root_dir. */
  const char* relative_path = csilk_get_path(c);
  if (prefix && strncmp(relative_path, prefix, strlen(prefix)) == 0) {
    relative_path += strlen(prefix);
    if (*relative_path == '/') relative_path++;
  }

  snprintf(full_path, sizeof(full_path), "%s/%s", root_dir, relative_path);

  /* Path traversal protection: realpath() resolves all symlinks and ".."
     segments. If the resolved file path does not start with the resolved
     root directory prefix, the request is outside the allowed tree. */
  if (realpath(full_path, resolved_file) == NULL) {
    csilk_string(c, CSILK_STATUS_NOT_FOUND, "Not Found");
    return;
  }

  if (strncmp(resolved_root, resolved_file, strlen(resolved_root)) != 0) {
    csilk_string(c, CSILK_STATUS_FORBIDDEN, "Forbidden");
    return;
  }

  uv_fs_t open_req;
  int fd = uv_fs_open(NULL, &open_req, resolved_file, O_RDONLY, 0, NULL);
  uv_fs_req_cleanup(&open_req);
  if (fd < 0) {
    csilk_string(c, CSILK_STATUS_NOT_FOUND, "Not Found");
    return;
  }

  uv_fs_t stat_req;
  uv_fs_fstat(NULL, &stat_req, fd, NULL);
  size_t size = stat_req.statbuf.st_size;
  uv_fs_req_cleanup(&stat_req);

  const char* mime_type = get_mime_type(resolved_file);
  set_range_response(c, fd, size, mime_type);
}

/**
 * @brief libuv after-work callback: send the static file response.
 *
 * Runs on the event loop after static_work_cb completes. If the client
 * connection is still alive (_internal_client is non-NULL), it triggers
 * _csilk_send_response() to transmit the file (using sendfile or chunked
 * I/O depending on the platform).
 *
 * @param req     libuv work request. req->data points to the csilk_ctx_t.
 * @param status  Work status — 0 on success, negative on cancellation.
 */
static void static_after_work_cb(uv_work_t* req, int status) {
  (void)status;
  csilk_ctx_t* c = (csilk_ctx_t*)req->data;
  if (c->_internal_client) {
    _csilk_send_response(c);
  }
}

/**
 * @brief Serve static files from a local directory (async, offloaded).
 *
 * Offloads file resolution, path traversal checks, and file open/stat
 * operations to the libuv thread pool. The response is sent via the
 * normal _csilk_send_response() path, which sends the file using
 * platform-specific zero-copy mechanisms (e.g. sendfile on Linux).
 *
 * The URL prefix is automatically stripped from the request path before
 * resolving against root_dir — the "static_prefix" must be set via
 * csilk_set(c, "static_prefix", prefix_string) before calling this
 * function.
 *
 * @param c        The request context.
 * @param root_dir Absolute or relative path to the static file root
 *                 directory. Must not be NULL.
 *
 * @note The prefix is configured separately via csilk_set(c, "static_prefix",
 *       ...) NOT via this function's parameters.
 * @warning Path traversal attacks are mitigated via realpath() comparison.
 *          Ensure root_dir does not contain symlinks that could be
 *          exploited.
 */
void csilk_static(csilk_ctx_t* c, const char* root_dir) {
  c->is_async = 1;
  c->work_req.data = c;
  csilk_set(c, "static_root", (void*)root_dir);
  uv_queue_work(uv_default_loop(), &c->work_req, static_work_cb,
                static_after_work_cb);
}
