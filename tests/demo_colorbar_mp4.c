#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "zst_pipeline.h"
#include "zst_scheduler.h"
#include "zst_element.h"
#include "zst_pad.h"
#include "zst_bus.h"

zst_element_t* zst_video_test_src_create(void);
zst_element_t* zst_audio_test_src_create(void);
zst_element_t* zst_text_overlay_create(const char* text);
zst_element_t* zst_h264_encoder_create(void);
zst_element_t* zst_aac_encoder_create(void);
zst_element_t* zst_mp4_muxer_create(void);

#define CHECK_OK(expr, label) \
    do { \
        zst_result_t _r = (expr); \
        if (_r != ZST_OK) { \
            fprintf(stderr, "%s failed: %d\n", (label), (int)_r); \
            goto fail; \
        } \
    } while (0)

#define CHECK_PTR(ptr, label) \
    do { \
        if (!(ptr)) { \
            fprintf(stderr, "%s failed\n", (label)); \
            goto fail; \
        } \
    } while (0)

int main(int argc, char** argv)
{
    const char* output = argc > 1 ? argv[1] : "colorbar_timecode_10s.mp4";
    const int width = 320;
    const int height = 240;
    const int fps = 30;
    const int seconds = 10;
    const int sample_rate = 44100;
    const int channels = 2;

    zst_pipeline_t* pipe = NULL;
    zst_scheduler_t* sched = NULL;
    zst_element_t* video_src = NULL;
    zst_element_t* overlay = NULL;
    zst_element_t* h264 = NULL;
    zst_element_t* audio_src = NULL;
    zst_element_t* aac = NULL;
    zst_element_t* mux = NULL;
    int rc = 1;

    pipe = zst_pipeline_create();
    CHECK_PTR(pipe, "zst_pipeline_create");

    zst_scheduler_config_t cfg = {
        .mode = ZST_SCHEDULER_SINGLE_THREAD,
        .worker_threads = 1
    };
    sched = zst_scheduler_create(&cfg);
    CHECK_PTR(sched, "zst_scheduler_create");

    video_src = zst_video_test_src_create();
    overlay = zst_text_overlay_create(NULL);
    h264 = zst_h264_encoder_create();
    audio_src = zst_audio_test_src_create();
    aac = zst_aac_encoder_create();
    mux = zst_mp4_muxer_create();

    CHECK_PTR(video_src, "zst_video_test_src_create");
    CHECK_PTR(overlay, "zst_text_overlay_create");
    CHECK_PTR(h264, "zst_h264_encoder_create");
    CHECK_PTR(audio_src, "zst_audio_test_src_create");
    CHECK_PTR(aac, "zst_aac_encoder_create");
    CHECK_PTR(mux, "zst_mp4_muxer_create");

    CHECK_OK(zst_element_set_property(video_src, "width", "320"), "video width");
    CHECK_OK(zst_element_set_property(video_src, "height", "240"), "video height");
    CHECK_OK(zst_element_set_property(video_src, "fps", "30"), "video fps");
    CHECK_OK(zst_element_set_property(video_src, "pattern", "bars"), "video pattern");
    CHECK_OK(zst_element_set_property(video_src, "num-buffers", "300"), "video num-buffers");
    CHECK_OK(zst_element_set_property(video_src, "use-clock", "false"), "video use-clock");

    CHECK_OK(zst_element_set_property(overlay, "timecode", "true"), "overlay timecode");
    CHECK_OK(zst_element_set_property(overlay, "font-size", "24"), "overlay font-size");
    CHECK_OK(zst_element_set_property(overlay, "x", "10"), "overlay x");
    CHECK_OK(zst_element_set_property(overlay, "y", "35"), "overlay y");

    CHECK_OK(zst_element_set_property(audio_src, "sample-rate", "44100"), "audio sample-rate");
    CHECK_OK(zst_element_set_property(audio_src, "channels", "2"), "audio channels");
    CHECK_OK(zst_element_set_property(audio_src, "sample-format", "S16LE"), "audio sample-format");
    CHECK_OK(zst_element_set_property(audio_src, "wave", "sine"), "audio wave");
    CHECK_OK(zst_element_set_property(audio_src, "frequency", "1000"), "audio frequency");
    CHECK_OK(zst_element_set_property(audio_src, "samples-per-buffer", "1024"), "audio samples-per-buffer");
    CHECK_OK(zst_element_set_property(audio_src, "num-samples", "441000"), "audio num-samples");
    CHECK_OK(zst_element_set_property(audio_src, "use-clock", "false"), "audio use-clock");

    CHECK_OK(zst_element_set_property(mux, "width", "320"), "mux width");
    CHECK_OK(zst_element_set_property(mux, "height", "240"), "mux height");
    CHECK_OK(zst_element_set_property(mux, "fps", "30"), "mux fps");
    CHECK_OK(zst_element_set_property(mux, "sample-rate", "44100"), "mux sample-rate");
    CHECK_OK(zst_element_set_property(mux, "channels", "2"), "mux channels");
    CHECK_OK(zst_element_set_property(mux, "location", output), "mux location");

    CHECK_OK(zst_pipeline_add(pipe, video_src), "add video_src");
    CHECK_OK(zst_pipeline_add(pipe, overlay), "add overlay");
    CHECK_OK(zst_pipeline_add(pipe, h264), "add h264");
    CHECK_OK(zst_pipeline_add(pipe, audio_src), "add audio_src");
    CHECK_OK(zst_pipeline_add(pipe, aac), "add aac");
    CHECK_OK(zst_pipeline_add(pipe, mux), "add mux");

    CHECK_OK(zst_pad_link(zst_element_get_pad(video_src, "src"), zst_element_get_pad(overlay, "sink")), "link video_src->overlay");
    CHECK_OK(zst_pad_link(zst_element_get_pad(overlay, "src"), zst_element_get_pad(h264, "sink")), "link overlay->h264");
    CHECK_OK(zst_pad_link(zst_element_get_pad(h264, "src"), zst_element_get_pad(mux, "video")), "link h264->mux");
    CHECK_OK(zst_pad_link(zst_element_get_pad(audio_src, "src"), zst_element_get_pad(aac, "sink")), "link audio_src->aac");
    CHECK_OK(zst_pad_link(zst_element_get_pad(aac, "src"), zst_element_get_pad(mux, "audio")), "link aac->mux");

    CHECK_OK(zst_scheduler_attach(sched, pipe), "scheduler attach");
    CHECK_OK(zst_pipeline_set_state(pipe, ZST_STATE_READY), "pipeline READY");
    CHECK_OK(zst_pipeline_set_state(pipe, ZST_STATE_PLAYING), "pipeline PLAYING");
    CHECK_OK(zst_scheduler_run(sched), "scheduler run");

    printf("Writing %ds %dx%d H.264/AAC MP4 to %s ...\n", seconds, width, height, output);

    for (;;) {
        zst_event_t* ev = NULL;
        zst_result_t r = zst_bus_pop(zst_pipeline_get_bus(pipe), &ev, 15000);
        if (r == ZST_TIMEOUT) {
            fprintf(stderr, "Timed out waiting for EOS\n");
            break;
        }
        if (r != ZST_OK || !ev) {
            fprintf(stderr, "Bus error while waiting for EOS: %d\n", (int)r);
            break;
        }
        if (ev->type == ZST_EVENT_ERROR) {
            fprintf(stderr, "Pipeline error: %s (%d)\n",
                    ev->as.error.message ? ev->as.error.message : "unknown",
                    (int)ev->as.error.result);
            zst_event_destroy(ev);
            break;
        }
        if (ev->type == ZST_EVENT_EOS) {
            zst_event_destroy(ev);
            rc = 0;
            break;
        }
        zst_event_destroy(ev);
    }

fail:
    if (sched) zst_scheduler_stop(sched);
    if (pipe) zst_pipeline_set_state(pipe, ZST_STATE_NULL);
    if (sched) zst_scheduler_destroy(sched);
    if (pipe) zst_pipeline_destroy(pipe);

    if (rc == 0) {
        printf("Done: %s\n", output);
    }
    return rc;
}
