/*=============================================================================
    test_custom_paced_video_source.c — Test real-time paced video source bin
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
#include "zst_bin.h"

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

typedef struct {
    zst_time_t timestamps[10];
    int count;
} paced_source_probe_ctx_t;

static zst_pad_probe_return_t
paced_source_probe_cb(zst_pad_t* pad, zst_buffer_t* buf,
                      zst_pad_probe_type_t type, void* user_data)
{
    (void)pad;
    (void)type;
    if (!buf || (buf->flags & ZST_BUFFER_FLAG_EOS)) {
        return ZST_PAD_PROBE_OK;
    }

    paced_source_probe_ctx_t* ctx = (paced_source_probe_ctx_t*)user_data;
    if (ctx->count < 10) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ctx->timestamps[ctx->count++] = (zst_time_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    }
    return ZST_PAD_PROBE_OK;
}

static void
test_custom_paced_video_source(void)
{
    TEST("custom real-time paced video source ('test-video-source' bin)");

    zst_plugin_registry_init();
    zst_register_builtin_elements();

    /* 1. Create an element bin named 'test-video-source' containing videotestsrc */
    zst_element_t* bin = zst_bin_create("test-video-source");
    assert(bin != NULL);

    zst_element_t* vts = zst_element_factory_make("videotestsrc");
    assert(vts != NULL);

    /* Configure videotestsrc */
    zst_element_set_property_int(vts, "width", 320);
    zst_element_set_property_int(vts, "height", 240);
    zst_element_set_property_int(vts, "fps", 30);
    zst_element_set_property_int(vts, "num-buffers", 5);
    zst_element_set_property_bool(vts, "use-clock", false); /* count-based PTS for scheduler pacing */

    assert(zst_bin_add(bin, vts) == ZST_OK);

    /* Expose videotestsrc src pad as a ghost pad on the bin */
    zst_pad_t* ghost_src = zst_ghost_pad_create("src", zst_element_get_pad(vts, "src"));
    assert(ghost_src != NULL);
    assert(zst_bin_add_ghost_pad(bin, ghost_src) == ZST_OK);

    /* 2. Create the pipeline: test-video-source -> fakesink */
    zst_pipeline_t* pipe = zst_pipeline_create();
    assert(pipe != NULL);
    zst_pipeline_set_clock_sync(pipe, 1); /* Enable clock-sync pacing on the pipeline */

    zst_element_t* sink = zst_element_factory_make("fakesink");
    assert(sink != NULL);

    assert(zst_pipeline_add(pipe, bin) == ZST_OK);
    assert(zst_pipeline_add(pipe, sink) == ZST_OK);

    zst_pad_t* bin_src = zst_element_get_pad(bin, "src");
    zst_pad_t* sink_pad = zst_element_get_pad(sink, "sink");
    assert(bin_src != NULL && sink_pad != NULL);
    assert(zst_pad_link(bin_src, sink_pad) == ZST_OK);

    /* Add probe on fakesink input pad to measure real-time pacing */
    paced_source_probe_ctx_t ctx = {0};
    uint64_t probe_id = zst_pad_add_probe(sink_pad, ZST_PAD_PROBE_PRE_BUFFER, paced_source_probe_cb, &ctx);
    assert(probe_id != 0);

    /* Run the pipeline with a single-threaded scheduler */
    zst_scheduler_config_t cfg = {
        .mode           = ZST_SCHEDULER_SINGLE_THREAD,
        .worker_threads = 1
    };
    zst_scheduler_t* sched = zst_scheduler_create(&cfg);
    assert(sched != NULL);
    zst_scheduler_attach(sched, pipe);

    assert(zst_pipeline_set_state(pipe, ZST_STATE_PLAYING) == ZST_OK);
    assert(zst_scheduler_run(sched) == ZST_OK);

    /* Wait for the 5 frames to process (at 30 fps, total duration is ~133ms) */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 300000000ULL }; /* 300 ms sleep */
    nanosleep(&ts, NULL);

    /* Stop scheduler and clean up */
    zst_scheduler_stop(sched);
    zst_scheduler_destroy(sched);
    assert(zst_pipeline_set_state(pipe, ZST_STATE_NULL) == ZST_OK);
    zst_pipeline_destroy(pipe);

    /* Assert that all 5 frames were received and were real-time paced */
    assert(ctx.count == 5);
    for (int i = 1; i < 5; i++) {
        zst_time_t diff = ctx.timestamps[i] - ctx.timestamps[i - 1];
        /* Frame rate is 30 fps -> ~33.3 ms (33,333,333 ns).
         * Verify pacing did introduce delay: interval should be at least 15 ms,
         * and not unreasonably long (e.g., less than 150 ms to allow for CI CPU scheduling variance). */
        assert(diff >= 15000000ULL);
        assert(diff <= 150000000ULL);
    }

    zst_plugin_registry_deinit();
    PASS();
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("\n╔════════════════════════════════════════════════════╗\n");
    printf("║     zstreamer — custom paced video source tests    ║\n");
    printf("╚════════════════════════════════════════════════════╝\n\n");

    test_custom_paced_video_source();

    printf("\n──────────────────────────────────────────────────\n");
    printf("  %d / %d tests passed\n", g_tests_passed, g_tests_run);
    printf("──────────────────────────────────────────────────\n\n");

    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
