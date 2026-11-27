#pragma once
/**
 * @file flamegraph.h
 * @brief Real-time stack sampler and SVG flame graph generator.
 *
 * Spawns a background thread that captures call stacks every interval
 * and aggregates them. Stacks are captured via backtrace().
 *
 * @version 1.3.0
 * @copyright MIT License
 */

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start continuous stack sampling.
 *
 * @param interval_us Sampling interval in microseconds (10ms = 10000).
 * @return 0 on success, -1 if already running or allocation failure.
 */
int csilk_flamegraph_start(unsigned int interval_us);

/**
 * @brief Stop sampling and generate an SVG flame graph.
 *
 * Joins the sampler thread, aggregates collected stacks, and renders a
 * Brendan Gregg-style SVG flame graph.
 *
 * @param[out] out_svg  Receives the heap-allocated SVG string (caller frees).
 * @param[out] out_len  Receives the byte length of the SVG.
 * @return 0 on success, -1 if sampling was not active.
 */
int csilk_flamegraph_stop(char** out_svg, size_t* out_len);

/**
 * @brief Check whether the flame graph profiler is currently sampling.
 * @return 1 if sampling, 0 otherwise.
 */
int csilk_flamegraph_is_running(void);

/** @brief Completion callback for async flame-graph sessions.
 *  Invoked on the sampler thread after auto-stop + SVG generation.
 *  @p out_svg is heap-allocated (caller frees); pass NULL to skip SVG. */
typedef void (*csilk_flamegraph_done_cb)(char* out_svg, size_t out_len, void* user_data);

/**
 * @brief Start sampling with an auto-stop duration and completion callback.
 *
 * The sampler thread runs for @p duration_ms milliseconds, then automatically
 * stops and invokes @p cb.  Only one profiler session may run at a time;
 * returns -1 if already active or on allocation failure.
 *
 * @param interval_us  Sampling interval in microseconds.
 * @param duration_ms  Auto-stop duration in milliseconds.
 * @param cb           Completion callback (may be NULL for fire-and-forget).
 * @param user_data    Opaque pointer passed to @p cb.
 * @return 0 on success, -1 if already running or allocation failure.
 */
int csilk_flamegraph_start_async(useconds_t               interval_us,
                                 uint64_t                 duration_ms,
                                 csilk_flamegraph_done_cb cb,
                                 void*                    user_data);

#ifdef __cplusplus
}
#endif
