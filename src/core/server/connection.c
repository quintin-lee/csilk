/**
 * @file connection.c
 * @brief Connection module - thin wrapper for backward compatibility.
 *
 * This file exists to maintain the original translation unit.
 * Implementation has been split across:
 *   connection_pool.c  - pool management
 *   connection_state.c - state machine
 *   connection_timer.c - timer callbacks
 *   connection_close.c - close/destroy logic
 *   connection_io.c    - I/O callbacks
 */

#include "csilk/core/internal.h"
#include "../internal/srv_internal.h"
#include "../internal/srv_impl.h"
