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

#include <execinfo.h>
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
#define FG_MAX_DEPTH 64
/* Default sampling duration in seconds */
#define FG_DEFAULT_DURATION 5
/* Sampling interval in microseconds (10ms = 100 samples/sec) */
#define FG_SAMPLE_INTERVAL_US 10000
/* Maximum number of unique stacks to track in the hash map */
#define FG_MAX_STACKS 4096
/* Maximum function name length */
#define FG_MAX_NAME 256
/* Flame graph SVG dimensions */
#define FG_FRAME_HEIGHT 20
#define FG_TITLE_HEIGHT 24
#define FG_FONT_SIZE 12
#define FG_MIN_WIDTH 0.5

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

/* djb2 hash for collapsed stack strings */
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

static void*
fg_sampler_thread(void* arg)
{
	fg_profiler_t* p = (fg_profiler_t*)arg;
	char collapsed[FG_MAX_DEPTH * FG_MAX_NAME];
	void* buffer[FG_MAX_DEPTH];

	while (atomic_load(&p->running)) {
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

		usleep(p->interval_us);
	}

	return nullptr;
}

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
	for (int i = 0; i < p->entry_count; i++) {
		free(p->entries[i].frames.stack[0]);
	}
	free(p->entries);
	free(p->hash_table);
	free(p);

	return 0;
}

int
csilk_flamegraph_is_running(void)
{
	return g_profiler && atomic_load(&g_profiler->running);
}
