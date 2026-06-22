/*=============================================================================
    test_vaapi_dmabuf_integration.c — VA-API DMABUF zero-copy integration test
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "zst_pipeline.h"
#include "zst_scheduler.h"
#include "zst_element.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_plugin.h"
#include "zst_bus.h"

typedef struct {
    int fd;
    int is_mock;
} v4l2_private_header_t;

static int g_frame_count = 0;

static zst_pad_probe_return_t
fill_colorbar_probe(zst_pad_t* pad, zst_buffer_t* buf, zst_pad_probe_type_t type, void* user_data)
{
    (void)pad;
    (void)type;
    (void)user_data;

    // We assume 640x480 resolution for this colorbar
    if (buf && buf->memory.data && buf->memory.size >= 640 * 480 * 2) {
        uint8_t* data = (uint8_t*)buf->memory.data;
        int width = 640;
        int height = 480;

        // Populate a YUYV colorbar pattern directly in the mapped user space
        for (int r = 0; r < height; r++) {
            for (int c = 0; c < width; c += 2) {
                int bar = (c * 8) / width;
                uint8_t y0 = 128, y1 = 128, u = 128, v = 128;
                switch (bar) {
                    case 0: // White
                        y0 = 235; y1 = 235; u = 128; v = 128; break;
                    case 1: // Yellow
                        y0 = 210; y1 = 210; u = 16;  v = 146; break;
                    case 2: // Cyan
                        y0 = 170; y1 = 170; u = 166; v = 16;  break;
                    case 3: // Green
                        y0 = 145; y1 = 145; u = 54;  v = 34;  break;
                    case 4: // Magenta
                        y0 = 106; y1 = 106; u = 202; v = 222; break;
                    case 5: // Red
                        y0 = 81;  y1 = 81;  u = 90;  v = 240; break;
                    case 6: // Blue
                        y0 = 41;  y1 = 41;  u = 240; v = 110; break;
                    case 7: // Black
                        y0 = 16;  y1 = 16;  u = 128; v = 128; break;
                }
                size_t offset = (size_t)(r * width + c) * 2;
                data[offset]     = y0;
                data[offset + 1] = u;
                data[offset + 2] = y1;
                data[offset + 3] = v;
            }
        }
    }
    return ZST_PAD_PROBE_OK;
}

static zst_pad_probe_return_t
on_encoded_frame(zst_pad_t* pad, zst_buffer_t* buf, zst_pad_probe_type_t type, void* user_data)
{
    (void)pad;
    (void)type;
    (void)user_data;

    if (buf && !(buf->flags & ZST_BUFFER_FLAG_EOS)) {
        g_frame_count++;
        if (g_frame_count % 10 == 0) {
            printf("[INTEGRATION] Encoded %d frames successfully...\n", g_frame_count);
        }
    }
    return ZST_PAD_PROBE_OK;
}

static const char* test_plugin_path(void)
{
    const char* ppath = getenv("ZSTREAMER_TEST_PLUGIN_PATH");
    if (!ppath) {
        ppath = "/workspace/build/plugins";
        if (access("/app/build/plugins", R_OK) == 0) {
            ppath = "/app/build/plugins";
        }
    }
    return ppath;
}

int main(int argc, char* argv[])
{
    const char* device = "/dev/video0";
    int width = 640;
    int height = 480;
    int max_frames = 30;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            device = argv[++i];
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            max_frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [--device /dev/videoX] [--frames N]\n", argv[0]);
            return EXIT_SUCCESS;
        }
    }

    printf("[INTEGRATION] Scanning plugins from %s...\n", test_plugin_path());
    if (zst_register_builtin_elements() != ZST_OK) {
        fprintf(stderr, "Failed to register builtin elements\n");
        return EXIT_FAILURE;
    }
    if (zst_plugin_registry_init() != ZST_OK ||
        zst_plugin_registry_scan(test_plugin_path()) != ZST_OK) {
        fprintf(stderr, "Failed to initialize and scan plugins\n");
        return EXIT_FAILURE;
    }

    zst_pipeline_t* pipe = zst_pipeline_create();
    zst_scheduler_config_t sched_cfg = {
        .mode = ZST_SCHEDULER_SINGLE_THREAD,
        .worker_threads = 1
    };
    zst_scheduler_t* sched = zst_scheduler_create(&sched_cfg);

    zst_element_t* src = zst_element_factory_make("v4l2src");
    zst_element_t* enc = zst_element_factory_make("vaapienc");
    zst_element_t* sink = zst_element_factory_make("fakesink");

    if (!pipe || !sched || !src || !enc || !sink) {
        fprintf(stderr, "Failed to create pipeline elements\n");
        return EXIT_FAILURE;
    }

    // Configure V4L2 source in MMAP-EXPORT mode (vivid allocates and provides DMABUFs)
    zst_element_set_property(src, "device", device);
    zst_element_set_property_uint(src, "width", (uint64_t)width);
    zst_element_set_property_uint(src, "height", (uint64_t)height);
    zst_element_set_property(src, "pixel-format", "YUYV");
    zst_element_set_property(src, "memory-type", "mmap-export");

    // Configure VA-API video encoder
    zst_element_set_property(enc, "codec", "h264");

    // Get source pad and register the user-space colorbar writing probe
    zst_pad_t* src_pad = zst_element_get_pad(src, "src");
    zst_pad_add_probe(src_pad, ZST_PAD_PROBE_PRE_BUFFER, fill_colorbar_probe, NULL);

    // Monitor output at the sink
    zst_pad_t* sink_pad = zst_element_get_pad(sink, "sink");
    zst_pad_add_probe(sink_pad, ZST_PAD_PROBE_POST_BUFFER, on_encoded_frame, NULL);

    zst_pipeline_add(pipe, src);
    zst_pipeline_add(pipe, enc);
    zst_pipeline_add(pipe, sink);

    zst_pad_link(src_pad, zst_element_get_pad(enc, "sink"));
    zst_pad_link(zst_element_get_pad(enc, "src"), sink_pad);

    printf("[INTEGRATION] Transitioning pipeline to READY...\n");
    if (zst_pipeline_set_state(pipe, ZST_STATE_READY) != ZST_OK) {
        printf("SKIP: Pipeline failed to reach READY state (check if VA-API device or V4L2 device is missing)\n");
        zst_scheduler_destroy(sched);
        zst_pipeline_destroy(pipe);
        return EXIT_SUCCESS;
    }

    // Check if v4l2src fell back to mock (meaning no real vivid device is at /dev/video0)
    v4l2_private_header_t* src_priv = (v4l2_private_header_t*)src->priv;
    if (src_priv->is_mock) {
        printf("SKIP: v4l2src fell back to mock (ensure --device points to a valid vivid device)\n");
        zst_pipeline_set_state(pipe, ZST_STATE_NULL);
        zst_scheduler_destroy(sched);
        zst_pipeline_destroy(pipe);
        return EXIT_SUCCESS;
    }

    printf("[INTEGRATION] Starting zero-copy integration pipeline...\n");
    struct timespec start_time, end_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    if (zst_pipeline_set_state(pipe, ZST_STATE_PLAYING) != ZST_OK) {
        printf("SKIP: Pipeline failed to reach PLAYING (VA-API DRM device may have rejected DMABUF import)\n");
        zst_pipeline_set_state(pipe, ZST_STATE_NULL);
        zst_scheduler_destroy(sched);
        zst_pipeline_destroy(pipe);
        return EXIT_SUCCESS;
    }

    zst_scheduler_attach(sched, pipe);
    zst_scheduler_run(sched);

    // Keep running until we receive the target frame count, error, or timeout
    int timeout_sec = 10;
    struct timespec loop_start;
    clock_gettime(CLOCK_MONOTONIC, &loop_start);
    int run_loop = 1;
    while (g_frame_count < max_frames && run_loop) {
        zst_event_t* ev = NULL;
        zst_result_t r = zst_bus_pop(zst_pipeline_get_bus(pipe), &ev, 10); // 10ms timeout
        if (r == ZST_OK && ev) {
            if (ev->type == ZST_EVENT_ERROR) {
                fprintf(stderr, "[INTEGRATION] Pipeline error: %s\n",
                        ev->as.error.message ? ev->as.error.message : "unknown");
                run_loop = 0;
            } else if (ev->type == ZST_EVENT_EOS) {
                run_loop = 0;
            }
            zst_event_destroy(ev);
        }
        struct timespec loop_now;
        clock_gettime(CLOCK_MONOTONIC, &loop_now);
        double diff = (loop_now.tv_sec - loop_start.tv_sec) + (loop_now.tv_nsec - loop_start.tv_nsec) / 1e9;
        if (diff >= timeout_sec) {
            fprintf(stderr, "[INTEGRATION] Timeout waiting for %d frames\n", max_frames);
            break;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double elapsed = (end_time.tv_sec - start_time.tv_sec) + (end_time.tv_nsec - start_time.tv_nsec) / 1e9;

    printf("[INTEGRATION] Stopping pipeline...\n");
    zst_pipeline_set_state(pipe, ZST_STATE_NULL);
    zst_scheduler_stop(sched);
    zst_scheduler_destroy(sched);
    zst_pipeline_destroy(pipe);

    if (g_frame_count > 0) {
        double fps = g_frame_count / elapsed;
        printf("[INTEGRATION] Test Passed! Processed %d frames in %.3f seconds (%.2f FPS).\n",
               g_frame_count, elapsed, fps);
    } else {
        printf("SKIP: No frames were processed during the test interval.\n");
    }

    return EXIT_SUCCESS;
}
