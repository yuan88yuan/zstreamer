/*=============================================================================
    test_audiotestsrc_timing.c — Verify audiotestsrc real-time-pacing=true
    paces generated audio buffers in real time.
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "zst_buffer.h"
#include "zst_clock.h"
#include "zst_element.h"
#include "zstreamer/elements/zst_audio_test_src.h"

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

static zst_time_t run_source_buffers(zst_element_t* src,
                                     int buffers,
                                     zst_time_t* return_times,
                                     zst_time_t* pts_values,
                                     zst_time_t* durations)
{
    zst_time_t start = monotonic_now_ns();

    for (int i = 0; i < buffers; i++) {
        zst_buffer_t* out = NULL;
        assert_ok(src->ops->process(src, NULL, &out), "audiotestsrc process");
        return_times[i] = monotonic_now_ns();

        assert(out != NULL);
        pts_values[i] = out->pts;
        durations[i] = out->duration;
        zst_buffer_unref(out);
    }

    return return_times[buffers - 1] - start;
}

static zst_element_t* create_configured_source(bool real_time_pacing,
                                               uint32_t sample_rate,
                                               uint32_t samples_per_buffer,
                                               int buffers)
{
    zst_element_t* src = zst_audio_test_src_create();
    assert(src != NULL);

    assert_ok(zst_element_set_property_uint(src, "sample-rate", sample_rate), "set sample-rate");
    assert_ok(zst_element_set_property_uint(src, "channels", 1), "set channels");
    assert_ok(zst_element_set_property_string(src, "sample-format", "S16LE"), "set sample-format");
    assert_ok(zst_element_set_property_string(src, "wave", "silence"), "set wave");
    assert_ok(zst_element_set_property_uint(src, "samples-per-buffer", samples_per_buffer), "set samples-per-buffer");
    assert_ok(zst_element_set_property_int(src, "num-buffers", buffers), "set num-buffers");
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

static void test_real_time_pacing_true_is_real_time_paced(void)
{
    const uint32_t sample_rate = 1000;
    const uint32_t samples_per_buffer = 50;
    const int buffers = 16;
    const zst_time_t buffer_duration =
        (zst_time_t)samples_per_buffer * 1000000000ULL / sample_rate;
    const zst_time_t expected_elapsed = (zst_time_t)(buffers - 1) * buffer_duration;

    zst_time_t paced_times[buffers];
    zst_time_t paced_pts[buffers];
    zst_time_t paced_durations[buffers];

    zst_element_t* paced_src = create_configured_source(true, sample_rate,
                                                        samples_per_buffer,
                                                        buffers);
    zst_time_t paced_elapsed = run_source_buffers(paced_src, buffers,
                                                  paced_times,
                                                  paced_pts,
                                                  paced_durations);
    destroy_source(paced_src);

    for (int i = 0; i < buffers; i++) {
        assert(paced_durations[i] == buffer_duration);
        assert(paced_pts[i] == (zst_time_t)i * buffer_duration);
    }

    zst_time_t min_delta = UINT64_MAX;
    zst_time_t max_delta = 0;
    zst_time_t sum_delta = 0;
    for (int i = 1; i < buffers; i++) {
        zst_time_t delta = paced_times[i] - paced_times[i - 1];
        if (delta < min_delta) min_delta = delta;
        if (delta > max_delta) max_delta = delta;
        sum_delta += delta;
    }

    double avg_delta = (double)sum_delta / (double)(buffers - 1);

    printf("real-time-pacing=true audio timing:\n");
    printf("  sample-rate:      %u\n", sample_rate);
    printf("  samples/buffer:   %u\n", samples_per_buffer);
    printf("  buffers:          %d\n", buffers);
    printf("  expected elapsed: %" PRIu64 " ns\n", expected_elapsed);
    printf("  actual elapsed:   %" PRIu64 " ns\n", paced_elapsed);
    printf("  min interval:     %" PRIu64 " ns\n", min_delta);
    printf("  max interval:     %" PRIu64 " ns\n", max_delta);
    printf("  avg interval:     %.0f ns\n", avg_delta);

    assert(paced_elapsed >= expected_elapsed * 80 / 100);
    assert(paced_elapsed <= expected_elapsed * 180 / 100);
    assert(avg_delta >= (double)buffer_duration * 0.80);
    assert(avg_delta <= (double)buffer_duration * 1.80);

    zst_time_t burst_times[buffers];
    zst_time_t burst_pts[buffers];
    zst_time_t burst_durations[buffers];

    zst_element_t* burst_src = create_configured_source(false, sample_rate,
                                                        samples_per_buffer,
                                                        buffers);
    zst_time_t burst_elapsed = run_source_buffers(burst_src, buffers,
                                                  burst_times,
                                                  burst_pts,
                                                  burst_durations);
    destroy_source(burst_src);

    printf("real-time-pacing=false audio control elapsed: %" PRIu64 " ns\n", burst_elapsed);

    assert(burst_elapsed < expected_elapsed * 25 / 100);
    assert(paced_elapsed > burst_elapsed * 4);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("\n[test_audiotestsrc_timing]\n");
    test_real_time_pacing_true_is_real_time_paced();
    printf("PASS: audiotestsrc real-time-pacing=true is real-time paced\n\n");

    return 0;
}
