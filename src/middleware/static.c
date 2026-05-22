/**
 * @file static.c
 * @brief Static file serving middleware implementation.
 * @license MIT
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <uv.h>

#include "gin.h"

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

/** @brief Static file serving middleware.
 * @param c The request context.
 * @param root_dir The root directory for static files. */
void gin_static(gin_ctx_t* c, const char* root_dir) {
  char full_path[1024];
  snprintf(full_path, sizeof(full_path), "%s/%s", root_dir, c->request.path);

  // Simple security: check for path traversal
  if (strstr(c->request.path, "..")) {
    gin_string(c, 403, "Forbidden");
    return;
  }

  uv_fs_t open_req;
  int fd = uv_fs_open(NULL, &open_req, full_path, O_RDONLY, 0, NULL);
  if (fd < 0) {
    gin_string(c, 404, "Not Found");
    return;
  }

  uv_fs_t stat_req;
  uv_fs_fstat(NULL, &stat_req, fd, NULL);
  size_t size = stat_req.statbuf.st_size;

  char* buffer = malloc(size);
  uv_fs_t read_req;
  uv_buf_t iov = uv_buf_init(buffer, size);
  uv_fs_read(NULL, &read_req, fd, &iov, 1, 0, NULL);

  gin_set_header(c, "Content-Type", get_mime_type(full_path));
  gin_status(c, 200);
  // For now, we assume c->response.body can be set directly to our managed
  // buffer
  c->response.body = buffer;
  c->response.body_is_managed = 1;

  uv_fs_close(NULL, &open_req, fd, NULL);
  uv_fs_req_cleanup(&open_req);
  uv_fs_req_cleanup(&stat_req);
  uv_fs_req_cleanup(&read_req);
}
