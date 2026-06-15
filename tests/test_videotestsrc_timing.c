/*=============================================================================
    test_videotestsrc_timing.c — Verify videotestsrc real-time-pacing=true paces
    frame production in real time.

    This intentionally lives outside test_core.c so timing-sensitive checks can
    run as an independent CTest target.
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "zst_buffer.h"
#include "zst_clock.h"
#include "zst_element.h"
#include "zstreamer/elements/zst_video_test_src.h"

static zst_time_t monotonic_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (zst_time_t)ts.tv_sec * 1000000000ULL + (zst_time_t)ts.tv_nsec;
}

static void assert_ok(zst_result_t result, const char* what)
{
    if (result != ZST_OK) {
        fprintf(stderr, "FAIL: %s returned %d\n", what, result);
        assert(result == ZST_OK);
    }
}

static zst_time_t run_source_frames(zst_element_t* src,
                                    int frames,
                                    zst_time_t* return_times,
                                    zst_time_t* pts_values,
                                    zst_time_t* durations)
{
    zst_time_t start = monotonic_now_ns();

    for (int i = 0; i < frames; i++) {
        zst_buffer_t* out = NULL;
        assert_ok(src->ops->process(src, NULL, &out), "videotestsrc process");
        return_times[i] = monotonic_now_ns();

        assert(out != NULL);
        pts_values[i] = out->pts;
        durations[i] = out->duration;
        zst_buffer_unref(out);
    }

    return return_times[frames - 1] - start;
}

static zst_element_t* create_configured_source(bool real_time_pacing, int fps, int frames)
{
    zst_element_t* src = zst_video_test_src_create();
    assert(src != NULL);

    /* Small frames keep CPU/rendering overhead negligible, making the measured
       wall-clock time attributable to real-time-pacing. */
    assert_ok(zst_element_set_property_int(src, "width", 64), "set width");
    assert_ok(zst_element_set_property_int(src, "height", 48), "set height");
    assert_ok(zst_element_set_property_int(src, "fps", fps), "set fps");
    assert_ok(zst_element_set_property_int(src, "num-buffers", frames), "set num-buffers");
    assert_ok(zst_element_set_property(src, "pattern", "black"), "set pattern");
    assert_ok(zst_element_set_property_bool(src, "use-clock", false), "set use-clock");
    assert_ok(zst_element_set_property_bool(src, "real-time-pacing", real_time_pacing), "set real-time-pacing");

    bool prop_value = false;
    assert_ok(zst_element_get_property_bool(src, "real-time-pacing", &prop_value), "get real-time-pacing");
    assert(prop_value == real_time_pacing);

    zst_clock_t* clock = zst_clock_system_create();
    assert(clock != NULL);
    zst_element_set_clock(src, clock);
    zst_clock_unref(clock);

    assert_ok(zst_element_set_state(src, ZST_STATE_PLAYING), "set PLAYING");
    return src;
}

static void destroy_source(zst_element_t* src)
{
    if (!src) return;
    assert_ok(zst_element_set_state(src, ZST_STATE_NULL), "set NULL");
    zst_element_destroy(src);
}

static void test_clock_sync_property_removed(void)
{
    zst_element_t* src = zst_video_test_src_create();
    assert(src != NULL);

    bool prop_value = false;
    assert(zst_element_set_property_bool(src, "clock-sync", true) == ZST_ERROR);
    assert(zst_element_get_property_bool(src, "clock-sync", &prop_value) == ZST_ERROR);

    zst_element_destroy(src);
}

static void test_real_time_pacing_true_is_real_time_paced(void)
{
    const int fps = 20;
    const int frames = 16;
    const zst_time_t frame_duration = 1000000000ULL / (zst_time_t)fps;
    const zst_time_t expected_elapsed = (zst_time_t)(frames - 1) * frame_duration;

    zst_time_t sync_times[frames];
    zst_time_t sync_pts[frames];
    zst_time_t sync_durations[frames];

    zst_element_t* sync_src = create_configured_source(true, fps, frames);
    zst_time_t sync_elapsed = run_source_frames(sync_src, frames,
                                                sync_times,
                                                sync_pts,
                                                sync_durations);
    destroy_source(sync_src);

    for (int i = 0; i < frames; i++) {
        assert(sync_durations[i] == frame_duration);
        assert(sync_pts[i] == (zst_time_t)i * frame_duration);
    }

    zst_time_t min_delta = UINT64_MAX;
    zst_time_t max_delta = 0;
    zst_time_t sum_delta = 0;
    for (int i = 1; i < frames; i++) {
        zst_time_t delta = sync_times[i] - sync_times[i - 1];
        if (delta < min_delta) min_delta = delta;
        if (delta > max_delta) max_delta = delta;
        sum_delta += delta;
    }

    double avg_delta = (double)sum_delta / (double)(frames - 1);

    printf("real-time-pacing=true timing:\n");
    printf("  fps:              %d\n", fps);
    printf("  frames:           %d\n", frames);
    printf("  expected elapsed: %" PRIu64 " ns\n", expected_elapsed);
    printf("  actual elapsed:   %" PRIu64 " ns\n", sync_elapsed);
    printf("  min interval:     %" PRIu64 " ns\n", min_delta);
    printf("  max interval:     %" PRIu64 " ns\n", max_delta);
    printf("  avg interval:     %.0f ns (%.2f fps)\n", avg_delta, 1e9 / avg_delta);

    /* The lower bound is the key real-time-pacing assertion: without
       real-time-pacing this source produces all frames in a CPU-speed burst.  The
       upper bound is deliberately loose to avoid false negatives under noisy
       Docker hosts while still catching multi-second stalls. */
    assert(sync_elapsed >= expected_elapsed * 80 / 100);
    assert(sync_elapsed <= expected_elapsed * 180 / 100);
    assert(avg_delta >= (double)frame_duration * 0.80);
    assert(avg_delta <= (double)frame_duration * 1.80);

    /* Control check: with real-time-pacing=false, the same source should not be
       real-time paced.  This proves the elapsed time above comes from the
       videotestsrc real-time-pacing=true property, not rendering overhead. */
    zst_time_t burst_times[frames];
    zst_time_t burst_pts[frames];
    zst_time_t burst_durations[frames];

    zst_element_t* burst_src = create_configured_source(false, fps, frames);
    zst_time_t burst_elapsed = run_source_frames(burst_src, frames,
                                                 burst_times,
                                                 burst_pts,
                                                 burst_durations);
    destroy_source(burst_src);

    printf("real-time-pacing=false control elapsed: %" PRIu64 " ns\n", burst_elapsed);

    assert(burst_elapsed < expected_elapsed * 25 / 100);
    assert(sync_elapsed > burst_elapsed * 4);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("\n[test_videotestsrc_timing]\n");
    test_clock_sync_property_removed();
    test_real_time_pacing_true_is_real_time_paced();
    printf("PASS: videotestsrc real-time-pacing=true is real-time paced\n\n");

    return 0;
}
