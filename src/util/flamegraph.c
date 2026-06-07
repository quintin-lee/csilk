/**
 * @file flamegraph.c
 * @brief Real-time stack sampler and SVG flame graph generator.
 *
 * Captures per-thread call stacks at a configurable interval using
 * backtrace() (glibc execinfo.h) and renders Brendan Gregg-style
 * SVG flame graphs directly from the collected samples.
 *
 * Architecture:
 *   - A background sampler thread captures stacks every 10ms.
 *   - Collapsed stacks are aggregated in a hash map (func;func;func → count).
 *   - On request, the accumulated data is converted into an SVG flame graph.
 *   - Sampling stops after a configurable duration and the SVG is returned.
 *
 * @copyright MIT License
 */

#if defined(__GLIBC__)
#include <execinfo.h>
#endif
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "csilk/csilk.h"

/* Maximum call stack depth to capture */
static constexpr int FG_MAX_DEPTH = 64;
/* Default sampling duration in seconds */
static constexpr int FG_DEFAULT_DURATION = 5;
/* Sampling interval in microseconds (10ms = 100 samples/sec) */
static constexpr int FG_SAMPLE_INTERVAL_US = 10000;
/* Maximum number of unique stacks to track in the hash map */
static constexpr int FG_MAX_STACKS = 4096;
/* Maximum function name length */
static constexpr int FG_MAX_NAME = 256;
/* Flame graph SVG dimensions */
static constexpr int FG_FRAME_HEIGHT = 20;
static constexpr int FG_TITLE_HEIGHT = 24;
static constexpr int FG_FONT_SIZE = 12;
static constexpr double FG_MIN_WIDTH = 0.5;

typedef struct {
	char stack[FG_MAX_DEPTH][FG_MAX_NAME]; /**< Function names from bottom to top. */
	int depth;			       /**< Number of frames in this stack. */
} fg_stack_t;

typedef struct {
	int count; /**< Number of times this stack was sampled. */
	int next;  /**< Index of next entry in the same hash bucket (-1 = end). */
	fg_stack_t frames;
} fg_stack_entry_t;

typedef struct {
	atomic_int running;	   /**< Non-zero while sampler is active. */
	atomic_int sample_count;   /**< Total samples captured. */
	fg_stack_entry_t* entries; /**< Aggregated stack entries. */
	int* hash_table;	   /**< Hash bucket heads (-1 = empty). */
	int entry_count;	   /**< Number of entries used. */
	pthread_t sampler_thread;  /**< Background profiler thread. */
	pthread_mutex_t mutex;	   /**< Guards entries and hash_table. */
	useconds_t interval_us;	   /**< Sampling interval. */
	csilk_ctx_t* notify_ctx;   /**< Admin context for completion notification. */
} fg_profiler_t;

static fg_profiler_t* g_profiler = nullptr;

/** @brief djb2 hash function for collapsed stack strings.
 *
 * The classic djb2 algorithm (Daniel J. Bernstein) — simple, fast, and
 * well-distributed for ASCII strings.  Used to index into the stack hash
 * table for deduplication. */
static unsigned int
fg_hash(const char* str)
{
	unsigned int h = 5381;
	while (*str) {
		h = ((h << 5) + h) + (unsigned char)*str;
		str++;
	}
	return h;
}

/** @brief Insert or increment a collapsed stack entry in the hash map.
 *
 * Looks up @p collapsed in the hash table.  If found, increments the hit
 * count; otherwise allocates a new entry at the end of the entries array.
 * Caller must hold @c p->mutex.
 *
 * @param p         The profiler instance.
 * @param collapsed Semicolon-separated function names (collapsed format).
 * @param depth     Number of frames in this stack.
 * @return          Entry index on success, -1 if the table is full. */
static int
fg_entry_insert(fg_profiler_t* p, const char* collapsed, int depth)
{
	unsigned int h = fg_hash(collapsed) % FG_MAX_STACKS;
	int idx = p->hash_table[h];
	while (idx >= 0) {
		if (strcmp((const char*)p->entries[idx].frames.stack[0], collapsed) == 0) {
			p->entries[idx].count++;
			return idx;
		}
		idx = p->entries[idx].next;
	}

	if (p->entry_count >= FG_MAX_STACKS) {
		return -1;
	}
	idx = p->entry_count++;
	p->entries[idx].count = 1;
	p->entries[idx].next = p->hash_table[h];
	p->entries[idx].frames.depth = depth;
	p->hash_table[h] = idx;
	return idx;
}

/** @brief Background sampler thread entry point.
 *
 * Loops at the configured interval while @c running is set.  On each
 * iteration captures the calling thread's stack via backtrace() (glibc),
 * converts frames to semicolon-separated collapsed format, and inserts
 * the result into the hash map.  On non-glibc platforms this is a no-op
 * loop (sampling is not supported).
 *
 * @param arg Pointer to the fg_profiler_t instance. */
static void*
fg_sampler_thread(void* arg)
{
	fg_profiler_t* p = (fg_profiler_t*)arg;
	char collapsed[FG_MAX_DEPTH * FG_MAX_NAME];
	void* buffer[FG_MAX_DEPTH];

	while (atomic_load(&p->running)) {
#if defined(__GLIBC__)
		int nptrs = backtrace(buffer, FG_MAX_DEPTH);
		if (nptrs > 1) {
			char** symbols = backtrace_symbols(buffer, nptrs);
			if (symbols) {
				collapsed[0] = '\0';
				for (int i = nptrs - 1; i >= 0; i--) {
					char* s = symbols[i];
					char* paren = strchr(s, '(');
					char name[FG_MAX_NAME];
					if (paren) {
						size_t name_len = paren - s;
						if (name_len >= FG_MAX_NAME) {
							name_len = FG_MAX_NAME - 1;
						}
						memcpy(name, s, name_len);
						name[name_len] = '\0';
					} else {
						snprintf(name, sizeof(name), "%s", s);
					}
					if (i < nptrs - 1) {
						strncat(collapsed,
							";",
							sizeof(collapsed) - strlen(collapsed) - 1);
					}
					strncat(collapsed,
						name,
						sizeof(collapsed) - strlen(collapsed) - 1);
				}
				free(symbols);

				pthread_mutex_lock(&p->mutex);
				fg_entry_insert(p, collapsed, nptrs);
				atomic_fetch_add(&p->sample_count, 1);
				pthread_mutex_unlock(&p->mutex);
			}
		}
#else
		(void)buffer;
		(void)collapsed;
#endif
		usleep(p->interval_us);
	}

	return nullptr;
}

/** @brief Render accumulated samples as a Brendan Gregg-style SVG flame graph.
 *
 * Builds an inline SVG document with proportional-width rectangles coloured
 * by a 12-colour rainbow palette.  Each stack frame gets a <rect> with a
 * <title> tooltip and horizontally truncated label text.  The output is
 * returned as a heap-allocated string the caller must free.
 *
 * Uses a large stack buffer for the SVG skeleton plus a heap buffer for
 * per-frame <g> elements to avoid repeated reallocation.  On allocation
 * failure the function still produces a valid (if empty) SVG snippet.
 *
 * @param p       The profiler instance (must have been stopped).
 * @param out_svg [out] Receives a heap-allocated SVG string.
 * @param out_len [out] Receives the length of the SVG string. */
static void
fg_generate_svg(fg_profiler_t* p, char** out_svg, size_t* out_len)
{
	char buf[FG_MAX_STACKS * 512];
	size_t pos = 0;
	int max_count = 0;

	for (int i = 0; i < p->entry_count; i++) {
		if (p->entries[i].count > max_count) {
			max_count = p->entries[i].count;
		}
	}
	if (max_count == 0) {
		max_count = 1;
	}

	double total_width = 1200.0;
	double width_per_sample = total_width / (double)atomic_load(&p->sample_count);

	pos += (size_t)snprintf(
	    buf + pos,
	    sizeof(buf) - pos,
	    "<svg xmlns='http://www.w3.org/2000/svg' width='1200' height='auto' "
	    "font-family='monospace' font-size='%d'>\n"
	    "  <rect width='100%%' height='100%%' fill='#1e293b'/>\n"
	    "  <text x='10' y='%d' fill='#e2e8f0' font-weight='bold'>Flame Graph — "
	    "%d samples, %d unique stacks</text>\n",
	    FG_FONT_SIZE,
	    FG_TITLE_HEIGHT - 8,
	    atomic_load(&p->sample_count),
	    p->entry_count);

	double y_offset = FG_TITLE_HEIGHT + 4;
	char* frame_buf = malloc(FG_MAX_STACKS * 2048);
	size_t frame_pos = 0;

	if (!frame_buf) {
		goto done;
	}

	for (int i = 0; i < p->entry_count; i++) {
		double width = (double)p->entries[i].count * width_per_sample;
		if (width < FG_MIN_WIDTH) {
			width = FG_MIN_WIDTH;
		}

		int rainbow_index = (i * 7 + 3) % 12;
		const char* colors[12] = {
		    "#f97316",
		    "#eab308",
		    "#84cc16",
		    "#22c55e",
		    "#14b8a6",
		    "#06b6d4",
		    "#3b82f6",
		    "#6366f1",
		    "#a855f7",
		    "#d946ef",
		    "#ec4899",
		    "#f43f5e",
		};

		double x = 0;
		fg_stack_t* s = &p->entries[i].frames;
		for (int j = 0; j < s->depth; j++) {
			double frame_y = y_offset + (s->depth - 1 - j) * FG_FRAME_HEIGHT;
			frame_pos += (size_t)snprintf(
			    frame_buf + frame_pos,
			    2048,
			    "  <g><rect x='%.1f' y='%.1f' width='%.1f' height='%d' "
			    "fill='%s' rx='2'><title>%s (%d samples)</title></rect>"
			    "<text x='%.1f' y='%.1f' fill='white' font-size='%d' clip-path='none'>"
			    "%s</text></g>\n",
			    x,
			    frame_y,
			    width,
			    FG_FRAME_HEIGHT - 1,
			    colors[rainbow_index],
			    s->stack[j],
			    p->entries[i].count,
			    x + 3,
			    frame_y + 14,
			    FG_FONT_SIZE - 1,
			    s->stack[j]);
		}
	}

	double total_height = y_offset + FG_MAX_DEPTH * FG_FRAME_HEIGHT + 10;
	pos += (size_t)snprintf(
	    buf + pos, sizeof(buf) - pos, "<svg viewBox='0 0 1200 %.0f'>\n", total_height);
	memcpy(buf + pos, frame_buf, frame_pos);
	pos += frame_pos;
	free(frame_buf);

done:
	pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "</svg></svg>\n");

	*out_svg = strdup(buf);
	*out_len = pos;
}

/** @brief Start a background stack-sampling profiler.
 *
 * Allocates a profiler instance, initialises the hash table, and spawns
 * a sampler thread.  Only one profiler can be active at a time (returns
 * -1 if one is already running).
 *
 * @param interval_us Sampling interval in microseconds (e.g. 10000 for
 *                    100 samples/sec).  Must be > 0.
 * @return 0 on success, -1 on allocation or thread-creation failure. */
int
csilk_flamegraph_start(useconds_t interval_us)
{
	if (g_profiler) {
		return -1;
	}

	fg_profiler_t* p = calloc(1, sizeof(fg_profiler_t));
	if (!p) {
		return -1;
	}

	p->entries = calloc(FG_MAX_STACKS, sizeof(fg_stack_entry_t));
	p->hash_table = malloc(FG_MAX_STACKS * sizeof(int));
	if (!p->entries || !p->hash_table) {
		free(p->entries);
		free(p->hash_table);
		free(p);
		return -1;
	}
	for (int i = 0; i < FG_MAX_STACKS; i++) {
		p->hash_table[i] = -1;
	}
	p->interval_us = interval_us;
	pthread_mutex_init(&p->mutex, nullptr);
	atomic_store(&p->running, 1);
	atomic_store(&p->sample_count, 0);

	if (pthread_create(&p->sampler_thread, nullptr, fg_sampler_thread, p) != 0) {
		pthread_mutex_destroy(&p->mutex);
		free(p->entries);
		free(p->hash_table);
		free(p);
		return -1;
	}

	g_profiler = p;
	return 0;
}

/** @brief Stop the profiler and generate the flame graph SVG.
 *
 * Signals the sampler thread to exit, joins it, generates the SVG, and
 * frees all profiler resources.  The returned SVG is heap-allocated;
 * the caller takes ownership and must free it with free().
 *
 * @param out_svg [out] Receives the SVG string (caller must free).
 * @param out_len [out] Receives the SVG byte length.
 * @return 0 on success, -1 if no profiler is active. */
int
csilk_flamegraph_stop(char** out_svg, size_t* out_len)
{
	if (!g_profiler) {
		return -1;
	}
	fg_profiler_t* p = g_profiler;
	atomic_store(&p->running, 0);
	pthread_join(p->sampler_thread, nullptr);
	g_profiler = nullptr;

	fg_generate_svg(p, out_svg, out_len);

	pthread_mutex_destroy(&p->mutex);
	/* entries[i].frames.stack is an inline member array (not heap-allocated),
	 * so only the entries array itself needs to be freed. */
	free(p->entries);
	free(p->hash_table);
	free(p);

	return 0;
}

/** @brief Check whether a flame-graph profiler is currently running.
 * @return 1 if the profiler is active, 0 otherwise. */
int
csilk_flamegraph_is_running(void)
{
	return g_profiler && atomic_load(&g_profiler->running);
}
