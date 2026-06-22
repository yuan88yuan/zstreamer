/*=============================================================================
    test_x11sink.c — Smoke tests for X11 video sink
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zst_buffer.h"
#include "zst_element.h"
#include "zst_element_factory.h"
#include "zst_pipeline.h"
#include "zst_scheduler.h"
#include "zstreamer/elements/zst_x11_sink.h"

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", (msg)); \
        return 1; \
    } \
} while (0)

static zst_buffer_t*
make_yuv420p_buffer(uint32_t width, uint32_t height)
{
    size_t y_size = (size_t)width * height;
    size_t uv_size = ((size_t)width / 2) * (height / 2);
    size_t total = y_size + uv_size * 2;

    zst_buffer_t* buf = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
    if (!buf) return NULL;

    buf->memory.data = calloc(1, total);
    buf->memory.size = total;
    buf->memory.type = ZST_MEMORY_CPU;
    if (!buf->memory.data) {
        zst_buffer_unref(buf);
        return NULL;
    }

    zst_video_frame_t* frame = calloc(1, sizeof(*frame));
    if (!frame) {
        free(buf->memory.data);
        zst_buffer_unref(buf);
        return NULL;
    }

    uint8_t* data = (uint8_t*)buf->memory.data;
    memset(data, 128, y_size);                 /* Y */
    memset(data + y_size, 128, uv_size);       /* U */
    memset(data + y_size + uv_size, 128, uv_size); /* V */

    frame->width = width;
    frame->height = height;
    frame->format = 0; /* YUV420P */
    frame->plane[0] = data;
    frame->plane[1] = data + y_size;
    frame->plane[2] = data + y_size + uv_size;
    frame->stride[0] = width;
    frame->stride[1] = width / 2;
    frame->stride[2] = width / 2;
    buf->payload = frame;
    return buf;
}

static void
free_test_buffer(zst_buffer_t* buf)
{
    if (!buf) return;
    free(buf->payload);
    free(buf->memory.data);
    buf->payload = NULL;
    buf->memory.data = NULL;
    zst_buffer_unref(buf);
}

static int
test_factory_and_properties(void)
{
    zst_element_t* sink = zst_element_factory_make(ZST_X11_SINK_FACTORY);
    CHECK(sink != NULL, "factory could not create x11sink");

    CHECK(zst_element_set_property_string(sink, ZST_X11_SINK_PROP_DISPLAY, ":65534") == ZST_OK,
          "set display property failed");
    CHECK(zst_element_set_property_string(sink, ZST_X11_SINK_PROP_WINDOW_TITLE, "x11sink test") == ZST_OK,
          "set window-title property failed");

    char value[256];
    CHECK(zst_element_get_property_string(sink, ZST_X11_SINK_PROP_DISPLAY, value, sizeof(value)) == ZST_OK,
          "get display property failed");
    CHECK(strcmp(value, ":65534") == 0, "display property mismatch");

    CHECK(zst_element_get_property_string(sink, ZST_X11_SINK_PROP_WINDOW_TITLE, value, sizeof(value)) == ZST_OK,
          "get window-title property failed");
    CHECK(strcmp(value, "x11sink test") == 0, "window-title property mismatch");

    zst_element_destroy(sink);
    return 0;
}

static int
test_state_and_process_null_mode(void)
{
    zst_element_t* sink = zst_x11_sink_create(":65534");
    CHECK(sink != NULL, "direct create failed");

    CHECK(zst_element_set_state(sink, ZST_STATE_READY) == ZST_OK,
          "NULL -> READY should succeed in null mode");
    CHECK(zst_element_set_state(sink, ZST_STATE_PLAYING) == ZST_OK,
          "READY -> PLAYING failed");

    zst_buffer_t* buf = make_yuv420p_buffer(64, 48);
    CHECK(buf != NULL, "could not allocate test buffer");
    CHECK(sink->ops->process(sink, buf, NULL) == ZST_OK,
          "process YUV420P buffer failed");
    free_test_buffer(buf);

    char count[32];
    CHECK(zst_element_get_property_string(sink, ZST_X11_SINK_PROP_FRAME_COUNT, count, sizeof(count)) == ZST_OK,
          "get frame-count failed");
    CHECK(strcmp(count, "1") == 0, "frame-count should be 1");

    CHECK(zst_element_set_state(sink, ZST_STATE_NULL) == ZST_OK,
          "READY/PLAYING -> NULL failed");
    zst_element_destroy(sink);
    return 0;
}

static int
test_pipeline_smoke(void)
{
    zst_pipeline_t* pipe = zst_pipeline_create();
    CHECK(pipe != NULL, "pipeline create failed");

    zst_element_t* src = zst_element_factory_make("videotestsrc");
    zst_element_t* sink = zst_element_factory_make("x11sink");
    CHECK(src != NULL && sink != NULL, "factory make videotestsrc/x11sink failed");

    zst_element_set_property_uint(src, "width", 64);
    zst_element_set_property_uint(src, "height", 48);
    zst_element_set_property_int(src, "num-buffers", 3);
    const char* force_null = getenv("ZST_X11SINK_FORCE_NULL");
    if (force_null && strcmp(force_null, "0") != 0) {
        zst_element_set_property_string(sink, "display", ":65534");
    }

    CHECK(zst_pipeline_add(pipe, src) == ZST_OK, "add source failed");
    CHECK(zst_pipeline_add(pipe, sink) == ZST_OK, "add sink failed");
    CHECK(zst_pad_link(src->src_pads[0], sink->sink_pads[0]) == ZST_OK, "pad link failed");

    zst_scheduler_config_t cfg = { .mode = ZST_SCHEDULER_SINGLE_THREAD };
    zst_scheduler_t* sched = zst_scheduler_create(&cfg);
    CHECK(sched != NULL, "scheduler create failed");
    CHECK(zst_scheduler_attach(sched, pipe) == ZST_OK, "set scheduler pipeline failed");

    CHECK(zst_pipeline_set_state(pipe, ZST_STATE_PLAYING) == ZST_OK, "pipeline PLAYING failed");
    CHECK(zst_scheduler_run(sched) == ZST_OK, "scheduler run failed");

    zst_scheduler_destroy(sched);
    zst_pipeline_destroy(pipe);
    return 0;
}

int main(void)
{
    CHECK(zst_register_builtin_elements() == ZST_OK, "register builtins failed");

    if (test_factory_and_properties() != 0) return 1;
    if (test_state_and_process_null_mode() != 0) return 1;
    if (test_pipeline_smoke() != 0) return 1;

    printf("test_x11sink: PASS\n");
    return 0;
}
