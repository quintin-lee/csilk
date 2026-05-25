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

#include "context_internal.h"
#include "csilk.h"
#include "csilk_internal.h"

/** @brief Internal helper to get MIME type from file path.
 * @param path The file path.
 * @return The MIME type string. */
static const char* get_mime_type(const char* path) {
  const char* ext = strrchr(path, '.');
  if (!ext) return "application/octet-stream";
  if (strcmp(ext, ".html") == 0) return "text/html";
  if (strcmp(ext, ".css") == 0) return "text/css";
  if (strcmp(ext, ".js") == 0) return "application/javascript";
  return "application/octet-stream";
}

/** @brief libuv work callback: read static file from disk (offloaded from event
 * loop). @param req libuv work request. */
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

  const char* relative_path = csilk_get_path(c);
  if (prefix && strncmp(relative_path, prefix, strlen(prefix)) == 0) {
    relative_path += strlen(prefix);
    if (*relative_path == '/') relative_path++;
  }

  snprintf(full_path, sizeof(full_path), "%s/%s", root_dir, relative_path);

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

/** @brief libuv after-work callback: send the static file response. @param req
 * libuv work request. @param status Work status. */
static void static_after_work_cb(uv_work_t* req, int status) {
  (void)status;
  csilk_ctx_t* c = (csilk_ctx_t*)req->data;
  if (c->_internal_client) {
    _csilk_send_response(c);
  }
}

/** @brief Serve static files from a local directory (async, offloaded). */
void csilk_static(csilk_ctx_t* c, const char* root_dir) {
  c->is_async = 1;
  c->work_req.data = c;
  csilk_set(c, "static_root", (void*)root_dir);
  uv_queue_work(uv_default_loop(), &c->work_req, static_work_cb,
                static_after_work_cb);
}
