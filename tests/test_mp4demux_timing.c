#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#include "zst_buffer.h"
#include "zst_clock.h"
#include "zst_element.h"
#include "zst_element_factory.h"
#include "zst_pipeline.h"
#include "zst_scheduler.h"
#include "zst_bus.h"
#include "zst_plugin.h"

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

#define MAX_FRAMES 128
static zst_time_t g_times[MAX_FRAMES];
static zst_time_t g_pts[MAX_FRAMES];
static zst_time_t g_durations[MAX_FRAMES];
static int g_count = 0;

static zst_pad_probe_return_t probe_cb(zst_pad_t* pad, zst_buffer_t* buf, zst_pad_probe_type_t ptype, void* user_data) {
    (void)pad;
    (void)user_data;
    if (ptype == ZST_PAD_PROBE_PRE_BUFFER) {
        if (g_count < MAX_FRAMES && buf->flags != ZST_BUFFER_FLAG_EOS) {
            g_times[g_count] = monotonic_now_ns();
            g_pts[g_count] = buf->pts;
            g_durations[g_count] = buf->duration;
            g_count++;
        }
    }
    return ZST_PAD_PROBE_OK;
}

static void test_real_time_pacing(void) {
    int expected_frames = 16;
    int fps = 30;
    zst_time_t frame_duration = 1000000000ULL / fps;
    zst_time_t expected_elapsed = (zst_time_t)(expected_frames - 1) * frame_duration;

    const char* tmp_file = "/tmp/test_mp4demux_timing.mp4";

    // Write phase
    {
        zst_pipeline_t* pipe = zst_pipeline_create();
        zst_scheduler_config_t cfg = { .mode = ZST_SCHEDULER_SINGLE_THREAD, .worker_threads = 1 };
        zst_scheduler_t* sched = zst_scheduler_create(&cfg);

        zst_element_t* src = zst_element_factory_make("videotestsrc");
        assert_ok(zst_element_set_property_int(src, "width", 64), "set width");
        assert_ok(zst_element_set_property_int(src, "height", 48), "set height");
        assert_ok(zst_element_set_property_int(src, "fps", fps), "set fps");
        assert_ok(zst_element_set_property_int(src, "num-buffers", expected_frames), "set num-buffers");
        assert_ok(zst_element_set_property_bool(src, "use-clock", false), "set use-clock");
        assert_ok(zst_element_set_property_string(src, "pattern", "black"), "set pattern");

        // Use x264enc and mp4mux since mp4mux requires encoded video
        zst_element_t* enc = zst_element_factory_make("x264enc");
        assert_ok(zst_element_set_property_int(enc, "bitrate", 500000), "enc bitrate");

        zst_element_t* mux = zst_element_factory_make("mp4mux");
        assert_ok(zst_element_set_property_int(mux, "width", 64), "mux width");
        assert_ok(zst_element_set_property_int(mux, "height", 48), "mux height");
        assert_ok(zst_element_set_property_int(mux, "fps", fps), "mux fps");
        assert_ok(zst_element_set_property_string(mux, "location", tmp_file), "mux location");

        zst_pipeline_add(pipe, src);
        zst_pipeline_add(pipe, enc);
        zst_pipeline_add(pipe, mux);

        assert_ok(zst_pad_link(zst_element_get_pad(src, "src"), zst_element_get_pad(enc, "sink")), "src->enc");
        assert_ok(zst_pad_link(zst_element_get_pad(enc, "src"), zst_element_get_pad(mux, "video")), "enc->mux");

        assert_ok(zst_scheduler_attach(sched, pipe), "attach");
        assert_ok(zst_pipeline_set_state(pipe, ZST_STATE_PLAYING), "set PLAYING");
        assert_ok(zst_scheduler_run(sched), "run");

        for (;;) {
            zst_event_t* ev = NULL;
            zst_result_t r = zst_bus_pop(zst_pipeline_get_bus(pipe), &ev, 15000);
            if (r == ZST_TIMEOUT || r != ZST_OK || !ev) break;
            if (ev->type == ZST_EVENT_ERROR || ev->type == ZST_EVENT_EOS) {
                zst_event_destroy(ev);
                break;
            }
            zst_event_destroy(ev);
        }

        zst_scheduler_stop(sched);
        zst_pipeline_set_state(pipe, ZST_STATE_NULL);
        zst_scheduler_destroy(sched);
        zst_pipeline_destroy(pipe);
    }

    // Read phase sync
    zst_time_t sync_elapsed = 0;
    {
        g_count = 0;
        zst_pipeline_t* pipe = zst_pipeline_create();
        zst_scheduler_config_t cfg = { .mode = ZST_SCHEDULER_SINGLE_THREAD, .worker_threads = 1 };
        zst_scheduler_t* sched = zst_scheduler_create(&cfg);

        zst_element_t* demux = zst_element_factory_make("mp4demux");
        assert_ok(zst_element_set_property_string(demux, "location", tmp_file), "demux location");
        assert_ok(zst_element_set_property_bool(demux, "real-time-pacing", true), "set real-time-pacing");

        zst_element_t* sink = zst_element_factory_make("fakesink");

        zst_pipeline_add(pipe, demux);
        zst_pipeline_add(pipe, sink);

        assert_ok(zst_pad_link(zst_element_get_pad(demux, "video"), zst_element_get_pad(sink, "sink")), "demux->sink");

        zst_pad_add_probe(zst_element_get_pad(demux, "video"), ZST_PAD_PROBE_PRE_BUFFER, probe_cb, NULL);

        zst_clock_t* clock = zst_clock_system_create();
        zst_pipeline_set_clock(pipe, clock);
        zst_clock_unref(clock);

        assert_ok(zst_scheduler_attach(sched, pipe), "attach");
        assert_ok(zst_pipeline_set_state(pipe, ZST_STATE_PLAYING), "set PLAYING");
        assert_ok(zst_scheduler_run(sched), "run");

        for (;;) {
            zst_event_t* ev = NULL;
            zst_result_t r = zst_bus_pop(zst_pipeline_get_bus(pipe), &ev, 15000);
            if (r == ZST_TIMEOUT || r != ZST_OK || !ev) break;
            if (ev->type == ZST_EVENT_ERROR || ev->type == ZST_EVENT_EOS) {
                zst_event_destroy(ev);
                break;
            }
            zst_event_destroy(ev);
        }

        // Use pad probe times to compute actual duration rather than entire run,
        // since demux startup might take time.
        if (g_count > 1) {
            sync_elapsed = g_times[g_count-1] - g_times[0];
        }

        zst_scheduler_stop(sched);
        zst_pipeline_set_state(pipe, ZST_STATE_NULL);
        zst_scheduler_destroy(sched);
        zst_pipeline_destroy(pipe);
    }

    int sync_count = g_count;

    // Read phase burst
    zst_time_t burst_elapsed = 0;
    {
        g_count = 0;
        zst_pipeline_t* pipe = zst_pipeline_create();
        zst_scheduler_config_t cfg = { .mode = ZST_SCHEDULER_SINGLE_THREAD, .worker_threads = 1 };
        zst_scheduler_t* sched = zst_scheduler_create(&cfg);

        zst_element_t* demux = zst_element_factory_make("mp4demux");
        assert_ok(zst_element_set_property_string(demux, "location", tmp_file), "demux location");
        assert_ok(zst_element_set_property_bool(demux, "real-time-pacing", false), "set real-time-pacing");

        zst_element_t* sink = zst_element_factory_make("fakesink");

        zst_pipeline_add(pipe, demux);
        zst_pipeline_add(pipe, sink);

        assert_ok(zst_pad_link(zst_element_get_pad(demux, "video"), zst_element_get_pad(sink, "sink")), "demux->sink");

        zst_clock_t* clock = zst_clock_system_create();
        zst_pipeline_set_clock(pipe, clock);
        zst_clock_unref(clock);

        assert_ok(zst_scheduler_attach(sched, pipe), "attach");
        assert_ok(zst_pipeline_set_state(pipe, ZST_STATE_PLAYING), "set PLAYING");
        assert_ok(zst_scheduler_run(sched), "run");

        for (;;) {
            zst_event_t* ev = NULL;
            zst_result_t r = zst_bus_pop(zst_pipeline_get_bus(pipe), &ev, 15000);
            if (r == ZST_TIMEOUT || r != ZST_OK || !ev) break;
            if (ev->type == ZST_EVENT_ERROR || ev->type == ZST_EVENT_EOS) {
                zst_event_destroy(ev);
                break;
            }
            zst_event_destroy(ev);
        }

        if (g_count > 1) {
            burst_elapsed = g_times[g_count-1] - g_times[0];
        }

        zst_scheduler_stop(sched);
        zst_pipeline_set_state(pipe, ZST_STATE_NULL);
        zst_scheduler_destroy(sched);
        zst_pipeline_destroy(pipe);
    }

    unlink(tmp_file);

    printf("real-time-pacing=true timing:\n");
    printf("  actual elapsed:   %" PRIu64 " ns\n", sync_elapsed);
    printf("real-time-pacing=false control elapsed: %" PRIu64 " ns\n", burst_elapsed);

    assert(sync_count > 0);
    assert(sync_elapsed >= expected_elapsed * 80 / 100);
    assert(burst_elapsed < expected_elapsed * 25 / 100);
    assert(sync_elapsed > burst_elapsed * 4);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    zst_register_builtin_elements();

    printf("\n[test_mp4demux_timing]\n");
    test_real_time_pacing();
    printf("PASS: mp4demux real-time-pacing=true is real-time paced\n\n");
    return 0;
}
