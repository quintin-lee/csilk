/**
 * @file flamegraph.h
 * @brief Real-time stack sampler and SVG flame graph generator.
 * @copyright MIT License
 */

#ifndef CSILK_FLAMEGRAPH_H
#define CSILK_FLAMEGRAPH_H

#include <stddef.h>
#include <sys/types.h>

/**
 * @brief Start continuous stack sampling.
 *
 * Spawns a background thread that captures call stacks every @p interval_us
 * microseconds and aggregates them. Stacks are captured via backtrace()
 * (glibc execinfo.h, Linux-only).
 *
 * @param interval_us Sampling interval in microseconds (10ms = 10000).
 * @return 0 on success, -1 if already running or allocation failure.
 */
int csilk_flamegraph_start(useconds_t interval_us);

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
 *
 * @return 1 if sampling, 0 otherwise.
 */
int csilk_flamegraph_is_running(void);

#endif /* CSILK_FLAMEGRAPH_H */
