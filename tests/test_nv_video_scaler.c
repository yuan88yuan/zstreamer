/*=============================================================================
    test_nv_video_scaler.c — Integration Test for NV V4L2 Video Scaler
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "zst_plugin.h"
#include <fcntl.h>

#include "zst_pipeline.h"
#include "zst_scheduler.h"
#include "zst_element.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_log.h"

static int g_frame_count = 0;
static int g_expected_width = 1280;
static int g_expected_height = 720;
static int g_target_frames = 10;

static zst_pad_probe_return_t
on_frame_probe(zst_pad_t* pad, zst_buffer_t* buf, zst_pad_probe_type_t type, void* user_data)
{
    (void)pad;
    (void)type;
    (void)user_data;

    if (buf) {
        g_frame_count++;
        size_t expected_size = (size_t)g_expected_width * g_expected_height * 3 / 2;
        if (buf->memory.size != expected_size) {
            fprintf(stderr, "[Test] Warning: buffer size %zu does not match expected %zu for %dx%d YUV420P\n",
                    buf->memory.size, expected_size, g_expected_width, g_expected_height);
        } else {
            printf("[Test] Received scaled frame %d, size: %zu\n", g_frame_count, buf->memory.size);
        }
    }
    return ZST_PAD_PROBE_OK;
}

int main(void)
{
    // Check if /dev/nvhost-vic exists
    int fd = open("/dev/nvhost-vic", O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        printf("[System] /dev/nvhost-vic not found. Skipping NV Video Scaler test.\n");
        return EXIT_SUCCESS;
    }
    close(fd);

    printf("[System] Running NV Video Scaler test...\n");

    if (zst_register_builtin_elements() != ZST_OK) {
        fprintf(stderr, "Failed to register builtin elements\n");
        return EXIT_FAILURE;
    }

    zst_pipeline_t* pipe = zst_pipeline_create();
    zst_scheduler_config_t sched_cfg = {
        .mode = ZST_SCHEDULER_SINGLE_THREAD,
        .worker_threads = 1
    };
    zst_scheduler_t* sched = zst_scheduler_create(&sched_cfg);

    zst_element_t* src = zst_element_factory_make("videotestsrc");
    zst_element_t* scaler = zst_element_factory_make("nvvideoscaler");
    zst_element_t* sink = zst_element_factory_make("fakesink");

    if (!src || !scaler || !sink) {
        fprintf(stderr, "Failed to create elements\n");
        return EXIT_FAILURE;
    }

    // Configure videotestsrc
    zst_element_set_property_int(src, "width", 640);
    zst_element_set_property_int(src, "height", 480);
    zst_element_set_property_int(src, "fps", 30);
    zst_element_set_property_int(src, "num-buffers", g_target_frames);

    // Configure nvvideoscaler
    zst_element_set_property_int(scaler, "width", g_expected_width);
    zst_element_set_property_int(scaler, "height", g_expected_height);

    // Add probe to verify and count scaled frames
    zst_pad_t* src_pad = zst_element_get_pad(scaler, "src");
    zst_pad_add_probe(src_pad, ZST_PAD_PROBE_POST_BUFFER, on_frame_probe, NULL);

    zst_pipeline_add(pipe, src);
    zst_pipeline_add(pipe, scaler);
    zst_pipeline_add(pipe, sink);

    zst_pad_link(zst_element_get_pad(src, "src"), zst_element_get_pad(scaler, "sink"));
    zst_pad_link(src_pad, zst_element_get_pad(sink, "sink"));

    if (zst_pipeline_set_state(pipe, ZST_STATE_READY) != ZST_OK) {
        fprintf(stderr, "Failed to set pipeline to READY\n");
        return EXIT_FAILURE;
    }

    if (zst_pipeline_set_state(pipe, ZST_STATE_PLAYING) != ZST_OK) {
        fprintf(stderr, "Failed to set pipeline to PLAYING\n");
        return EXIT_FAILURE;
    }

    zst_scheduler_attach(sched, pipe);
    zst_scheduler_run(sched);

    int timeout_ms = 5000;
    int elapsed_ms = 0;
    while (g_frame_count < g_target_frames && elapsed_ms < timeout_ms) {
        usleep(50000); // 50ms
        elapsed_ms += 50;
    }

    zst_pipeline_set_state(pipe, ZST_STATE_NULL);
    zst_scheduler_stop(sched);
    zst_scheduler_destroy(sched);
    zst_pipeline_destroy(pipe);

    if (g_frame_count < g_target_frames) {
        fprintf(stderr, "[Test] Error: Timeout reached before getting %d frames! Got %d.\n", g_target_frames, g_frame_count);
        return EXIT_FAILURE;
    }

    printf("[System] Test finished successfully.\n");
    return EXIT_SUCCESS;
}
