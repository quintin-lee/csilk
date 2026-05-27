/**
 * @file workflow_wal.c
 * @brief Binary Write-Ahead Log implementation for AI workflows.
 */

#include "csilk/app/workflow_wal.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int _wf_wal_append(const char* wal_path, csilk_wf_event_type_t type,
                   const void* payload, size_t len) {
  if (!wal_path) return -1;

  int fd = open(wal_path, O_WRONLY | O_APPEND | O_CREAT, 0644);
  if (fd < 0) return -1;

  csilk_wf_wal_header_t header;
  header.magic = CSILK_WF_MAGIC;
  header.type = (uint8_t)type;
  header.timestamp = (uint32_t)time(NULL);
  header.payload_len = (uint32_t)len;

  if (write(fd, &header, sizeof(header)) != sizeof(header)) {
    close(fd);
    return -1;
  }

  if (len > 0 && payload) {
    if (write(fd, payload, len) != (ssize_t)len) {
      close(fd);
      return -1;
    }
  }

  fdatasync(fd);
  close(fd);
  return 0;
}
