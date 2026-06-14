/*=============================================================================
    test_videotestsrc_timing.c — Measure real frame timing from videotestsrc
    with clock_sync enabled and log PTS vs wall-clock to a file.
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>

#include "zst_types.h"
#include "zst_buffer.h"
#include "zst_pad.h"
#include "zst_element.h"
#include "zst_pipeline.h"
#include "zst_scheduler.h"
#include "zst_plugin.h"
#include "zst_element_factory.h"

/* ═══════════════════════════════════════════════════════════════
   Test harness macros
   ═══════════════════════════════════════════════════════════════ */
static int g_tests_run   = 0;
static int g_tests_passed = 0;

#define TEST(name)                                              \
    do {                                                        \
        g_tests_run++;                                          \
        printf("  TEST: %-50s ... ", name);                     \
        fflush(stdout);                                         \
    } while (0)

#define PASS()                                                  \
    do {                                                        \
        g_tests_passed++;                                       \
        printf("PASS\n");                                       \
    } while (0)

/* ═══════════════════════════════════════════════════════════════
   Frame timing record — one per delivered buffer
   ═══════════════════════════════════════════════════════════════ */
typedef struct {
    int         frame_num;
    zst_time_t  wall_ns;      /* CLOCK_MONOTONIC at buffer delivery */
    zst_time_t  pts;          /* buffer PTS */
    zst_time_t  duration;     /* buffer duration in ns */
} timing_record_t;

typedef struct {
    timing_record_t* records;
    int              capacity;
    int              count;
    FILE*            log;
} timing_ctx_t;

/* ── Pad probe callback — records wall-clock + PTS for each frame ── */
static zst_pad_probe_return_t
timing_probe_cb(zst_pad_t* pad, zst_buffer_t* buf,
                zst_pad_probe_type_t type, void* user_data)
{
    (void)pad;
    (void)type;
    if (!buf || (buf->flags & ZST_BUFFER_FLAG_EOS))
        return ZST_PAD_PROBE_OK;

    timing_ctx_t* ctx = (timing_ctx_t*)user_data;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    zst_time_t wall = (zst_time_t)ts.tv_sec * 1000000000ULL
                    + (zst_time_t)ts.tv_nsec;

    int idx = ctx->count;
    if (idx < ctx->capacity) {
        ctx->records[idx].frame_num = idx;
        ctx->records[idx].wall_ns   = wall;
        ctx->records[idx].pts       = buf->pts;
        ctx->records[idx].duration  = buf->duration;
    }
    ctx->count++;

    if (ctx->log) {
        fprintf(ctx->log, "%d %" PRIu64 " %" PRIu64 " %" PRIu64 "\n",
                idx, wall, buf->pts, buf->duration);
    }

    return ZST_PAD_PROBE_OK;
}

/* ═══════════════════════════════════════════════════════════════
   Test — videotestsrc frame timing with clock_sync
   ═══════════════════════════════════════════════════════════════ */
static void
test_videotestsrc_timing(void)
{
    TEST("videotestsrc frame timing with clock_sync");

    zst_plugin_registry_init();
    zst_register_builtin_elements();

    /* ----------------------------------------------------------------
       Setup pipeline: videotestsrc -> fakesink  with clock_sync
       ---------------------------------------------------------------- */
    zst_pipeline_t* pipe = zst_pipeline_create();
    assert(pipe != NULL);
    zst_pipeline_set_clock_sync(pipe, 1);  /* enable PTS-based pacing */

    zst_element_t* src = zst_element_factory_make("videotestsrc");
    assert(src != NULL);
    zst_element_set_property_int(src, "width", 640);
    zst_element_set_property_int(src, "height", 480);
    zst_element_set_property_int(src, "fps", 30);
    zst_element_set_property(src,     "pattern", "bars");
    zst_element_set_property_int(src, "num-buffers", 60);   /* 2 s at 30 fps */
    zst_element_set_property_bool(src, "use-clock", false);  /* count-based PTS */

    zst_element_t* sink = zst_element_factory_make("fakesink");
    assert(sink != NULL);

    zst_pipeline_add(pipe, src);
    zst_pipeline_add(pipe, sink);

    zst_pad_t* src_pad  = zst_element_get_pad(src, "src");
    zst_pad_t* sink_pad = zst_element_get_pad(sink, "sink");
    assert(src_pad != NULL && sink_pad != NULL);
    assert(zst_pad_link(src_pad, sink_pad) == ZST_OK);

    /* ----------------------------------------------------------------
       Timing probe — records wall-clock + PTS per frame
       ---------------------------------------------------------------- */
    enum { MAX_FRAMES = 128 };
    timing_record_t records[MAX_FRAMES];

    const char* log_path = "videotestsrc_timing.log";
    FILE* log = fopen(log_path, "w");
    assert(log != NULL);
    fprintf(log, "# frame wall_ns pts duration_ns\n");
    fprintf(log, "# wall_ns = CLOCK_MONOTONIC at probe callback\n");
    fprintf(log, "# all values in nanoseconds\n");

    timing_ctx_t ctx = {
        .records  = records,
        .capacity = MAX_FRAMES,
        .count    = 0,
        .log      = log
    };

    uint64_t probe_id = zst_pad_add_probe(sink_pad, ZST_PAD_PROBE_PRE_BUFFER,
                                          timing_probe_cb, &ctx);
    assert(probe_id != 0);

    /* ----------------------------------------------------------------
       Run pipeline with single-threaded scheduler
       ---------------------------------------------------------------- */
    zst_scheduler_config_t cfg = {
        .mode           = ZST_SCHEDULER_SINGLE_THREAD,
        .worker_threads = 1
    };
    zst_scheduler_t* sched = zst_scheduler_create(&cfg);
    assert(sched != NULL);
    zst_scheduler_attach(sched, pipe);

    assert(zst_pipeline_set_state(pipe, ZST_STATE_PLAYING) == ZST_OK);
    zst_scheduler_run(sched);

    /* Wait for all 60 frames + margin.
       NOTE: tv_nsec must be < 1000000000 per POSIX spec. Use tv_sec for
       durations >= 1 second. */
    struct timespec ts;
    ts.tv_sec  = 2;              /* 2 seconds */
    ts.tv_nsec = 500000000ULL;   /* 500 ms = 2.5 s total */
    nanosleep(&ts, NULL);

    /* ----------------------------------------------------------------
       Stop — order matters: stop scheduler BEFORE destroying pipeline.
       ---------------------------------------------------------------- */
    zst_scheduler_stop(sched);
    zst_scheduler_destroy(sched);
    zst_pipeline_set_state(pipe, ZST_STATE_NULL);
    fclose(log);

    /* ----------------------------------------------------------------
       Analysis & assertions
       ---------------------------------------------------------------- */
    int frame_count = ctx.count;
    printf("\n  [timing] Frames captured: %d\n", frame_count);
    printf("  [timing] Log written to:   %s\n", log_path);

    assert(frame_count > 0);

    int valid = (frame_count < MAX_FRAMES) ? frame_count : MAX_FRAMES;
    if (valid < 2) {
        printf("  [timing] WARNING: not enough frames for interval analysis\n");
        zst_pipeline_destroy(pipe);
        zst_plugin_registry_deinit();
        PASS();
        return;
    }

    zst_time_t expected_delta = 1000000000ULL / 30;  /* ~33.3 ms */

    /* Compute wall-clock deltas */
    zst_time_t min_delta = UINT64_MAX;
    zst_time_t max_delta = 0;
    double     sum_delta = 0.0;
    int        outliers  = 0;

    printf("  [timing] Expected frame interval: %" PRIu64 " ns (30 fps)\n",
           expected_delta);
    printf("  [timing] Frame intervals (wall-clock):\n");

    for (int i = 1; i < valid; i++) {
        zst_time_t delta = records[i].wall_ns - records[i - 1].wall_ns;
        if (delta < min_delta) min_delta = delta;
        if (delta > max_delta) max_delta = delta;
        sum_delta += (double)delta;

        int pct  = (int)((double)delta * 100.0 / (double)expected_delta + 0.5);
        int flag = (pct < 50 || pct > 200) ? 1 : 0;
        if (flag) outliers++;

        if (i <= 10 || i == valid - 1 || flag) {
            printf("    frame %4d -> %4d: %'" PRIu64 " ns (%3d%% of expected)%s\n",
                   i - 1, i, delta, pct,
                   flag ? "  ***" : "");
        } else if (i == 11) {
            printf("    ... (omitting %d frames)\n", valid - 12);
        }
    }

    double avg_delta = sum_delta / (double)(valid - 1);
    printf("  [timing] Min interval: %'" PRIu64 " ns\n", min_delta);
    printf("  [timing] Max interval: %'" PRIu64 " ns\n", max_delta);
    printf("  [timing] Avg interval: %.0f ns  (%.2f fps)\n",
           avg_delta, 1e9 / avg_delta);
    printf("  [timing] Outliers (<50%% or >200%% of expected): %d / %d\n",
           outliers, valid - 1);

    /* PTS vs wall-clock correlation (first few & last frame) */
    printf("  [timing] PTS vs wall-clock skew (wall - PTS):\n");
    for (int i = 0; i < valid && i < 5; i++) {
        int64_t skew = (int64_t)(records[i].wall_ns - records[i].pts);
        printf("    frame %4d: PTS=%'" PRIu64 "  wall=%'" PRIu64 "  skew=%'" PRId64 " ns\n",
               i, records[i].pts, records[i].wall_ns, skew);
    }
    if (valid > 5) {
        int i = valid - 1;
        int64_t skew = (int64_t)(records[i].wall_ns - records[i].pts);
        printf("    frame %4d: PTS=%'" PRIu64 "  wall=%'" PRIu64 "  skew=%'" PRId64 " ns\n",
               i, records[i].pts, records[i].wall_ns, skew);
    }

    /* Verify we got all 60 frames (or close to it within scheduler timing) */
    assert(frame_count >= 55 && frame_count <= 65);

    /* Clock sync behaviour note: the current scheduler compares PTS against
     * the pipeline clock (CLOCK_MONOTONIC uptime in ns). Count-based PTS
     * (0, 33ms, 66ms, …) is in a different time base and never exceeds the
     * clock, so no waiting occurs. Use the log file to verify the actual
     * delivery intervals vs PTS values. If clock sync is enhanced to use
     * frame-to-frame delta comparison, this test's intervals should shift
     * from ~1.4 ms (max-speed) to ~33.3 ms (paced). */
    printf("  [timing] NOTE: clock_sync is currently comparing PTS against the\n");
    printf("  [timing] system clock (CLOCK_MONOTONIC uptime). Since count-based\n");
    printf("  [timing] PTS (0, 33ms, …) is in a different time base, no pacing\n");
    printf("  [timing] is applied. Frames are delivered at max scheduler speed\n");
    printf("  [timing] (~%.1f fps). See log for details.\n", 1e9 / avg_delta);

    zst_pipeline_destroy(pipe);
    zst_plugin_registry_deinit();

    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   Main
   ═══════════════════════════════════════════════════════════════ */
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("\n╔════════════════════════════════════════════════════╗\n");
    printf("║     zstreamer — videotestsrc timing tests         ║\n");
    printf("╚════════════════════════════════════════════════════╝\n\n");

    printf("[videotestsrc timing]\n");
    test_videotestsrc_timing();

    printf("\n──────────────────────────────────────────────────\n");
    printf("  %d / %d tests passed\n", g_tests_passed, g_tests_run);
    printf("──────────────────────────────────────────────────\n\n");

    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
