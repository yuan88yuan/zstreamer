/*=============================================================================
    test_v4l2_dmabuf_sim.c — V4L2 vivid DMA-BUF capture simulation
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "zst_pipeline.h"
#include "zst_scheduler.h"
#include "zst_element.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_plugin.h"

/* Struct prefix matching v4l2_source_t's private header. */
typedef struct {
    int fd;
    int is_mock;
} v4l2_private_header_t;

static int g_frame_count = 0;
static int g_probe_error = 0;

static zst_pad_probe_return_t
on_dmabuf_frame(zst_pad_t* pad, zst_buffer_t* buf, zst_pad_probe_type_t type, void* user_data)
{
    (void)pad;
    (void)type;
    (void)user_data;

    if (!buf) {
        g_probe_error = 1;
        return ZST_PAD_PROBE_DROP;
    }

    g_frame_count++;

    if (buf->memory.type != ZST_MEMORY_DMABUF || !buf->memory.priv) {
        fprintf(stderr, "[DMABUF] Error: frame %d is not DMABUF-backed\n", g_frame_count);
        g_probe_error = 1;
        return ZST_PAD_PROBE_DROP;
    }

    int fd = *(int*)buf->memory.priv;
    if (fd < 0) {
        fprintf(stderr, "[DMABUF] Error: frame %d has invalid DMABUF fd\n", g_frame_count);
        g_probe_error = 1;
        return ZST_PAD_PROBE_DROP;
    }

    if (!buf->memory.data || buf->memory.size == 0) {
        fprintf(stderr, "[DMABUF] Error: frame %d has invalid mapped payload\n", g_frame_count);
        g_probe_error = 1;
        return ZST_PAD_PROBE_DROP;
    }

    if (g_frame_count % 5 == 0) {
        printf("[DMABUF] Received %d frames, fd=%d, size=%zu\n", g_frame_count, fd, buf->memory.size);
    }

    return ZST_PAD_PROBE_OK;
}

int
main(int argc, char* argv[])
{
    const char* device = "/dev/video0";
    int width = 640;
    int height = 480;
    int frames = 10;
    int duration = 5;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            device = argv[++i];
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            width = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            height = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            duration = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [--device /dev/videoX] [--frames N] [--duration seconds] [--width W] [--height H]\n", argv[0]);
            return EXIT_SUCCESS;
        }
    }

    printf("[DMABUF] Starting V4L2 DMABUF simulation on %s (%dx%d, %d frames)...\n",
           device, width, height, frames);

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

    zst_element_t* src = zst_element_factory_make("v4l2src");
    zst_element_t* sink = zst_element_factory_make("fakesink");

    if (!pipe || !sched || !src || !sink) {
        fprintf(stderr, "Failed to create V4L2 DMABUF simulation pipeline\n");
        return EXIT_FAILURE;
    }

    zst_element_set_property(src, "device", device);
    zst_element_set_property_int(src, "width", width);
    zst_element_set_property_int(src, "height", height);
    zst_element_set_property(src, "pixel-format", "YUYV");
    zst_element_set_property(src, "memory-type", "dmabuf");

    zst_pad_t* src_pad = zst_element_get_pad(src, "src");
    zst_pad_add_probe(src_pad, ZST_PAD_PROBE_POST_BUFFER, on_dmabuf_frame, NULL);

    zst_pipeline_add(pipe, src);
    zst_pipeline_add(pipe, sink);
    zst_pad_link(src_pad, zst_element_get_pad(sink, "sink"));

    if (zst_pipeline_set_state(pipe, ZST_STATE_READY) != ZST_OK) {
        fprintf(stderr, "Failed to set pipeline to READY\n");
        return EXIT_FAILURE;
    }

    v4l2_private_header_t* src_priv = (v4l2_private_header_t*)src->priv;
    if (src_priv->is_mock) {
        fprintf(stderr, "[DMABUF] Error: v4l2src fell back to mock. Ensure %s is a vivid capture device.\n", device);
        return EXIT_FAILURE;
    }

    if (zst_pipeline_set_state(pipe, ZST_STATE_PLAYING) != ZST_OK) {
        fprintf(stderr, "Failed to set pipeline to PLAYING (VIDIOC_QBUF/STREAMON may have rejected DMABUF fds)\n");
        return EXIT_FAILURE;
    }

    zst_scheduler_attach(sched, pipe);
    zst_scheduler_run(sched);

    int timeout_ms = duration * 1000;
    int elapsed_ms = 0;
    while (g_frame_count < frames && elapsed_ms < timeout_ms && !g_probe_error) {
        usleep(50000);
        elapsed_ms += 50;
    }

    printf("[DMABUF] Captured %d / %d expected frames.\n", g_frame_count, frames);

    zst_pipeline_set_state(pipe, ZST_STATE_NULL);
    zst_scheduler_stop(sched);
    zst_scheduler_destroy(sched);
    zst_pipeline_destroy(pipe);

    if (g_probe_error || g_frame_count < frames) {
        return EXIT_FAILURE;
    }

    printf("[DMABUF] V4L2 DMABUF simulation test passed.\n");
    return EXIT_SUCCESS;
}
