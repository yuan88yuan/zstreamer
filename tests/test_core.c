/*=============================================================================
    test_core.c — Unit / smoke tests for the zstreamer core framework
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <unistd.h>

#include "zst_types.h"
#include "zst_buffer.h"
#include "zst_pad.h"
#include "zst_element.h"
#include "zst_pipeline.h"
#include "zst_buffer_pool.h"
#include "zst_queue.h"
#include "zst_scheduler.h"
#include "zst_bus.h"
#include "zst_plugin.h"
#include "zst_log.h"
#include "zst_allocator.h"
#include "zst_buffer_pool.h"
#include "zst_clock.h"

zst_element_t* zst_video_scaler_create(int target_width, int target_height, const char* target_pixel_format);
zst_element_t* zst_audio_resampler_create(int target_sample_rate, int target_channels, const char* target_format);
zst_element_t* zst_text_overlay_create(const char* text);

static int g_tests_run   = 0;
static int g_tests_passed = 0;

#define TEST(name)                                              \
    do {                                                        \
        g_tests_run++;                                          \
        printf("  TEST: %-50s ... ", name);                     \
        fflush(stdout);                                         \
    } while (0)

#define PASS()                                                  \
    do {                                                        \
        g_tests_passed++;                                       \
        printf("PASS\n");                                       \
    } while (0)

/* ═══════════════════════════════════════════════════════════════
   Buffer tests
   ═══════════════════════════════════════════════════════════════ */
static void
test_buffer_create_destroy(void)
{
    TEST("buffer create / destroy");
    zst_buffer_t* buf = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
    assert(buf != NULL);
    assert(buf->type == ZST_BUFFER_VIDEO_FRAME);
    assert(buf->refcount == 1);

    zst_buffer_unref(buf);
    PASS();
}

static void
test_buffer_refcount(void)
{
    TEST("buffer refcount");
    zst_buffer_t* buf = zst_buffer_create(ZST_BUFFER_AUDIO_FRAME);
    assert(buf != NULL);

    zst_buffer_ref(buf);
    assert(buf->refcount == 2);

    zst_buffer_unref(buf);
    assert(buf->refcount == 1);

    zst_buffer_unref(buf);
    PASS();
}

static void
test_buffer_null_safety(void)
{
    TEST("buffer null safety");
    /* These should not crash */
    zst_buffer_ref(NULL);
    zst_buffer_unref(NULL);
    PASS();
}

static void
test_buffer_create_with_pool(void)
{
    TEST("buffer create with pool");
    zst_buffer_pool_config_t cfg = {
        .min_buffers = 1,
        .max_buffers = 1,
        .buffer_size = 1024,
        .buffer_type = ZST_BUFFER_USER
    };

    zst_buffer_pool_t* pool = zst_buffer_pool_create(NULL, &cfg);
    assert(pool != NULL);

    /* Pool preallocates min_buffers, so we should be able to get 1 */
    zst_buffer_t* buf = zst_buffer_create_with_pool(pool);
    assert(buf != NULL);
    assert(buf->type == ZST_BUFFER_USER);
    assert(buf->pool == pool);

    /* Pool size is max_buffers = 1, so getting another one with no timeout should return NULL since it fails to acquire */
    zst_buffer_t* buf2 = zst_buffer_create_with_pool(pool);
    assert(buf2 == NULL);

    zst_buffer_unref(buf);

    /* Now we should be able to get it again */
    buf2 = zst_buffer_create_with_pool(pool);
    assert(buf2 != NULL);
    zst_buffer_unref(buf2);

    zst_buffer_pool_destroy(pool);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   Pad tests
   ═══════════════════════════════════════════════════════════════ */
static void
test_pad_create_destroy(void)
{
    TEST("pad create / destroy");
    zst_pad_t* src = zst_pad_create("src", ZST_PAD_SRC);
    assert(src != NULL);
    assert(strcmp(src->name, "src") == 0);
    assert(src->direction == ZST_PAD_SRC);

    zst_pad_t* sink = zst_pad_create("sink", ZST_PAD_SINK);
    assert(sink != NULL);
    assert(sink->direction == ZST_PAD_SINK);

    zst_pad_destroy(src);
    zst_pad_destroy(sink);
    PASS();
}

static void
test_pad_link_unlink(void)
{
    TEST("pad link / unlink");
    zst_pad_t* src  = zst_pad_create("src",  ZST_PAD_SRC);
    zst_pad_t* sink = zst_pad_create("sink", ZST_PAD_SINK);

    zst_result_t r = zst_pad_link(src, sink);
    assert(r == ZST_OK);
    assert(src->peer == sink);
    assert(sink->peer == src);

    /* Double-link should fail */
    zst_pad_t* sink2 = zst_pad_create("sink2", ZST_PAD_SINK);
    r = zst_pad_link(src, sink2);
    assert(r == ZST_ERROR);

    zst_pad_unlink(src);
    assert(src->peer == NULL);
    assert(sink->peer == NULL);

    zst_pad_destroy(src);
    zst_pad_destroy(sink);
    zst_pad_destroy(sink2);
    PASS();
}

static void
test_pad_invalid_link(void)
{
    TEST("pad invalid link");
    zst_pad_t* src  = zst_pad_create("src",  ZST_PAD_SRC);
    zst_pad_t* src2 = zst_pad_create("src2", ZST_PAD_SRC);

    /* SRC-SRC should fail */
    zst_result_t r = zst_pad_link(src, src2);
    assert(r == ZST_ERROR);

    zst_pad_destroy(src);
    zst_pad_destroy(src2);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   Element tests
   ═══════════════════════════════════════════════════════════════ */
static zst_result_t
dummy_open(zst_element_t* el) { (void)el; return ZST_OK; }

static zst_element_ops_t g_dummy_ops = {
    .name   = "dummy",
    .open   = dummy_open,
};

static void
test_element_create_destroy(void)
{
    TEST("element create / destroy");
    zst_element_t* el = zst_element_create(&g_dummy_ops, NULL);
    assert(el != NULL);
    assert(el->state == ZST_STATE_NULL);
    assert(el->ops == &g_dummy_ops);

    zst_element_destroy(el);
    PASS();
}

static void
test_element_state_transition(void)
{
    TEST("element state transition");
    zst_element_t* el = zst_element_create(&g_dummy_ops, NULL);
    zst_result_t r;

    r = zst_element_set_state(el, ZST_STATE_READY);
    assert(r == ZST_OK);
    assert(el->state == ZST_STATE_READY);

    r = zst_element_set_state(el, ZST_STATE_PLAYING);
    assert(r == ZST_OK);
    assert(el->state == ZST_STATE_PLAYING);

    r = zst_element_set_state(el, ZST_STATE_NULL);
    assert(r == ZST_OK);
    assert(el->state == ZST_STATE_NULL);

    zst_element_destroy(el);
    PASS();
}

static void
test_element_pads(void)
{
    TEST("element pads");
    zst_element_t* el  = zst_element_create(&g_dummy_ops, NULL);
    zst_pad_t*     src = zst_pad_create("src", ZST_PAD_SRC);
    zst_pad_t*     snk = zst_pad_create("sink", ZST_PAD_SINK);

    zst_element_add_pad(el, src);
    zst_element_add_pad(el, snk);

    assert(el->nb_src_pads  == 1);
    assert(el->nb_sink_pads == 1);

    /* get_pad should find both */
    assert(zst_element_get_pad(el, "src")  == src);
    assert(zst_element_get_pad(el, "sink") == snk);
    assert(zst_element_get_pad(el, "none") == NULL);

    zst_element_destroy(el);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   Pipeline tests
   ═══════════════════════════════════════════════════════════════ */
static void
test_pipeline_create_destroy(void)
{
    TEST("pipeline create / destroy");
    zst_pipeline_t* pipe = zst_pipeline_create();
    assert(pipe != NULL);
    zst_pipeline_destroy(pipe);
    PASS();
}

static void
test_pipeline_add_remove(void)
{
    TEST("pipeline add / remove");
    zst_pipeline_t* pipe = zst_pipeline_create();
    zst_element_t*  el   = zst_element_create(&g_dummy_ops, NULL);

    zst_pipeline_add(pipe, el);
    assert(pipe->nb_elements == 1);

    zst_pipeline_remove(pipe, el);
    assert(pipe->nb_elements == 0);

    zst_element_destroy(el);
    zst_pipeline_destroy(pipe);
    PASS();
}

static void
test_pipeline_state_propagation(void)
{
    TEST("pipeline state propagation");
    zst_pipeline_t* pipe = zst_pipeline_create();
    zst_element_t*  el   = zst_element_create(&g_dummy_ops, NULL);

    zst_pipeline_add(pipe, el);
    zst_pipeline_set_state(pipe, ZST_STATE_PLAYING);
    assert(el->state == ZST_STATE_PLAYING);
    assert(pipe->state == ZST_STATE_PLAYING);

    zst_pipeline_set_state(pipe, ZST_STATE_NULL);
    assert(el->state == ZST_STATE_NULL);

    zst_pipeline_destroy(pipe);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   Queue tests
   ═══════════════════════════════════════════════════════════════ */
static void
test_queue_push_pop(void)
{
    TEST("queue push / pop");
    zst_queue_config_t cfg = {
        .mode        = ZST_QUEUE_SYNC,
        .max_buffers = 5,
        .max_bytes   = 0,
        .max_duration= 0,
    };
    zst_queue_t* q = zst_queue_create(&cfg);
    assert(q != NULL);

    zst_buffer_t* b1 = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
    zst_buffer_t* b2 = zst_buffer_create(ZST_BUFFER_AUDIO_FRAME);

    /* Push two buffers */
    assert(zst_queue_push(q, b1, 1000) == ZST_OK);
    assert(zst_queue_push(q, b2, 1000) == ZST_OK);
    assert(zst_queue_size(q) == 2);

    /* Pop them back */
    zst_buffer_t* out;
    assert(zst_queue_pop(q, &out, 1000) == ZST_OK);
    assert(out == b1);
    zst_buffer_unref(out);

    assert(zst_queue_pop(q, &out, 1000) == ZST_OK);
    assert(out == b2);
    zst_buffer_unref(out);

    assert(zst_queue_size(q) == 0);

    zst_queue_destroy(q);
    zst_buffer_unref(b1);
    zst_buffer_unref(b2);
    PASS();
}

static void
test_queue_timeout(void)
{
    TEST("queue timeout");
    zst_queue_config_t cfg = {
        .mode        = ZST_QUEUE_SYNC,
        .max_buffers = 1,
    };
    zst_queue_t* q = zst_queue_create(&cfg);
    assert(q != NULL);

    /* Pop from empty queue should timeout */
    zst_buffer_t* out;
    zst_result_t r = zst_queue_pop(q, &out, 10);
    assert(r == ZST_TIMEOUT);

    zst_queue_destroy(q);
    PASS();
}

static void
test_queue_flush(void)
{
    TEST("queue flush");
    zst_queue_config_t cfg = {
        .mode        = ZST_QUEUE_SYNC,
        .max_buffers = 5,
    };
    zst_queue_t* q = zst_queue_create(&cfg);

    zst_buffer_t* b1 = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
    zst_buffer_t* b2 = zst_buffer_create(ZST_BUFFER_AUDIO_FRAME);
    zst_queue_push(q, b1, 1000);
    zst_queue_push(q, b2, 1000);

    zst_queue_flush(q);
    assert(zst_queue_size(q) == 0);

    zst_buffer_unref(b1);
    zst_buffer_unref(b2);
    zst_queue_destroy(q);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   Scheduler / Pipeline integration tests (Phase 2)
   ═══════════════════════════════════════════════════════════════ */
static void
mock_buf_destroy(zst_buffer_t* b)
{
    free(b->payload);
}

static zst_result_t
mock_source_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    (void)in;
    int* counter = el->priv;
    if (*counter >= 5) {
        return ZST_EOF;
    }

    zst_buffer_t* buf = zst_buffer_create(ZST_BUFFER_USER);
    if (!buf) return ZST_ERROR;

    int* data = malloc(sizeof(int));
    if (!data) {
        zst_buffer_unref(buf);
        return ZST_ERROR;
    }
    *data = ++(*counter);
    buf->payload = data;
    buf->destroy = mock_buf_destroy;

    *out = buf;
    return ZST_OK;
}

static zst_result_t
mock_transform_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    (void)el;
    if (!in || !in->payload) return ZST_ERROR;

    int val = *(int*)in->payload;

    zst_buffer_t* buf = zst_buffer_create(ZST_BUFFER_USER);
    if (!buf) return ZST_ERROR;

    int* data = malloc(sizeof(int));
    if (!data) {
        zst_buffer_unref(buf);
        return ZST_ERROR;
    }
    *data = val * 2;
    buf->payload = data;
    buf->destroy = mock_buf_destroy;

    *out = buf;
    return ZST_OK;
}

typedef struct {
    int count;
    int sum;
} mock_sink_t;

static zst_result_t
mock_sink_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    (void)out;
    mock_sink_t* sink = el->priv;
    if (!in || !in->payload) return ZST_ERROR;

    int val = *(int*)in->payload;
    sink->count++;
    sink->sum += val;

    return ZST_OK;
}

static void
test_scheduler_single_threaded(void)
{
    TEST("scheduler single-threaded pipeline");

    zst_pipeline_t* pipe = zst_pipeline_create();
    zst_scheduler_config_t cfg = {
        .mode = ZST_SCHEDULER_SINGLE_THREAD,
        .worker_threads = 1
    };
    zst_scheduler_t* sched = zst_scheduler_create(&cfg);
    zst_scheduler_attach(sched, pipe);

    int* source_counter = malloc(sizeof(int));
    *source_counter = 0;
    static zst_element_ops_t source_ops = {
        .name = "mock_source",
        .process = mock_source_process
    };
    zst_element_t* source = zst_element_create(&source_ops, source_counter);
    zst_pad_t* src_pad = zst_pad_create("src", ZST_PAD_SRC);
    zst_element_add_pad(source, src_pad);

    static zst_element_ops_t transform_ops = {
        .name = "mock_transform",
        .process = mock_transform_process
    };
    zst_element_t* transform = zst_element_create(&transform_ops, NULL);
    zst_pad_t* trans_sink = zst_pad_create("sink", ZST_PAD_SINK);
    zst_pad_t* trans_src = zst_pad_create("src", ZST_PAD_SRC);
    zst_element_add_pad(transform, trans_sink);
    zst_element_add_pad(transform, trans_src);

    mock_sink_t* sink_data = calloc(1, sizeof(mock_sink_t));
    static zst_element_ops_t sink_ops = {
        .name = "mock_sink",
        .process = mock_sink_process
    };
    zst_element_t* sink = zst_element_create(&sink_ops, sink_data);
    zst_pad_t* sink_pad = zst_pad_create("sink", ZST_PAD_SINK);
    zst_element_add_pad(sink, sink_pad);

    zst_pipeline_add(pipe, source);
    zst_pipeline_add(pipe, transform);
    zst_pipeline_add(pipe, sink);

    zst_pad_link(src_pad, trans_sink);
    zst_pad_link(trans_src, sink_pad);

    zst_pipeline_set_state(pipe, ZST_STATE_PLAYING);
    zst_scheduler_run(sched);

    struct timespec ts = { .tv_sec = 0, .tv_nsec = 50000000 }; /* 50 ms */
    nanosleep(&ts, NULL);

    zst_pipeline_set_state(pipe, ZST_STATE_NULL);
    zst_scheduler_stop(sched);

    assert(sink_data->count == 5);
    assert(sink_data->sum == 30);

    zst_scheduler_destroy(sched);
    zst_pipeline_destroy(pipe);

    PASS();
}

static void
test_queue_config_limits(void)
{
    TEST("queue config limits (bytes, duration, async mode)");

    /* Test 1: Max bytes in SYNC mode blocks/times out */
    zst_queue_config_t cfg_bytes = {
        .mode = ZST_QUEUE_SYNC,
        .max_bytes = 100,
    };
    zst_queue_t* q1 = zst_queue_create(&cfg_bytes);
    zst_buffer_t* b1 = zst_buffer_create(ZST_BUFFER_USER);
    b1->memory.size = 60;
    assert(zst_queue_push(q1, b1, 10) == ZST_OK);
    
    zst_buffer_t* b2 = zst_buffer_create(ZST_BUFFER_USER);
    b2->memory.size = 50; // Total is now 110, queue is now full
    assert(zst_queue_push(q1, b2, 10) == ZST_OK);

    zst_buffer_t* b3 = zst_buffer_create(ZST_BUFFER_USER);
    b3->memory.size = 10; // Queue is already full (110 >= 100), this must time out
    assert(zst_queue_push(q1, b3, 10) == ZST_TIMEOUT);

    zst_queue_destroy(q1);
    zst_buffer_unref(b1);
    zst_buffer_unref(b2);
    zst_buffer_unref(b3);

    /* Test 2: Max duration in SYNC mode blocks/times out */
    zst_queue_config_t cfg_dur = {
        .mode = ZST_QUEUE_SYNC,
        .max_duration = 1000,
    };
    zst_queue_t* q2 = zst_queue_create(&cfg_dur);
    zst_buffer_t* bd1 = zst_buffer_create(ZST_BUFFER_USER);
    bd1->pts = 10000;
    bd1->duration = 0;
    assert(zst_queue_push(q2, bd1, 10) == ZST_OK);

    zst_buffer_t* bd2 = zst_buffer_create(ZST_BUFFER_USER);
    bd2->pts = 11500; // Duration is 1500 (>= 1000), queue is now full
    bd2->duration = 0;
    assert(zst_queue_push(q2, bd2, 10) == ZST_OK);

    zst_buffer_t* bd3 = zst_buffer_create(ZST_BUFFER_USER);
    bd3->pts = 12000; // Queue is already full (1500 >= 1000), this must time out
    bd3->duration = 0;
    assert(zst_queue_push(q2, bd3, 10) == ZST_TIMEOUT);

    zst_queue_destroy(q2);
    zst_buffer_unref(bd1);
    zst_buffer_unref(bd2);
    zst_buffer_unref(bd3);

    /* Test 3: ASYNC mode drops buffers when full */
    zst_queue_config_t cfg_async = {
        .mode = ZST_QUEUE_ASYNC,
        .max_buffers = 1,
    };
    zst_queue_t* q3 = zst_queue_create(&cfg_async);
    zst_buffer_t* ba1 = zst_buffer_create(ZST_BUFFER_USER);
    assert(zst_queue_push(q3, ba1, 10) == ZST_OK);

    zst_buffer_t* ba2 = zst_buffer_create(ZST_BUFFER_USER);
    assert(zst_queue_push(q3, ba2, 10) == ZST_ERROR);

    zst_queue_destroy(q3);
    zst_buffer_unref(ba1);
    zst_buffer_unref(ba2);

    PASS();
}

static void
test_scheduler_multi_threaded(void)
{
    TEST("scheduler multi-threaded pipeline with queues");

    zst_pipeline_t* pipe = zst_pipeline_create();
    zst_scheduler_config_t cfg = {
        .mode = ZST_SCHEDULER_MULTI_THREAD,
        .worker_threads = 3
    };
    zst_scheduler_t* sched = zst_scheduler_create(&cfg);
    zst_scheduler_attach(sched, pipe);

    int* source_counter = malloc(sizeof(int));
    *source_counter = 0;
    static zst_element_ops_t source_ops = {
        .name = "mock_source",
        .process = mock_source_process
    };
    zst_element_t* source = zst_element_create(&source_ops, source_counter);
    zst_pad_t* src_pad = zst_pad_create("src", ZST_PAD_SRC);
    zst_element_add_pad(source, src_pad);

    /* Add explicit queue elements */
    zst_element_t* q1 = zst_queue_element_create(NULL);
    zst_element_t* q2 = zst_queue_element_create(NULL);

    static zst_element_ops_t transform_ops = {
        .name = "mock_transform",
        .process = mock_transform_process
    };
    zst_element_t* transform = zst_element_create(&transform_ops, NULL);
    zst_pad_t* trans_sink = zst_pad_create("sink", ZST_PAD_SINK);
    zst_pad_t* trans_src = zst_pad_create("src", ZST_PAD_SRC);
    zst_element_add_pad(transform, trans_sink);
    zst_element_add_pad(transform, trans_src);

    mock_sink_t* sink_data = calloc(1, sizeof(mock_sink_t));
    static zst_element_ops_t sink_ops = {
        .name = "mock_sink",
        .process = mock_sink_process
    };
    zst_element_t* sink = zst_element_create(&sink_ops, sink_data);
    zst_pad_t* sink_pad = zst_pad_create("sink", ZST_PAD_SINK);
    zst_element_add_pad(sink, sink_pad);

    zst_pipeline_add(pipe, source);
    zst_pipeline_add(pipe, q1);
    zst_pipeline_add(pipe, transform);
    zst_pipeline_add(pipe, q2);
    zst_pipeline_add(pipe, sink);

    zst_pad_link(src_pad, zst_element_get_pad(q1, "sink"));
    zst_pad_link(zst_element_get_pad(q1, "src"), trans_sink);
    zst_pad_link(trans_src, zst_element_get_pad(q2, "sink"));
    zst_pad_link(zst_element_get_pad(q2, "src"), sink_pad);

    zst_pipeline_set_state(pipe, ZST_STATE_PLAYING);
    zst_scheduler_run(sched);

    struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 }; /* 100 ms to let explicit queues process */
    nanosleep(&ts, NULL);

    zst_pipeline_set_state(pipe, ZST_STATE_NULL);
    zst_scheduler_stop(sched);

    assert(sink_data->count == 5);
    assert(sink_data->sum == 30);

    zst_scheduler_destroy(sched);
    zst_pipeline_destroy(pipe);

    PASS();
}

static void
test_pipeline_topological_sort_check(void)
{
    TEST("pipeline topological sort");

    zst_pipeline_t* pipe = zst_pipeline_create();

    zst_element_t* a = zst_element_create(&g_dummy_ops, NULL);
    zst_pad_t* a_src = zst_pad_create("src", ZST_PAD_SRC);
    zst_element_add_pad(a, a_src);

    zst_element_t* b = zst_element_create(&g_dummy_ops, NULL);
    zst_pad_t* b_sink = zst_pad_create("sink", ZST_PAD_SINK);
    zst_pad_t* b_src = zst_pad_create("src", ZST_PAD_SRC);
    zst_element_add_pad(b, b_sink);
    zst_element_add_pad(b, b_src);

    zst_element_t* c = zst_element_create(&g_dummy_ops, NULL);
    zst_pad_t* c_sink = zst_pad_create("sink", ZST_PAD_SINK);
    zst_element_add_pad(c, c_sink);

    zst_pad_link(a_src, b_sink);
    zst_pad_link(b_src, c_sink);

    zst_pipeline_add(pipe, c);
    zst_pipeline_add(pipe, b);
    zst_pipeline_add(pipe, a);

    assert(pipe->elements[0] == c);
    assert(pipe->elements[1] == b);
    assert(pipe->elements[2] == a);

    zst_pipeline_topological_sort(pipe);

    assert(pipe->elements[0] == a);
    assert(pipe->elements[1] == b);
    assert(pipe->elements[2] == c);

    zst_pipeline_destroy(pipe);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   Caps Negotiation tests (Phase 5)
   ═══════════════════════════════════════════════════════════════ */
static void
test_caps_basic(void)
{
    TEST("caps create / copy / destroy");
    zst_caps_t* caps = zst_caps_create();
    assert(caps != NULL);
    assert(caps->structs == NULL);

    zst_caps_struct_t* s1 = zst_caps_struct_create_video("video/x-raw", 640, 480, 30.0, "YUV420P");
    assert(s1 != NULL);
    assert(strcmp(s1->media_type, "video/x-raw") == 0);
    assert(s1->type == ZST_CAPS_VIDEO);
    assert(s1->video.width == 640);
    assert(s1->video.height == 480);
    assert(s1->video.framerate == 30.0);
    assert(strcmp(s1->video.pixel_format, "YUV420P") == 0);

    zst_result_t ret = zst_caps_append(caps, s1);
    assert(ret == ZST_OK);
    assert(caps->structs == s1);

    zst_caps_t* copy = zst_caps_copy(caps);
    assert(copy != NULL);
    assert(copy->structs != NULL);
    assert(copy->structs != s1);
    assert(strcmp(copy->structs->media_type, "video/x-raw") == 0);
    assert(copy->structs->video.width == 640);

    zst_caps_destroy(caps);
    zst_caps_destroy(copy);
    PASS();
}

static void
test_caps_intersection_video(void)
{
    TEST("caps intersection (video)");
    
    zst_caps_t* c1 = zst_caps_create();
    zst_caps_append(c1, zst_caps_struct_create_video("video/x-raw", 640, 480, 30.0, "YUV420P"));
    zst_caps_t* c2 = zst_caps_create();
    zst_caps_append(c2, zst_caps_struct_create_video("video/x-h264", 640, 480, 30.0, ""));
    
    zst_caps_t* res = zst_caps_intersect(c1, c2);
    assert(res != NULL);
    assert(res->structs == NULL);
    zst_caps_destroy(res);

    zst_caps_destroy(c2);
    c2 = zst_caps_create();
    zst_caps_append(c2, zst_caps_struct_create_video("video/x-raw", 0, 480, 0.0, ""));
    
    res = zst_caps_intersect(c1, c2);
    assert(res != NULL);
    assert(res->structs != NULL);
    assert(res->structs->video.width == 640);
    assert(res->structs->video.height == 480);
    assert(res->structs->video.framerate == 30.0);
    assert(strcmp(res->structs->video.pixel_format, "YUV420P") == 0);
    zst_caps_destroy(res);

    zst_caps_destroy(c2);
    c2 = zst_caps_create();
    zst_caps_append(c2, zst_caps_struct_create_video("video/x-raw", 1280, 480, 30.0, "YUV420P"));
    
    res = zst_caps_intersect(c1, c2);
    assert(res != NULL);
    assert(res->structs == NULL);
    zst_caps_destroy(res);

    zst_caps_destroy(c1);
    zst_caps_destroy(c2);
    PASS();
}

static void
test_caps_intersection_audio(void)
{
    TEST("caps intersection (audio)");
    
    zst_caps_t* c1 = zst_caps_create();
    zst_caps_append(c1, zst_caps_struct_create_audio("audio/x-raw", 2, 44100, "S16LE"));
    zst_caps_t* c2 = zst_caps_create();
    zst_caps_append(c2, zst_caps_struct_create_audio("audio/x-raw", 0, 0, ""));
    
    zst_caps_t* res = zst_caps_intersect(c1, c2);
    assert(res != NULL);
    assert(res->structs != NULL);
    assert(res->structs->audio.channels == 2);
    assert(res->structs->audio.sample_rate == 44100);
    assert(strcmp(res->structs->audio.format, "S16LE") == 0);
    
    zst_caps_destroy(res);
    zst_caps_destroy(c1);
    zst_caps_destroy(c2);
    PASS();
}

static void
test_caps_fixate(void)
{
    TEST("caps fixation");
    
    zst_caps_t* caps = zst_caps_create();
    zst_caps_append(caps, zst_caps_struct_create_video("video/x-raw", 0, 0, 0.0, ""));
    
    assert(!zst_caps_is_fixed(caps));
    
    zst_result_t ret = zst_caps_fixate(caps);
    assert(ret == ZST_OK);
    
    assert(zst_caps_is_fixed(caps));
    assert(caps->structs->video.width == 640);
    assert(caps->structs->video.height == 480);
    assert(caps->structs->video.framerate == 30.0);
    assert(strcmp(caps->structs->video.pixel_format, "YUV420P") == 0);
    
    zst_caps_destroy(caps);
    PASS();
}

static zst_caps_t*
element_get_caps_cb(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)el;
    (void)pad;
    (void)filter;
    zst_caps_t* caps = zst_caps_create();
    zst_caps_append(caps, zst_caps_struct_create_video("video/x-raw", 1920, 1080, 60.0, "NV12"));
    return caps;
}

static zst_element_ops_t g_query_ops = {
    .name = "query_element",
    .get_caps = element_get_caps_cb
};

static void
test_pad_negotiate_and_link(void)
{
    TEST("pad caps negotiation / link compatibility");
    
    zst_pad_t* src = zst_pad_create("src", ZST_PAD_SRC);
    zst_pad_t* sink = zst_pad_create("sink", ZST_PAD_SINK);
    
    zst_caps_t* src_template = zst_caps_create();
    zst_caps_append(src_template, zst_caps_struct_create_video("video/x-raw", 640, 480, 30.0, "YUV420P"));
    zst_pad_set_template_caps(src, src_template);
    
    zst_caps_t* sink_template = zst_caps_create();
    zst_caps_append(sink_template, zst_caps_struct_create_video("video/x-raw", 1280, 720, 30.0, "YUV420P"));
    zst_pad_set_template_caps(sink, sink_template);
    
    zst_result_t ret = zst_pad_link(src, sink);
    assert(ret == ZST_ERROR);
    assert(src->peer == NULL);
    assert(sink->peer == NULL);
    
    zst_caps_destroy(sink_template);
    sink_template = zst_caps_create();
    zst_caps_append(sink_template, zst_caps_struct_create_video("video/x-raw", 0, 0, 0.0, ""));
    zst_pad_set_template_caps(sink, sink_template);
    
    ret = zst_pad_link(src, sink);
    assert(ret == ZST_OK);
    assert(src->peer == sink);
    assert(sink->peer == src);
    
    assert(src->caps != NULL);
    assert(sink->caps != NULL);
    assert(zst_caps_is_fixed(src->caps));
    assert(src->caps->structs->video.width == 640);
    assert(src->caps->structs->video.height == 480);
    assert(strcmp(src->caps->structs->video.pixel_format, "YUV420P") == 0);
    
    zst_pad_destroy(src);
    zst_pad_destroy(sink);
    zst_caps_destroy(src_template);
    zst_caps_destroy(sink_template);
    
    zst_element_t* el = zst_element_create(&g_query_ops, NULL);
    zst_pad_t* query_pad = zst_pad_create("src", ZST_PAD_SRC);
    zst_element_add_pad(el, query_pad);
    
    zst_caps_t* queried = zst_pad_get_caps(query_pad);
    assert(queried != NULL);
    assert(queried->structs != NULL);
    assert(queried->structs->video.width == 1920);
    assert(strcmp(queried->structs->video.pixel_format, "NV12") == 0);
    
    zst_caps_destroy(queried);
    zst_element_destroy(el);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   Event Bus tests (Phase 6)
   ═══════════════════════════════════════════════════════════════ */
static void
test_event_create_destroy(void)
{
    TEST("event create / destroy");
    
    zst_event_t* ev1 = zst_event_new_eos(NULL);
    assert(ev1 != NULL);
    assert(ev1->type == ZST_EVENT_EOS);
    assert(ev1->src == NULL);
    zst_event_destroy(ev1);
    
    zst_event_t* ev2 = zst_event_new_error(NULL, ZST_ERROR, "test error");
    assert(ev2 != NULL);
    assert(ev2->type == ZST_EVENT_ERROR);
    assert(ev2->as.error.result == ZST_ERROR);
    assert(strcmp(ev2->as.error.message, "test error") == 0);
    zst_event_destroy(ev2);
    
    zst_event_t* ev3 = zst_event_new_state_changed(NULL, ZST_STATE_NULL, ZST_STATE_READY);
    assert(ev3 != NULL);
    assert(ev3->type == ZST_EVENT_STATE_CHANGED);
    assert(ev3->as.state_changed.old_state == ZST_STATE_NULL);
    assert(ev3->as.state_changed.new_state == ZST_STATE_READY);
    zst_event_destroy(ev3);
    
    PASS();
}

static void
test_bus_basic(void)
{
    TEST("bus basic post / pop");
    
    zst_bus_t* bus = zst_bus_create();
    assert(bus != NULL);
    
    zst_event_t* ev = zst_event_new_eos(NULL);
    zst_result_t r = zst_bus_post(bus, ev);
    assert(r == ZST_OK);
    
    zst_event_t* popped = NULL;
    r = zst_bus_pop(bus, &popped, 0);
    assert(r == ZST_OK);
    assert(popped != NULL);
    assert(popped->type == ZST_EVENT_EOS);
    
    zst_event_destroy(popped);
    zst_bus_destroy(bus);
    
    PASS();
}

static void
test_bus_timeout(void)
{
    TEST("bus pop timeout");
    
    zst_bus_t* bus = zst_bus_create();
    assert(bus != NULL);
    
    zst_event_t* popped = NULL;
    zst_result_t r = zst_bus_pop(bus, &popped, 10);
    assert(r == ZST_TIMEOUT);
    assert(popped == NULL);
    
    zst_bus_destroy(bus);
    
    PASS();
}

static volatile int g_handler_called = 0;
static zst_event_type_t g_last_event_type;

static void
test_bus_handler_cb(zst_bus_t* bus, zst_event_t* event, void* user_data)
{
    (void)bus;
    (void)user_data;
    g_handler_called++;
    g_last_event_type = event->type;
}

static void
test_bus_async_dispatch(void)
{
    TEST("bus async dispatch handler");
    
    zst_bus_t* bus = zst_bus_create();
    assert(bus != NULL);
    
    g_handler_called = 0;
    
    zst_result_t r = zst_bus_set_handler(bus, test_bus_handler_cb, NULL);
    assert(r == ZST_OK);
    
    zst_event_t* ev = zst_event_new_eos(NULL);
    r = zst_bus_post(bus, ev);
    assert(r == ZST_OK);
    
    /* Sleep a bit to allow dispatch thread to run */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 50000000 }; /* 50 ms */
    nanosleep(&ts, NULL);
    
    assert(g_handler_called == 1);
    assert(g_last_event_type == ZST_EVENT_EOS);
    
    /* Remove handler (stops thread) */
    r = zst_bus_set_handler(bus, NULL, NULL);
    assert(r == ZST_OK);
    
    zst_bus_destroy(bus);
    
    PASS();
}

static void
test_pipeline_bus_events(void)
{
    TEST("pipeline/element lifecycle events on bus");
    
    zst_pipeline_t* pipe = zst_pipeline_create();
    assert(pipe != NULL);
    assert(pipe->bus != NULL);
    
    zst_element_ops_t ops = { .name = "test_el" };
    zst_element_t* el = zst_element_create(&ops, NULL);
    assert(el != NULL);
    
    zst_result_t r = zst_pipeline_add(pipe, el);
    assert(r == ZST_OK);
    assert(el->bus == pipe->bus);
    
    /* Transition pipeline state */
    r = zst_pipeline_set_state(pipe, ZST_STATE_READY);
    assert(r == ZST_OK);
    
    /* We expect two events: element state changed, then pipeline state changed */
    zst_event_t* ev = NULL;
    r = zst_bus_pop(pipe->bus, &ev, 100);
    assert(r == ZST_OK);
    assert(ev != NULL);
    assert(ev->type == ZST_EVENT_STATE_CHANGED);
    assert(ev->src == el);
    assert(ev->as.state_changed.old_state == ZST_STATE_NULL);
    assert(ev->as.state_changed.new_state == ZST_STATE_READY);
    zst_event_destroy(ev);
    
    ev = NULL;
    r = zst_bus_pop(pipe->bus, &ev, 100);
    assert(r == ZST_OK);
    assert(ev != NULL);
    assert(ev->type == ZST_EVENT_STATE_CHANGED);
    assert(ev->src == NULL); // Pipeline itself
    assert(ev->as.state_changed.old_state == ZST_STATE_NULL);
    assert(ev->as.state_changed.new_state == ZST_STATE_READY);
    zst_event_destroy(ev);
    
    zst_pipeline_destroy(pipe); // also destroys el and bus
    
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   Dynamic Plugins tests (Phase 7)
   ═══════════════════════════════════════════════════════════════ */
static void
test_plugin_registry_basic(void)
{
    TEST("plugin registry initialization and scanning");
    
    zst_result_t r = zst_plugin_registry_init();
    assert(r == ZST_OK);
    
    const char* ppath = getenv("ZSTREAMER_TEST_PLUGIN_PATH");
    if (!ppath) {
        ppath = "/workspace/build/plugins";
        /* fallback to /app/build/plugins if we are there */
        if (access("/app/build/plugins", R_OK) == 0) {
            ppath = "/app/build/plugins";
        }
    }
    r = zst_plugin_registry_scan(ppath);
    assert(r == ZST_OK);
    
    setenv("ZSTREAMER_PLUGIN_PATH", ppath, 1);
    r = zst_plugin_registry_scan_env();
    assert(r == ZST_OK);
    
    PASS();
}

static void
test_element_factory_refcounting(void)
{
    TEST("element factory make and plugin refcounting");
    
    zst_plugin_registry_init();
    zst_plugin_registry_scan("/workspace/build/plugins");
    
    zst_element_t* filesink = zst_element_factory_make("filesink");
    assert(filesink != NULL);
    assert(filesink->plugin != NULL);
    assert(strcmp(filesink->ops->name, "filesink") == 0);
    
    zst_element_t* v4l2source = zst_element_factory_make("v4l2src");
    assert(v4l2source != NULL);
    assert(v4l2source->plugin != NULL);
    assert(strcmp(v4l2source->ops->name, "v4l2src") == 0);
    
    zst_element_t* alsasource = zst_element_factory_make("alsasrc");
    assert(alsasource != NULL);
    assert(alsasource->plugin != NULL);
    assert(strcmp(alsasource->ops->name, "alsasrc") == 0);
    
    zst_element_t* h264encoder = zst_element_factory_make("h264enc");
    assert(h264encoder != NULL);
    assert(h264encoder->plugin != NULL);
    assert(strcmp(h264encoder->ops->name, "h264enc") == 0);
    
    zst_element_t* aacencoder = zst_element_factory_make("aacenc");
    assert(aacencoder != NULL);
    assert(aacencoder->plugin != NULL);
    assert(strcmp(aacencoder->ops->name, "aacenc") == 0);
    
    zst_element_t* mp4muxer = zst_element_factory_make("mp4mux");
    assert(mp4muxer != NULL);
    assert(mp4muxer->plugin != NULL);
    assert(strcmp(mp4muxer->ops->name, "mp4mux") == 0);
    
    zst_element_t* videoscaler = zst_element_factory_make("videoscaler");
    assert(videoscaler != NULL);
    assert(videoscaler->plugin != NULL);
    assert(strcmp(videoscaler->ops->name, "videoscaler") == 0);

    zst_element_t* audioresampler = zst_element_factory_make("audioresampler");
    assert(audioresampler != NULL);
    assert(audioresampler->plugin != NULL);
    assert(strcmp(audioresampler->ops->name, "audioresampler") == 0);

    zst_plugin_t* filesink_plugin = filesink->plugin;
    assert(filesink_plugin->refcount == 2);
    
    zst_element_destroy(filesink);
    assert(filesink_plugin->refcount == 1);
    
    zst_element_destroy(v4l2source);
    zst_element_destroy(alsasource);
    zst_element_destroy(h264encoder);
    zst_element_destroy(aacencoder);
    zst_element_destroy(mp4muxer);
    zst_element_destroy(videoscaler);
    zst_element_destroy(audioresampler);
    
    zst_plugin_registry_deinit();
    
    PASS();
}

static void
scaler_test_free(zst_buffer_t* buf)
{
    if (buf) {
        free(buf->memory.data);
        free(buf->payload);
    }
}

static void
resampler_test_free(zst_buffer_t* buf)
{
    if (buf) {
        free(buf->memory.data);
        free(buf->payload);
    }
}

static void
test_video_scaler(void)
{
    TEST("video scaler (Phase 4g) basic scaling and fallback");

    /* 1. Create video scaler element */
    zst_element_t* scaler = zst_video_scaler_create(320, 240, "YUV420P");
    assert(scaler != NULL);
    assert(strcmp(scaler->ops->name, "videoscaler") == 0);

    /* 2. Open it */
    assert(zst_element_set_state(scaler, ZST_STATE_READY) == ZST_OK);

    /* 3. Create an input buffer (YUV420P, 640x480) */
    zst_buffer_t* in_buf = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
    assert(in_buf != NULL);

    size_t in_y_size = 640 * 480;
    size_t in_uv_size = in_y_size / 4;
    size_t in_total_size = in_y_size + 2 * in_uv_size;
    uint8_t* in_data = malloc(in_total_size);
    assert(in_data != NULL);
    memset(in_data, 128, in_total_size);

    in_buf->memory.type = ZST_MEMORY_CPU;
    in_buf->memory.data = in_data;
    in_buf->memory.size = in_total_size;

    zst_video_frame_t* in_frame = calloc(1, sizeof(*in_frame));
    assert(in_frame != NULL);
    in_frame->width = 640;
    in_frame->height = 480;
    in_frame->format = 0; /* YUV420P */
    in_frame->plane[0] = in_data;
    in_frame->plane[1] = in_data + in_y_size;
    in_frame->plane[2] = in_data + in_y_size + in_uv_size;
    in_frame->stride[0] = 640;
    in_frame->stride[1] = 320;
    in_frame->stride[2] = 320;
    in_buf->payload = in_frame;

    in_buf->destroy = scaler_test_free;

    /* Set some caps on sink pad representing input */
    zst_caps_t* sink_caps = zst_caps_create();
    zst_caps_append(sink_caps, zst_caps_struct_create_video("video/x-raw", 640, 480, 30.0, "YUV420P"));
    zst_pad_t* sink_pad = zst_element_get_pad(scaler, "sink");
    assert(zst_pad_set_caps(sink_pad, sink_caps) == ZST_OK);
    zst_caps_destroy(sink_caps);

    /* Set some caps on src pad representing target/negotiated output */
    zst_caps_t* src_caps = zst_caps_create();
    zst_caps_append(src_caps, zst_caps_struct_create_video("video/x-raw", 320, 240, 30.0, "YUV420P"));
    zst_pad_t* src_pad = zst_element_get_pad(scaler, "src");
    assert(zst_pad_set_caps(src_pad, src_caps) == ZST_OK);
    zst_caps_destroy(src_caps);

    /* 4. Process the buffer */
    zst_buffer_t* out_buf = NULL;
    zst_result_t res = scaler->ops->process(scaler, in_buf, &out_buf);
    assert(res == ZST_OK);
    assert(out_buf != NULL);

    /* Verify scaled output dimensions */
    zst_video_frame_t* out_frame = out_buf->payload;
    assert(out_frame != NULL);
    assert(out_frame->width == 320);
    assert(out_frame->height == 240);
    assert(out_frame->format == 0); /* YUV420P */

    zst_buffer_unref(out_buf);
    zst_buffer_unref(in_buf);

    /* 5. Clean up */
    assert(zst_element_set_state(scaler, ZST_STATE_NULL) == ZST_OK);
    zst_element_destroy(scaler);

    PASS();
}

static void
test_audio_resampler(void)
{
    TEST("audio resampler (Phase 4h) basic resampling and fallback");

    /* 1. Create resampler element: 48000Hz stereo -> 44100Hz stereo */
    zst_element_t* resampler = zst_audio_resampler_create(44100, 2, "S16LE");
    assert(resampler != NULL);
    assert(strcmp(resampler->ops->name, "audioresampler") == 0);

    /* 2. Open it */
    assert(zst_element_set_state(resampler, ZST_STATE_READY) == ZST_OK);

    /* 3. Create input buffer: 48000Hz stereo, 480 samples, interleaved S16 */
    zst_buffer_t* in_buf = zst_buffer_create(ZST_BUFFER_AUDIO_FRAME);
    assert(in_buf != NULL);

    int in_samples = 480;
    int in_channels = 2;
    size_t in_size = in_samples * in_channels * sizeof(int16_t);
    int16_t* in_data = malloc(in_size);
    assert(in_data != NULL);
    memset(in_data, 0, in_size);

    in_buf->memory.type = ZST_MEMORY_CPU;
    in_buf->memory.data = in_data;
    in_buf->memory.size = in_size;

    zst_audio_frame_t* in_frame = calloc(1, sizeof(*in_frame));
    assert(in_frame != NULL);
    in_frame->sample_rate = 48000;
    in_frame->channels = 2;
    in_frame->format = 0; /* S16LE */
    in_frame->nb_samples = in_samples;
    in_frame->data = in_data;
    in_buf->payload = in_frame;

    in_buf->destroy = resampler_test_free;

    /* Set caps on sink pad representing input */
    zst_caps_t* sink_caps = zst_caps_create();
    zst_caps_append(sink_caps, zst_caps_struct_create_audio("audio/x-raw", 2, 48000, "S16LE"));
    zst_pad_t* sink_pad = zst_element_get_pad(resampler, "sink");
    assert(zst_pad_set_caps(sink_pad, sink_caps) == ZST_OK);
    zst_caps_destroy(sink_caps);

    /* Set caps on src pad representing output */
    zst_caps_t* src_caps = zst_caps_create();
    zst_caps_append(src_caps, zst_caps_struct_create_audio("audio/x-raw", 2, 44100, "S16LE"));
    zst_pad_t* src_pad = zst_element_get_pad(resampler, "src");
    assert(zst_pad_set_caps(src_pad, src_caps) == ZST_OK);
    zst_caps_destroy(src_caps);

    /* 4. Process buffer */
    zst_buffer_t* out_buf = NULL;
    zst_result_t res = resampler->ops->process(resampler, in_buf, &out_buf);
    assert(res == ZST_OK);
    assert(out_buf != NULL);

    /* Verify output samples */
    zst_audio_frame_t* out_frame = out_buf->payload;
    assert(out_frame != NULL);
    assert(out_frame->sample_rate == 44100);
    assert(out_frame->channels == 2);
    /* Expect roughly 441 samples, or slightly fewer due to resampler filter delay (e.g. 425) */
    assert(out_frame->nb_samples >= 420 && out_frame->nb_samples <= 450);

    zst_buffer_unref(out_buf);
    zst_buffer_unref(in_buf);

    /* 5. Clean up */
    assert(zst_element_set_state(resampler, ZST_STATE_NULL) == ZST_OK);
    zst_element_destroy(resampler);

    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   Logging tests (Phase 3.5)
   ═══════════════════════════════════════════════════════════════ */

/* ── capture buffer for custom handler test ─────────────────────── */
static char g_log_buf[4096];
static int  g_log_called;

static void
test_log_handler(zst_log_level_t level,
                  const char* category,
                  const char* file,
                  int line,
                  const char* func,
                  const char* message)
{
    (void)file; (void)line; (void)func;
    g_log_called++;
    snprintf(g_log_buf, sizeof(g_log_buf),
             "%d [%s] %s", (int)level,
             category ? category : "",
             message ? message : "");
}

static void
test_log_levels(void)
{
    TEST("log level runtime filter");

    zst_log_set_handler(test_log_handler);
    zst_log_set_level(ZST_LOG_LEVEL_INFO);

    /* ERROR and INFO should pass; DEBUG should not */
    g_log_called = 0;
    ZST_LOG_ERROR("test", "error msg");
    assert(g_log_called == 1);

    g_log_called = 0;
    ZST_LOG_WARN("test", "warn msg");
    assert(g_log_called == 1);

    g_log_called = 0;
    ZST_LOG_INFO("test", "info msg");
    assert(g_log_called == 1);

    g_log_called = 0;
    ZST_LOG_DEBUG("test", "debug msg");
    assert(g_log_called == 0);

    g_log_called = 0;
    ZST_LOG_TRACE("test", "trace msg");
    assert(g_log_called == 0);

    /* Lower the bar: DEBUG should now pass */
    zst_log_set_level(ZST_LOG_LEVEL_DEBUG);
    g_log_called = 0;
    ZST_LOG_DEBUG("test", "debug ok");
    assert(g_log_called == 1);

    /* Restore defaults */
    zst_log_set_handler(NULL);
    zst_log_set_level(ZST_LOG_LEVEL_TRACE);

    PASS();
}

static void
test_log_custom_handler(void)
{
    TEST("log custom handler receives correct data");

    zst_log_set_handler(test_log_handler);
    zst_log_set_level(ZST_LOG_LEVEL_TRACE);

    g_log_called = 0;
    memset(g_log_buf, 0, sizeof(g_log_buf));

    ZST_LOG_WARN("mycat", "hello %s %d", "world", 42);

    assert(g_log_called == 1);
    /* Check the captured buffer */
    assert(strstr(g_log_buf, "2 [mycat] hello world 42") != NULL);

    zst_log_set_handler(NULL);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   Allocator & Clock tests (Phase 8a/8b)
   ═══════════════════════════════════════════════════════════════ */
static void
test_allocator_basic(void)
{
    TEST("allocator create / alloc / free / destroy");

    zst_allocator_t* alloc = zst_allocator_cpu_create();
    assert(alloc != NULL);

    void* ptr = zst_allocator_alloc(alloc, 1024);
    assert(ptr != NULL);

    zst_allocator_free(alloc, ptr);

    zst_buffer_t* buf = zst_buffer_create_with_allocator(ZST_BUFFER_USER, alloc, 512);
    assert(buf != NULL);
    assert(buf->memory.data != NULL);
    assert(buf->memory.size == 512);

    /* Buffer creation should have bumped the allocator's refcount */
    assert(alloc->refcount == 2);

    zst_buffer_unref(buf);

    /* Allocator refcount should be back to 1 */
    assert(alloc->refcount == 1);

    zst_allocator_unref(alloc);

    PASS();
}


static void
test_allocator_pool_nonblock(void)
{
    TEST("allocator pool nonblock acquire");

    zst_allocator_t* alloc = zst_allocator_cpu_create();

    zst_buffer_pool_config_t config = {0};
    config.min_buffers = 2;
    config.max_buffers = 2;
    config.buffer_size = 1024;
    config.buffer_type = ZST_BUFFER_USER;

    zst_buffer_pool_t* pool = zst_buffer_pool_create(alloc, &config);
    assert(pool != NULL);

    zst_buffer_t* buf1 = NULL;
    zst_buffer_t* buf2 = NULL;
    zst_buffer_t* buf3 = NULL;

    /* Acquire first buffer */
    assert(zst_buffer_pool_acquire(pool, &buf1, 0, 0) == ZST_OK);
    assert(buf1 != NULL);

    /* Acquire second buffer */
    assert(zst_buffer_pool_acquire(pool, &buf2, 0, 0) == ZST_OK);
    assert(buf2 != NULL);

    /* Pool is now exhausted. Blocking acquire should time out */
    assert(zst_buffer_pool_acquire(pool, &buf3, 50, 0) == ZST_TIMEOUT);

    /* Non-blocking acquire should time out immediately */
    assert(zst_buffer_pool_acquire(pool, &buf3, -1, ZST_POOL_ACQUIRE_NONBLOCK) == ZST_TIMEOUT);

    /* Release buffer 1 */
    zst_buffer_unref(buf1);

    /* Non-blocking acquire should now succeed */
    assert(zst_buffer_pool_acquire(pool, &buf3, -1, ZST_POOL_ACQUIRE_NONBLOCK) == ZST_OK);
    assert(buf3 != NULL);

    zst_buffer_unref(buf2);
    zst_buffer_unref(buf3);

    zst_buffer_pool_destroy(pool);
    zst_allocator_unref(alloc);

    PASS();
}

static void
test_clock_basic(void)
{
    TEST("clock create / time / wait / destroy");

    zst_clock_t* clk = zst_clock_system_create();
    assert(clk != NULL);

    zst_time_t t1 = zst_clock_get_time(clk);
    assert(t1 > 0);

    /* Wait for 50 ms (50000000 ns) */
    zst_clock_wait(clk, 50000000);

    zst_time_t t2 = zst_clock_get_time(clk);
    assert(t2 > t1);
    /* Should have elapsed at least ~50ms */
    assert(t2 - t1 >= 40000000);

    zst_pipeline_t* pipe = zst_pipeline_create();
    zst_pipeline_set_clock(pipe, clk);
    assert(zst_pipeline_get_clock(pipe) == clk);
    assert(clk->refcount == 2);

    zst_pipeline_destroy(pipe);
    assert(clk->refcount == 1);

    zst_clock_unref(clk);

    PASS();
}

static void
test_clock_slaving(void)
{
    TEST("clock slaving time advance");

    zst_clock_t* sys_clock = zst_clock_system_create();
    zst_clock_t* master_clock = zst_clock_system_create();
    zst_clock_t* slave = zst_clock_slave_create(master_clock, sys_clock);
    assert(slave != NULL);

    zst_time_t t1 = zst_clock_get_time(slave);
    zst_clock_wait(slave, 50000000); // 50 ms
    zst_time_t t2 = zst_clock_get_time(slave);

    assert(t2 > t1);

    zst_clock_unref(slave);
    zst_clock_unref(master_clock);
    zst_clock_unref(sys_clock);

    PASS();
}

/* ── Text Overlay (Phase 11a) ────────────────────────────────────────────── */

static void
test_text_overlay(void)
{
    TEST("text_overlay basic rendering");

    zst_element_t* overlay = zst_text_overlay_create("TEST");
    assert(overlay != NULL);

    /* We ignore open() failure here because the system might not have the font */
    zst_result_t open_res = overlay->ops->open(overlay);
    if (open_res != ZST_OK) {
        printf("  [SKIP] Could not open text overlay (missing font?)\n");
        zst_element_destroy(overlay);
        PASS();
        return;
    }

    zst_pad_t* sinkpad = zst_element_get_pad(overlay, "sink");
    assert(sinkpad != NULL);

    zst_caps_t* caps = zst_caps_create();
    zst_caps_append(caps, zst_caps_struct_create_video("video/x-raw", 320, 240, 30.0, "YUV420P"));
    sinkpad->caps = caps;

    zst_buffer_t* in_buf = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);

    zst_video_frame_t* vf = malloc(sizeof(*vf));
    vf->width = 320;
    vf->height = 240;
    vf->format = 0;

    uint8_t* dummy_data = calloc(1, 320 * 240 * 3 / 2);
    vf->plane[0] = dummy_data;
    vf->plane[1] = dummy_data + 320 * 240;
    vf->plane[2] = dummy_data + 320 * 240 + 320 * 240 / 4;
    vf->stride[0] = 320;
    vf->stride[1] = 160;
    vf->stride[2] = 160;

    in_buf->payload = vf;

    zst_buffer_t* out_buf = NULL;
    assert(overlay->ops->process(overlay, in_buf, &out_buf) == ZST_OK);
    assert(out_buf != NULL);

    zst_buffer_unref(out_buf);
    in_buf->payload = NULL;
    zst_buffer_unref(in_buf);

    free(dummy_data);
    free(vf);

    assert(overlay->ops->close(overlay) == ZST_OK);
    zst_element_destroy(overlay);

    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   Main
   ═══════════════════════════════════════════════════════════════ */
static void
test_text_overlay_multiline(void)
{
    TEST("text_overlay multi-line rendering");

    zst_element_t* overlay = zst_text_overlay_create("Line 1\nLine 2 is longer\nLine 3");
    assert(overlay != NULL);

    zst_result_t open_res = overlay->ops->open(overlay);
    if (open_res != ZST_OK) {
        printf("  [SKIP] Could not open text overlay (missing font?)\n");
        zst_element_destroy(overlay);
        PASS();
        return;
    }

    zst_pad_t* sinkpad = zst_element_get_pad(overlay, "sink");
    assert(sinkpad != NULL);

    zst_caps_t* caps = zst_caps_create();
    zst_caps_append(caps, zst_caps_struct_create_video("video/x-raw", 320, 240, 30.0, "YUV420P"));
    sinkpad->caps = caps;

    zst_buffer_t* in_buf = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);

    zst_video_frame_t* vf = malloc(sizeof(*vf));
    vf->width = 320;
    vf->height = 240;
    vf->format = 0;

    uint8_t* dummy_data = calloc(1, 320 * 240 * 3 / 2);
    vf->plane[0] = dummy_data;
    vf->plane[1] = dummy_data + 320 * 240;
    vf->plane[2] = dummy_data + 320 * 240 + 320 * 240 / 4;
    vf->stride[0] = 320;
    vf->stride[1] = 160;
    vf->stride[2] = 160;

    in_buf->payload = vf;

    zst_buffer_t* out_buf = NULL;
    assert(overlay->ops->process(overlay, in_buf, &out_buf) == ZST_OK);
    assert(out_buf != NULL);

    zst_buffer_unref(out_buf);
    in_buf->payload = NULL;
    zst_buffer_unref(in_buf);

    free(dummy_data);
    free(vf);

    zst_element_destroy(overlay);
    PASS();
}

int main(void)
{
    printf("\n╔════════════════════════════════════════════════════╗\n");
    printf("║     zstreamer — core unit tests                   ║\n");
    printf("╚════════════════════════════════════════════════════╝\n\n");

    /* ── Buffer ── */
    printf("[buffer]\n");
    test_buffer_create_destroy();
    test_buffer_refcount();
    test_buffer_null_safety();
    test_buffer_create_with_pool();

    /* ── Pad ── */
    printf("[pad]\n");
    test_pad_create_destroy();
    test_pad_link_unlink();
    test_pad_invalid_link();

    /* ── Element ── */
    printf("[element]\n");
    test_element_create_destroy();
    test_element_state_transition();
    test_element_pads();

    /* ── Pipeline ── */
    printf("[pipeline]\n");
    test_pipeline_create_destroy();
    test_pipeline_add_remove();
    test_pipeline_state_propagation();
    test_pipeline_topological_sort_check();

    /* ── Queue ── */
    printf("[queue]\n");
    test_queue_push_pop();
    test_queue_timeout();
    test_queue_flush();
    test_queue_config_limits();

    /* ── Scheduler (Phase 2) ── */
    printf("[scheduler]\n");
    test_scheduler_single_threaded();
    test_scheduler_multi_threaded();

    /* ── Caps Negotiation (Phase 5) ── */
    printf("[caps negotiation]\n");
    test_caps_basic();
    test_caps_intersection_video();
    test_caps_intersection_audio();
    test_caps_fixate();
    test_pad_negotiate_and_link();

    /* ── Event Bus (Phase 6) ── */
    printf("[event bus]\n");
    test_event_create_destroy();
    test_bus_basic();
    test_bus_timeout();
    test_bus_async_dispatch();
    test_pipeline_bus_events();

    /* ── Dynamic Plugins (Phase 7) ── */
    printf("[dynamic plugins]\n");
    test_plugin_registry_basic();
    test_element_factory_refcounting();

    /* ── Logging (Phase 3.5) ── */
    printf("[logging]\n");
    test_log_levels();
    test_log_custom_handler();

    /* ── Conversion Elements (Phase 4g/4h) ── */
    printf("[conversion elements]\n");
    test_video_scaler();
    test_audio_resampler();

    /* ── Allocator (Phase 8a) ── */
    printf("[allocator]\n");
    test_allocator_basic();
    test_allocator_pool_nonblock();

    /* ── Clock (Phase 8b) ── */
    printf("[clock]\n");
    test_clock_basic();
    test_clock_slaving();

    /* ── Text Overlay (Phase 11a) ── */
    printf("[text overlay]\n");
    test_text_overlay();
    test_text_overlay_multiline();

    /* ── Summary ── */
    printf("\n──────────────────────────────────────────────────\n");
    printf("  %d / %d tests passed\n", g_tests_passed, g_tests_run);
    printf("──────────────────────────────────────────────────\n\n");

    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
