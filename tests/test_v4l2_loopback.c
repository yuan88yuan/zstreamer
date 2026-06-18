/*=============================================================================
    test_v4l2_loopback.c — Multi-service V4L2 Loopback Integration Test
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#include "zst_pipeline.h"
#include "zst_scheduler.h"
#include "zst_element.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_log.h"
#include "zst_plugin.h"

// Struct prefix matching both v4l2_source_t and v4l2_sink_t's private headers
typedef struct {
    int fd;
    int is_mock;
} v4l2_private_header_t;

static int g_frame_count = 0;
static int g_expected_width = 640;
static int g_expected_height = 480;
static int g_probe_error = 0;
static const char* g_expected_memory_type = "mmap";

static zst_pad_probe_return_t
on_frame_probe(zst_pad_t* pad, zst_buffer_t* buf, zst_pad_probe_type_t type, void* user_data)
{
    (void)pad;
    (void)type;
    (void)user_data;

    if (buf) {
        g_frame_count++;
        size_t expected_size = (size_t)g_expected_width * g_expected_height * 3 / 2;
        if (strcmp(g_expected_memory_type, "mmap-export") == 0) {
            expected_size = (size_t)g_expected_width * g_expected_height * 2;
            if (buf->memory.type != ZST_MEMORY_DMABUF || !buf->memory.priv) {
                fprintf(stderr, "[Consumer] Error: mmap-export did not produce a DMABUF-backed buffer\n");
                g_probe_error = 1;
                return ZST_PAD_PROBE_DROP;
            }
        }
        if (buf->memory.size != expected_size) {
            fprintf(stderr, "[Consumer] Warning: buffer size %zu does not match expected %zu\n",
                    buf->memory.size, expected_size);
        }
        if (g_frame_count % 10 == 0) {
            printf("[Consumer] Received %d frames, buffer size: %zu\n", g_frame_count, buf->memory.size);
        }
    }
    return ZST_PAD_PROBE_OK;
}

int main(int argc, char* argv[])
{
    const char* device = "/dev/video10";
    const char* mode = NULL;
    int frames = 30;
    int width = 640;
    int height = 480;
    int duration = 5;
    const char* memory_type = "mmap";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            device = argv[++i];
        } else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            mode = argv[++i];
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            width = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            height = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            duration = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--memory-type") == 0 && i + 1 < argc) {
            memory_type = argv[++i];
        }
    }

    if (!mode) {
        fprintf(stderr, "Usage: %s --mode <read|write> [--device <device>] [--frames <frames>] [--width <width>] [--height <height>] [--memory-type <mmap|mmap-export|userptr|dmabuf>]\n", argv[0]);
        return EXIT_FAILURE;
    }

    // Register all builtin elements
    if (zst_register_builtin_elements() != ZST_OK) {
        fprintf(stderr, "Failed to register builtin elements\n");
        return EXIT_FAILURE;
    }

    g_expected_width = width;
    g_expected_height = height;
    g_expected_memory_type = memory_type;

    zst_pipeline_t* pipe = zst_pipeline_create();
    zst_scheduler_config_t sched_cfg = {
        .mode = ZST_SCHEDULER_SINGLE_THREAD,
        .worker_threads = 1
    };
    zst_scheduler_t* sched = zst_scheduler_create(&sched_cfg);

    if (strcmp(mode, "write") == 0) {
        printf("[Producer] Starting V4L2 Loopback Producer on %s (%dx%d, %d frames)...\n", device, width, height, frames);

        zst_element_t* src = zst_element_factory_make("videotestsrc");
        zst_element_t* scaler = zst_element_factory_make("videoscaler");
        zst_element_t* sink = zst_element_factory_make("v4l2sink");

        if (!src || !scaler || !sink) {
            fprintf(stderr, "Failed to create producer elements\n");
            return EXIT_FAILURE;
        }

        // Configure videotestsrc
        zst_element_set_property_int(src, "width", width);
        zst_element_set_property_int(src, "height", height);
        zst_element_set_property_int(src, "fps", 30);
        zst_element_set_property_int(src, "num-buffers", frames);
        zst_element_set_property_bool(src, "real-time-pacing", true);

        // Configure videoscaler (convert YUV420P -> YUYV)
        zst_element_set_property_int(scaler, "width", width);
        zst_element_set_property_int(scaler, "height", height);
        zst_element_set_property(scaler, "pixel-format", "YUYV");

        // Configure v4l2sink
        zst_element_set_property(sink, "device", device);
        zst_element_set_property_int(sink, "width", width);
        zst_element_set_property_int(sink, "height", height);
        zst_element_set_property(sink, "pixel-format", "YUYV");

        // Add to pipeline
        zst_pipeline_add(pipe, src);
        zst_pipeline_add(pipe, scaler);
        zst_pipeline_add(pipe, sink);

        // Link: src -> scaler -> sink
        zst_pad_link(zst_element_get_pad(src, "src"), zst_element_get_pad(scaler, "sink"));
        zst_pad_link(zst_element_get_pad(scaler, "src"), zst_element_get_pad(sink, "sink"));

        // Transition states
        if (zst_pipeline_set_state(pipe, ZST_STATE_READY) != ZST_OK) {
            fprintf(stderr, "Failed to set pipeline to READY\n");
            return EXIT_FAILURE;
        }

        // Verify that v4l2sink did not fall back to mock
        v4l2_private_header_t* sink_priv = (v4l2_private_header_t*)sink->priv;
        if (sink_priv->is_mock) {
            fprintf(stderr, "[Producer] Error: v4l2sink fell back to mock! Ensure %s is a valid v4l2loopback device and writable.\n", device);
            return EXIT_FAILURE;
        }

        if (zst_pipeline_set_state(pipe, ZST_STATE_PLAYING) != ZST_OK) {
            fprintf(stderr, "Failed to set pipeline to PLAYING\n");
            return EXIT_FAILURE;
        }

        zst_scheduler_attach(sched, pipe);
        zst_scheduler_run(sched);

        // Run until duration elapsed or we produced all frames
        printf("[Producer] Streaming...\n");
        for (int s = 0; s < duration * 10; s++) {
            usleep(100000); // 100ms
        }

    } else if (strcmp(mode, "read") == 0) {
        printf("[Consumer] Starting V4L2 Loopback Consumer on %s (%dx%d, expecting %d frames)...\n", device, width, height, frames);

        zst_element_t* src = zst_element_factory_make("v4l2src");
        zst_element_t* sink = zst_element_factory_make("fakesink");

        if (!src || !sink) {
            fprintf(stderr, "Failed to create consumer elements\n");
            return EXIT_FAILURE;
        }

        // Configure v4l2src
        zst_element_set_property(src, "device", device);
        zst_element_set_property_int(src, "width", width);
        zst_element_set_property_int(src, "height", height);
        zst_element_set_property(src, "memory-type", memory_type);

        // Add probe to verify and count frames
        zst_pad_t* src_pad = zst_element_get_pad(src, "src");
        zst_pad_add_probe(src_pad, ZST_PAD_PROBE_POST_BUFFER, on_frame_probe, NULL);

        // Add to pipeline
        zst_pipeline_add(pipe, src);
        zst_pipeline_add(pipe, sink);

        // Link
        zst_pad_link(src_pad, zst_element_get_pad(sink, "sink"));

        // Transition states
        if (zst_pipeline_set_state(pipe, ZST_STATE_READY) != ZST_OK) {
            fprintf(stderr, "Failed to set consumer pipeline to READY\n");
            return EXIT_FAILURE;
        }

        // Verify that v4l2src did not fall back to mock
        v4l2_private_header_t* src_priv = (v4l2_private_header_t*)src->priv;
        if (src_priv->is_mock) {
            fprintf(stderr, "[Consumer] Error: v4l2src fell back to mock! Ensure %s is a valid v4l2loopback device and has a writer active.\n", device);
            return EXIT_FAILURE;
        }

        if (zst_pipeline_set_state(pipe, ZST_STATE_PLAYING) != ZST_OK) {
            fprintf(stderr, "Failed to set consumer pipeline to PLAYING\n");
            return EXIT_FAILURE;
        }

        zst_scheduler_attach(sched, pipe);
        zst_scheduler_run(sched);

        // Run until duration elapsed or we got our frames
        printf("[Consumer] Capturing...\n");
        int timeout_ms = duration * 1000;
        int elapsed_ms = 0;
        while (g_frame_count < frames && elapsed_ms < timeout_ms) {
            usleep(50000); // 50ms
            elapsed_ms += 50;
        }

        printf("[Consumer] Captured %d / %d expected frames.\n", g_frame_count, frames);
        if (g_frame_count < frames) {
            fprintf(stderr, "[Consumer] Error: Timeout reached before getting %d frames!\n", frames);
            return EXIT_FAILURE;
        }
        if (g_probe_error) {
            return EXIT_FAILURE;
        }

    } else {
        fprintf(stderr, "Unknown mode: %s\n", mode);
        return EXIT_FAILURE;
    }

    // Clean up
    zst_pipeline_set_state(pipe, ZST_STATE_NULL);
    zst_scheduler_stop(sched);
    zst_scheduler_destroy(sched);
    zst_pipeline_destroy(pipe);

    printf("[System] Test finished successfully.\n");
    return EXIT_SUCCESS;
}
