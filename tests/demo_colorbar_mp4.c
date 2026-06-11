/*=============================================================================
    demo_colorbar_mp4.c — Colorbar + timecode + 1kHz tone → H.264/AAC MP4

    Demonstrates the zstreamer Element Public API:
      - zst_register_builtin_elements()   for factory-based element creation
      - zst_element_factory_make()         for named element instantiation
      - zst_element_set_property_int()     typed integer properties
      - zst_element_set_property_bool()    typed boolean properties
      - zst_element_set_property_string()  typed string properties

    Pipeline:
      videotestsrc → textoverlay → h264enc ─┐
                                             ├→ mp4mux → [file]
      audiotestsrc → aacenc ────────────────┘

    Generates: 10 seconds of 320×240 H.264 video with colour bars and
    a timecode overlay, plus 44.1 kHz 1 kHz sine AAC audio, muxed to MP4.
=============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "zst_pipeline.h"
#include "zst_scheduler.h"
#include "zst_element.h"
#include "zst_element_factory.h"
#include "zst_pad.h"
#include "zst_bus.h"

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

    /*──────────────────────────────────────────────────────────────────────
      Register built-in elements so that zst_element_factory_make() can
      find them without dynamic plugin scanning.
    ──────────────────────────────────────────────────────────────────────*/
    CHECK_OK(zst_register_builtin_elements(), "register builtins");

    pipe = zst_pipeline_create();
    CHECK_PTR(pipe, "zst_pipeline_create");

    zst_scheduler_config_t cfg = {
        .mode = ZST_SCHEDULER_SINGLE_THREAD,
        .worker_threads = 1
    };
    sched = zst_scheduler_create(&cfg);
    CHECK_PTR(sched, "zst_scheduler_create");

    /*──────────────────────────────────────────────────────────────────────
      Create elements via the Element Public API factory.
    ──────────────────────────────────────────────────────────────────────*/
    video_src = zst_element_factory_make("videotestsrc");
    overlay   = zst_element_factory_make("textoverlay");
    h264      = zst_element_factory_make("h264enc");
    audio_src = zst_element_factory_make("audiotestsrc");
    aac       = zst_element_factory_make("aacenc");
    mux       = zst_element_factory_make("mp4mux");

    CHECK_PTR(video_src, "factory make videotestsrc");
    CHECK_PTR(overlay,   "factory make textoverlay");
    CHECK_PTR(h264,      "factory make h264enc");
    CHECK_PTR(audio_src, "factory make audiotestsrc");
    CHECK_PTR(aac,       "factory make aacenc");
    CHECK_PTR(mux,       "factory make mp4mux");

    /*──────────────────────────────────────────────────────────────────────
      Configure elements using the typed Element Public API.

      Each property is set with the type-appropriate setter rather than
      a generic string conversion.
    ──────────────────────────────────────────────────────────────────────*/

    /* -- video source: colour bars, 320×240@30, 300 frames -- */
    CHECK_OK(zst_element_set_property_int(video_src, "width",       width),    "video width");
    CHECK_OK(zst_element_set_property_int(video_src, "height",      height),   "video height");
    CHECK_OK(zst_element_set_property_int(video_src, "fps",         fps),      "video fps");
    CHECK_OK(zst_element_set_property_string(video_src, "pattern",  "bars"),   "video pattern");
    CHECK_OK(zst_element_set_property_int(video_src, "num-buffers", 300),      "video num-buffers");
    CHECK_OK(zst_element_set_property_bool(video_src, "use-clock",  false),    "video use-clock");

    /* -- text overlay: timecode, 24 px, offset (10, 35) -- */
    CHECK_OK(zst_element_set_property_bool(overlay, "timecode",  true),   "overlay timecode");
    CHECK_OK(zst_element_set_property_int(overlay,  "font-size", 24),     "overlay font-size");
    CHECK_OK(zst_element_set_property_int(overlay,  "x",         10),     "overlay x");
    CHECK_OK(zst_element_set_property_int(overlay,  "y",         35),     "overlay y");

    /* -- audio source: 44.1 kHz, stereo, S16LE, 1 kHz sine, 10 s -- */
    CHECK_OK(zst_element_set_property_int(audio_src,   "sample-rate",       sample_rate),  "audio sample-rate");
    CHECK_OK(zst_element_set_property_int(audio_src,   "channels",          channels),     "audio channels");
    CHECK_OK(zst_element_set_property_string(audio_src, "sample-format",    "S16LE"),      "audio sample-format");
    CHECK_OK(zst_element_set_property_string(audio_src, "wave",             "sine"),       "audio wave");
    CHECK_OK(zst_element_set_property_int(audio_src,   "frequency",         1000),         "audio frequency");
    CHECK_OK(zst_element_set_property_int(audio_src,   "samples-per-buffer", 1024),         "audio samples-per-buffer");
    CHECK_OK(zst_element_set_property_int(audio_src,   "num-samples",       441000),       "audio num-samples");
    CHECK_OK(zst_element_set_property_bool(audio_src,  "use-clock",         false),        "audio use-clock");

    /* -- MP4 muxer: stream metadata + output path -- */
    CHECK_OK(zst_element_set_property_int(mux,    "width",       width),       "mux width");
    CHECK_OK(zst_element_set_property_int(mux,    "height",      height),      "mux height");
    CHECK_OK(zst_element_set_property_int(mux,    "fps",         fps),         "mux fps");
    CHECK_OK(zst_element_set_property_int(mux,    "sample-rate", sample_rate), "mux sample-rate");
    CHECK_OK(zst_element_set_property_int(mux,    "channels",    channels),    "mux channels");
    CHECK_OK(zst_element_set_property_string(mux, "location",    output),      "mux location");

    /*──────────────────────────────────────────────────────────────────────
      Build the pipeline topology.
    ──────────────────────────────────────────────────────────────────────*/
    CHECK_OK(zst_pipeline_add(pipe, video_src), "add video_src");
    CHECK_OK(zst_pipeline_add(pipe, overlay),   "add overlay");
    CHECK_OK(zst_pipeline_add(pipe, h264),      "add h264");
    CHECK_OK(zst_pipeline_add(pipe, audio_src), "add audio_src");
    CHECK_OK(zst_pipeline_add(pipe, aac),       "add aac");
    CHECK_OK(zst_pipeline_add(pipe, mux),       "add mux");

    /* Video: videotestsrc → textoverlay → h264enc → mp4mux.video */
    CHECK_OK(zst_pad_link(zst_element_get_pad(video_src, "src"),
                          zst_element_get_pad(overlay,   "sink")), "link video_src→overlay");
    CHECK_OK(zst_pad_link(zst_element_get_pad(overlay, "src"),
                          zst_element_get_pad(h264,    "sink")), "link overlay→h264");
    CHECK_OK(zst_pad_link(zst_element_get_pad(h264, "src"),
                          zst_element_get_pad(mux,  "video")), "link h264→mux");

    /* Audio: audiotestsrc → aacenc → mp4mux.audio */
    CHECK_OK(zst_pad_link(zst_element_get_pad(audio_src, "src"),
                          zst_element_get_pad(aac,       "sink")), "link audio_src→aac");
    CHECK_OK(zst_pad_link(zst_element_get_pad(aac, "src"),
                          zst_element_get_pad(mux, "audio")), "link aac→mux");

    /*──────────────────────────────────────────────────────────────────────
      Run the pipeline.
    ──────────────────────────────────────────────────────────────────────*/
    CHECK_OK(zst_scheduler_attach(sched, pipe), "scheduler attach");
    CHECK_OK(zst_pipeline_set_state(pipe, ZST_STATE_READY),  "pipeline READY");
    CHECK_OK(zst_pipeline_set_state(pipe, ZST_STATE_PLAYING), "pipeline PLAYING");
    CHECK_OK(zst_scheduler_run(sched), "scheduler run");

    printf("Writing %ds %dx%d H.264/AAC MP4 to %s ...\n", seconds, width, height, output);

    /*──────────────────────────────────────────────────────────────────────
      Wait for EOS or error on the pipeline bus.
    ──────────────────────────────────────────────────────────────────────*/
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
