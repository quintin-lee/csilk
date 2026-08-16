# HTTP Module Refactoring Design

**Date**: 2026-08-16  
**Status**: Draft

## Overview

Split `h2.c` (696 lines) and `http1_response.c` (567 lines) into focused modules.

## Target Structure

### h2.c → 3 files

```
src/core/http/
├── h2_callbacks.c    # nghttp2 callbacks (~250 lines)
│   - on_begin_headers_callback
│   - on_header_callback
│   - on_frame_recv_callback
│   - on_data_chunk_recv_callback
│   - body_read_callback
│   - on_stream_close_callback
│   - send_callback
├── h2_session.c      # Session management (~200 lines)
│   - csilk_h2_init_session
│   - csilk_h2_process_data
│   - csilk_h2_get_or_create_stream
│   - csilk_h2_free_streams
└── h2_response.c     # Response handling (~250 lines)
    - csilk_h2_send_response
    - csilk_h2_submit_push
```

### http1_response.c → 3 files

```
src/core/http/
├── http1_serialize.c   # Response serialization (~200 lines)
│   - serialize_status_line
│   - append_custom_headers
│   - _csilk_send_response
├── http1_write.c       # Write pipeline (~200 lines)
│   - on_write
│   - on_sendfile_complete
│   - csilk_client_write
│   - _csilk_send_data
│   - _csilk_send_data_owned
│   - _csilk_client_get_write_queue_size
│   - _csilk_check_and_trigger_drain
└── http1_pipeline.c    # Post-response logic (~150 lines)
    - get_status_text
    - _csilk_handle_post_response
```

## Changes Required

### New Files
- `src/core/http/h2_callbacks.c`
- `src/core/http/h2_session.c`
- `src/core/http/h2_response.c`
- `src/core/http/http1_serialize.c`
- `src/core/http/http1_write.c`
- `src/core/http/http1_pipeline.c`

### Modified Files
- `src/core/http/h2.c` → reduce to thin wrapper
- `src/core/http/http1_response.c` → reduce to thin wrapper
- `src/core/internal/srv_impl.h` → update declarations
- `cmake/sources.cmake` → add new source files

## Non-Goals
- No functional changes
- No API changes
- No performance changes
