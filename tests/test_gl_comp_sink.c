/*=============================================================================
    test_gl_comp_sink.c — OpenGL compositor sink tests
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "zst_types.h"
#include "zst_buffer.h"
#include "zst_element.h"
#include "zst_element_factory.h"
#include "zstreamer/elements/zst_gl_comp_sink.h"

static int g_tests_run = 0;
static int g_tests_passed = 0;

#define TEST(name) do { g_tests_run++; printf("  TEST: %-50s ... ", name); fflush(stdout); } while (0)
#define PASS() do { g_tests_passed++; printf("PASSED\n"); } while (0)
#define FAIL(msg) do { printf("FAILED: %s\n", msg); return; } while (0)

static void
free_video_buffer(zst_buffer_t* buf)
{
    if (!buf) return;
    free(buf->payload);
    free(buf->memory.data);
}

static zst_buffer_t*
make_yuv420p_buffer(uint32_t width, uint32_t height, unsigned char yv, unsigned char uv, unsigned char vv)
{
    size_t y_size = width * height;
    size_t uv_size = (width / 2) * (height / 2);
    size_t total = y_size + uv_size * 2;

    zst_buffer_t* buf = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
    if (!buf) return NULL;
    buf->destroy = free_video_buffer;
    buf->memory.data = calloc(1, total);
    buf->memory.size = total;
    buf->memory.type = ZST_MEMORY_CPU;

    zst_video_frame_t* frame = calloc(1, sizeof(*frame));
    if (!frame || !buf->memory.data) {
        free(frame);
        zst_buffer_unref(buf);
        return NULL;
    }
    frame->width = width;
    frame->height = height;
    frame->format = 0;
    frame->plane[0] = buf->memory.data;
    frame->plane[1] = (unsigned char*)buf->memory.data + y_size;
    frame->plane[2] = (unsigned char*)buf->memory.data + y_size + uv_size;
    frame->stride[0] = width;
    frame->stride[1] = width / 2;
    frame->stride[2] = width / 2;
    memset(frame->plane[0], yv, y_size);
    memset(frame->plane[1], uv, uv_size);
    memset(frame->plane[2], vv, uv_size);
    buf->payload = frame;
    buf->duration = 33333333;
    return buf;
}

static zst_buffer_t*
make_rgb_buffer(uint32_t width, uint32_t height, unsigned char r, unsigned char g, unsigned char b)
{
    size_t total = width * height * 3;
    zst_buffer_t* buf = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
    if (!buf) return NULL;
    buf->destroy = free_video_buffer;
    buf->memory.data = malloc(total);
    buf->memory.size = total;
    buf->memory.type = ZST_MEMORY_CPU;

    zst_video_frame_t* frame = calloc(1, sizeof(*frame));
    if (!frame || !buf->memory.data) {
        free(frame);
        zst_buffer_unref(buf);
        return NULL;
    }
    unsigned char* p = buf->memory.data;
    for (size_t i = 0; i < total; i += 3) {
        p[i + 0] = r;
        p[i + 1] = g;
        p[i + 2] = b;
    }
    frame->width = width;
    frame->height = height;
    frame->format = 2;
    frame->plane[0] = p;
    frame->stride[0] = width * 3;
    buf->payload = frame;
    buf->duration = 33333333;
    return buf;
}

static zst_element_t*
make_glcompsink(void)
{
    return zst_element_factory_make("glcompsink");
}

static void
test_factory_create(void)
{
    zst_element_t* el = make_glcompsink();
    if (!el) FAIL("zst_element_factory_make(\"glcompsink\") returned NULL");
    if (!zst_element_get_pad(el, "sink_0")) FAIL("default sink_0 missing");
    zst_element_destroy(el);
    PASS();
}

static void
test_properties_and_dynamic_pads(void)
{
    zst_element_t* el = make_glcompsink();
    if (!el) FAIL("factory make failed");

    if (zst_element_set_property_string(el, "window-title", "Compositor Test") != ZST_OK) FAIL("set window-title failed");
    if (zst_element_set_property_uint(el, "canvas-width", 800) != ZST_OK) FAIL("set canvas-width failed");
    if (zst_element_set_property_uint(el, "canvas-height", 600) != ZST_OK) FAIL("set canvas-height failed");
    if (zst_element_set_property_string(el, "background-color", "#203040ff") != ZST_OK) FAIL("set background-color failed");
    if (zst_element_set_property_uint(el, "input-count", 4) != ZST_OK) FAIL("set input-count failed");

    for (int i = 0; i < 4; i++) {
        char pad_name[16];
        snprintf(pad_name, sizeof(pad_name), "sink_%d", i);
        if (!zst_element_get_pad(el, pad_name)) FAIL("requested sink pad missing");
    }

    if (zst_element_set_property_int(el, "sink_1::x", 123) != ZST_OK) FAIL("set pad x failed");
    if (zst_element_set_property_int(el, "sink_1::y", 45) != ZST_OK) FAIL("set pad y failed");
    if (zst_element_set_property_uint(el, "sink_1::width", 320) != ZST_OK) FAIL("set pad width failed");
    if (zst_element_set_property_uint(el, "sink_1::height", 240) != ZST_OK) FAIL("set pad height failed");
    if (zst_element_set_property_double(el, "sink_1::alpha", 0.5) != ZST_OK) FAIL("set pad alpha failed");
    if (zst_element_set_property_string(el, "sink_1::scaling", "crop") != ZST_OK) FAIL("set pad scaling failed");
    if (zst_element_set_property_string(el, "sink_1::scaling", "invalid") == ZST_OK) FAIL("invalid scaling should fail");

    char buf[128];
    if (zst_element_get_property_string(el, "window-title", buf, sizeof(buf)) != ZST_OK || strcmp(buf, "Compositor Test") != 0) FAIL("get window-title failed");
    if (zst_element_get_property_string(el, "sink_1::scaling", buf, sizeof(buf)) != ZST_OK || strcmp(buf, "crop") != 0) FAIL("get pad scaling failed");
    uint64_t n = 0;
    if (zst_element_get_property_uint(el, "input-count", &n) != ZST_OK || n != 4) FAIL("get input-count failed");

    zst_element_destroy(el);
    PASS();
}

static void
test_request_pad_api(void)
{
    zst_element_t* el = make_glcompsink();
    if (!el) FAIL("factory make failed");
    zst_pad_t* p = zst_gl_comp_sink_request_pad(el, NULL);
    if (!p || strcmp(p->name, "sink_1") != 0) FAIL("auto request pad failed");
    p = zst_gl_comp_sink_request_pad(el, "sink_7");
    if (!p || strcmp(p->name, "sink_7") != 0) FAIL("named request pad failed");
    zst_element_destroy(el);
    PASS();
}

static void
test_null_mode_multi_input(void)
{
    char* old_display = getenv("DISPLAY") ? strdup(getenv("DISPLAY")) : NULL;
    unsetenv("DISPLAY");

    zst_element_t* el = make_glcompsink();
    if (!el) FAIL("factory make failed");
    zst_element_set_property_uint(el, "input-count", 2);
    zst_result_t r = zst_element_set_state(el, ZST_STATE_READY);
    if (r != ZST_OK) FAIL("NULL -> READY failed");
    r = zst_element_set_state(el, ZST_STATE_PLAYING);
    if (r != ZST_OK) FAIL("READY -> PLAYING failed");

    zst_buffer_t* a = make_yuv420p_buffer(160, 120, 80, 90, 240);
    zst_buffer_t* b = make_rgb_buffer(160, 120, 10, 200, 40);
    if (!a || !b) FAIL("buffer allocation failed");

    zst_pad_t* p0 = zst_element_get_pad(el, "sink_0");
    zst_pad_t* p1 = zst_element_get_pad(el, "sink_1");
    if (!p0 || !p1 || !p0->push || !p1->push) FAIL("sink pad push missing");
    if (p0->push(p0, a) != ZST_OK) FAIL("push sink_0 failed");
    if (p1->push(p1, b) != ZST_OK) FAIL("push sink_1 failed");
    zst_buffer_unref(a);
    zst_buffer_unref(b);

    char val[64];
    if (zst_element_get_property_string(el, "null-mode", val, sizeof(val)) != ZST_OK || strcmp(val, "true") != 0) FAIL("null-mode property not true");
    if (zst_element_get_property_string(el, "sink_1::frame-count", val, sizeof(val)) != ZST_OK || strcmp(val, "1") != 0) FAIL("pad frame-count wrong");

    zst_element_set_state(el, ZST_STATE_NULL);
    zst_element_destroy(el);
    if (old_display) { setenv("DISPLAY", old_display, 1); free(old_display); }
    PASS();
}

static void
test_eos_per_pad(void)
{
    zst_element_t* el = make_glcompsink();
    if (!el) FAIL("factory make failed");
    zst_element_set_property_uint(el, "input-count", 2);
    zst_pad_t* p0 = zst_element_get_pad(el, "sink_0");
    zst_pad_t* p1 = zst_element_get_pad(el, "sink_1");
    zst_buffer_t* eos = zst_buffer_create(ZST_BUFFER_USER);
    if (!eos) FAIL("eos alloc failed");
    eos->flags |= ZST_BUFFER_FLAG_EOS;
    if (p0->push(p0, eos) != ZST_OK) FAIL("first EOS should not finish all pads");
    if (p1->push(p1, eos) != ZST_EOF) FAIL("second EOS should return ZST_EOF");
    zst_buffer_unref(eos);
    zst_element_destroy(el);
    PASS();
}

static void
test_xvfb_gl_smoke(void)
{
    zst_element_t* el = make_glcompsink();
    if (!el) FAIL("factory make failed");
    zst_element_set_property_uint(el, "canvas-width", 320);
    zst_element_set_property_uint(el, "canvas-height", 240);
    zst_element_set_property_string(el, "background-color", "#000000ff");
    zst_element_set_property_uint(el, "input-count", 2);
    zst_element_set_property_int(el, "sink_0::x", 0);
    zst_element_set_property_int(el, "sink_0::y", 0);
    zst_element_set_property_uint(el, "sink_0::width", 160);
    zst_element_set_property_uint(el, "sink_0::height", 240);
    zst_element_set_property_int(el, "sink_1::x", 160);
    zst_element_set_property_int(el, "sink_1::y", 0);
    zst_element_set_property_uint(el, "sink_1::width", 160);
    zst_element_set_property_uint(el, "sink_1::height", 240);

    if (zst_element_set_state(el, ZST_STATE_READY) != ZST_OK) FAIL("open failed");
    if (zst_element_set_state(el, ZST_STATE_PLAYING) != ZST_OK) FAIL("start failed");

    zst_buffer_t* a = make_yuv420p_buffer(160, 120, 128, 128, 128);
    zst_buffer_t* b = make_rgb_buffer(160, 120, 255, 0, 0);
    if (!a || !b) FAIL("buffer allocation failed");
    zst_pad_t* p0 = zst_element_get_pad(el, "sink_0");
    zst_pad_t* p1 = zst_element_get_pad(el, "sink_1");
    if (p0->push(p0, a) != ZST_OK) FAIL("gl push sink_0 failed");
    if (p1->push(p1, b) != ZST_OK) FAIL("gl push sink_1 failed");
    zst_buffer_unref(a);
    zst_buffer_unref(b);

    uint64_t composites = 0;
    if (zst_element_get_property_uint(el, "composite-count", &composites) != ZST_OK || composites < 2) FAIL("composite-count too small");
    zst_element_set_state(el, ZST_STATE_NULL);
    zst_element_destroy(el);
    PASS();
}

int main(void)
{
    printf("=== glcompsink element tests ===\n\n");
    zst_result_t reg = zst_register_builtin_elements();
    if (reg != ZST_OK) {
        fprintf(stderr, "Failed to register builtin elements\n");
        return 1;
    }

    TEST("Factory creation");                 test_factory_create();
    TEST("Properties and dynamic pads");      test_properties_and_dynamic_pads();
    TEST("Request pad API");                  test_request_pad_api();
    TEST("Null-mode multi-input processing"); test_null_mode_multi_input();
    TEST("EOS per pad");                      test_eos_per_pad();
    TEST("Xvfb GL smoke");                    test_xvfb_gl_smoke();

    printf("\n=== Results: %d / %d passed ===\n", g_tests_passed, g_tests_run);
    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
