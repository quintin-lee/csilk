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
static void static_work_cb(uv_work_t* req) {
  csilk_ctx_t* c = (csilk_ctx_t*)req->data;
  const char* root_dir = (const char*)csilk_get(c, "static_root");
  const char* prefix = (const char*)csilk_get(c, "static_prefix");
  char full_path[PATH_MAX];
  char resolved_root[PATH_MAX];
  char resolved_file[PATH_MAX];

  // Resolve root_dir to absolute path
  if (realpath(root_dir, resolved_root) == NULL) {
    csilk_string(c, CSILK_STATUS_INTERNAL_SERVER_ERROR,
                 "Internal Server Error");
    ;
    return;
  }

  const char* relative_path = csilk_get_path(c);
  if (prefix && strncmp(relative_path, prefix, strlen(prefix)) == 0) {
    relative_path += strlen(prefix);
    if (*relative_path == '/') relative_path++;
  }

  snprintf(full_path, sizeof(full_path), "%s/%s", root_dir, relative_path);

  // Resolve full_path to absolute path
  if (realpath(full_path, resolved_file) == NULL) {
    csilk_string(c, CSILK_STATUS_NOT_FOUND, "Not Found");
    ;
    return;
  }

  // Security: check if resolved_file starts with resolved_root
  if (strncmp(resolved_root, resolved_file, strlen(resolved_root)) != 0) {
    csilk_string(c, CSILK_STATUS_FORBIDDEN, "Forbidden");
    return;
  }

  uv_fs_t open_req;
  int fd = uv_fs_open(NULL, &open_req, resolved_file, O_RDONLY, 0, NULL);
  if (fd < 0) {
    csilk_string(c, CSILK_STATUS_NOT_FOUND, "Not Found");
    ;
    return;
  }

  uv_fs_t stat_req;
  uv_fs_fstat(NULL, &stat_req, fd, NULL);
  size_t size = stat_req.statbuf.st_size;

  char* buffer = malloc(size + 1);
  if (!buffer) {
    uv_fs_close(NULL, &open_req, fd, NULL);
    uv_fs_req_cleanup(&open_req);
    uv_fs_req_cleanup(&stat_req);
    csilk_string(c, CSILK_STATUS_INTERNAL_SERVER_ERROR,
                 "Internal Server Error");
    ;
    return;
  }

  uv_fs_t read_req;
  uv_buf_t iov = uv_buf_init(buffer, size);
  int nread = uv_fs_read(NULL, &read_req, fd, &iov, 1, 0, NULL);
  if (nread < 0) {
    free(buffer);
    uv_fs_close(NULL, &open_req, fd, NULL);
    uv_fs_req_cleanup(&open_req);
    uv_fs_req_cleanup(&stat_req);
    uv_fs_req_cleanup(&read_req);
    csilk_string(c, CSILK_STATUS_INTERNAL_SERVER_ERROR,
                 "Internal Server Error");
    ;
    return;
  }
  buffer[size] = '\0';

  csilk_set_header(c, "Content-Type", get_mime_type(resolved_file));
  csilk_status(c, CSILK_STATUS_OK);

  if (c->response.body && c->response.body_is_managed) {
    free((void*)c->response.body);
  }
  c->response.body = buffer;
  c->response.body_is_managed = 1;
  c->response.body_len = size;

  uv_fs_close(NULL, &open_req, fd, NULL);
  uv_fs_req_cleanup(&open_req);
  uv_fs_req_cleanup(&stat_req);
  uv_fs_req_cleanup(&read_req);
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
