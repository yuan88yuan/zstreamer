/*=============================================================================
    test_core.c — Unit / smoke tests for the zstreamer core framework
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#include "mm_types.h"
#include "mm_buffer.h"
#include "mm_pad.h"
#include "mm_element.h"
#include "mm_pipeline.h"
#include "mm_queue.h"
#include "mm_scheduler.h"
#include "mm_bus.h"
#include "mm_plugin.h"

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
    mm_buffer_t* buf = mm_buffer_create(MM_BUFFER_VIDEO_FRAME);
    assert(buf != NULL);
    assert(buf->type == MM_BUFFER_VIDEO_FRAME);
    assert(buf->refcount == 1);

    mm_buffer_unref(buf);
    PASS();
}

static void
test_buffer_refcount(void)
{
    TEST("buffer refcount");
    mm_buffer_t* buf = mm_buffer_create(MM_BUFFER_AUDIO_FRAME);
    assert(buf != NULL);

    mm_buffer_ref(buf);
    assert(buf->refcount == 2);

    mm_buffer_unref(buf);
    assert(buf->refcount == 1);

    mm_buffer_unref(buf);
    PASS();
}

static void
test_buffer_null_safety(void)
{
    TEST("buffer null safety");
    /* These should not crash */
    mm_buffer_ref(NULL);
    mm_buffer_unref(NULL);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   Pad tests
   ═══════════════════════════════════════════════════════════════ */
static void
test_pad_create_destroy(void)
{
    TEST("pad create / destroy");
    mm_pad_t* src = mm_pad_create("src", MM_PAD_SRC);
    assert(src != NULL);
    assert(strcmp(src->name, "src") == 0);
    assert(src->direction == MM_PAD_SRC);

    mm_pad_t* sink = mm_pad_create("sink", MM_PAD_SINK);
    assert(sink != NULL);
    assert(sink->direction == MM_PAD_SINK);

    mm_pad_destroy(src);
    mm_pad_destroy(sink);
    PASS();
}

static void
test_pad_link_unlink(void)
{
    TEST("pad link / unlink");
    mm_pad_t* src  = mm_pad_create("src",  MM_PAD_SRC);
    mm_pad_t* sink = mm_pad_create("sink", MM_PAD_SINK);

    mm_result_t r = mm_pad_link(src, sink);
    assert(r == MM_OK);
    assert(src->peer == sink);
    assert(sink->peer == src);

    /* Double-link should fail */
    mm_pad_t* sink2 = mm_pad_create("sink2", MM_PAD_SINK);
    r = mm_pad_link(src, sink2);
    assert(r == MM_ERROR);

    mm_pad_unlink(src);
    assert(src->peer == NULL);
    assert(sink->peer == NULL);

    mm_pad_destroy(src);
    mm_pad_destroy(sink);
    mm_pad_destroy(sink2);
    PASS();
}

static void
test_pad_invalid_link(void)
{
    TEST("pad invalid link");
    mm_pad_t* src  = mm_pad_create("src",  MM_PAD_SRC);
    mm_pad_t* src2 = mm_pad_create("src2", MM_PAD_SRC);

    /* SRC-SRC should fail */
    mm_result_t r = mm_pad_link(src, src2);
    assert(r == MM_ERROR);

    mm_pad_destroy(src);
    mm_pad_destroy(src2);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   Element tests
   ═══════════════════════════════════════════════════════════════ */
static mm_result_t
dummy_open(mm_element_t* el) { (void)el; return MM_OK; }

static mm_element_ops_t g_dummy_ops = {
    .name   = "dummy",
    .open   = dummy_open,
};

static void
test_element_create_destroy(void)
{
    TEST("element create / destroy");
    mm_element_t* el = mm_element_create(&g_dummy_ops, NULL);
    assert(el != NULL);
    assert(el->state == MM_STATE_NULL);
    assert(el->ops == &g_dummy_ops);

    mm_element_destroy(el);
    PASS();
}

static void
test_element_state_transition(void)
{
    TEST("element state transition");
    mm_element_t* el = mm_element_create(&g_dummy_ops, NULL);
    mm_result_t r;

    r = mm_element_set_state(el, MM_STATE_READY);
    assert(r == MM_OK);
    assert(el->state == MM_STATE_READY);

    r = mm_element_set_state(el, MM_STATE_PLAYING);
    assert(r == MM_OK);
    assert(el->state == MM_STATE_PLAYING);

    r = mm_element_set_state(el, MM_STATE_NULL);
    assert(r == MM_OK);
    assert(el->state == MM_STATE_NULL);

    mm_element_destroy(el);
    PASS();
}

static void
test_element_pads(void)
{
    TEST("element pads");
    mm_element_t* el  = mm_element_create(&g_dummy_ops, NULL);
    mm_pad_t*     src = mm_pad_create("src", MM_PAD_SRC);
    mm_pad_t*     snk = mm_pad_create("sink", MM_PAD_SINK);

    mm_element_add_pad(el, src);
    mm_element_add_pad(el, snk);

    assert(el->nb_src_pads  == 1);
    assert(el->nb_sink_pads == 1);

    /* get_pad should find both */
    assert(mm_element_get_pad(el, "src")  == src);
    assert(mm_element_get_pad(el, "sink") == snk);
    assert(mm_element_get_pad(el, "none") == NULL);

    mm_element_destroy(el);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   Pipeline tests
   ═══════════════════════════════════════════════════════════════ */
static void
test_pipeline_create_destroy(void)
{
    TEST("pipeline create / destroy");
    mm_pipeline_t* pipe = mm_pipeline_create();
    assert(pipe != NULL);
    mm_pipeline_destroy(pipe);
    PASS();
}

static void
test_pipeline_add_remove(void)
{
    TEST("pipeline add / remove");
    mm_pipeline_t* pipe = mm_pipeline_create();
    mm_element_t*  el   = mm_element_create(&g_dummy_ops, NULL);

    mm_pipeline_add(pipe, el);
    assert(pipe->nb_elements == 1);

    mm_pipeline_remove(pipe, el);
    assert(pipe->nb_elements == 0);

    mm_element_destroy(el);
    mm_pipeline_destroy(pipe);
    PASS();
}

static void
test_pipeline_state_propagation(void)
{
    TEST("pipeline state propagation");
    mm_pipeline_t* pipe = mm_pipeline_create();
    mm_element_t*  el   = mm_element_create(&g_dummy_ops, NULL);

    mm_pipeline_add(pipe, el);
    mm_pipeline_set_state(pipe, MM_STATE_PLAYING);
    assert(el->state == MM_STATE_PLAYING);
    assert(pipe->state == MM_STATE_PLAYING);

    mm_pipeline_set_state(pipe, MM_STATE_NULL);
    assert(el->state == MM_STATE_NULL);

    mm_pipeline_destroy(pipe);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   Queue tests
   ═══════════════════════════════════════════════════════════════ */
static void
test_queue_push_pop(void)
{
    TEST("queue push / pop");
    mm_queue_config_t cfg = {
        .mode        = MM_QUEUE_SYNC,
        .max_buffers = 5,
        .max_bytes   = 0,
        .max_duration= 0,
    };
    mm_queue_t* q = mm_queue_create(&cfg);
    assert(q != NULL);

    mm_buffer_t* b1 = mm_buffer_create(MM_BUFFER_VIDEO_FRAME);
    mm_buffer_t* b2 = mm_buffer_create(MM_BUFFER_AUDIO_FRAME);

    /* Push two buffers */
    assert(mm_queue_push(q, b1, 1000) == MM_OK);
    assert(mm_queue_push(q, b2, 1000) == MM_OK);
    assert(mm_queue_size(q) == 2);

    /* Pop them back */
    mm_buffer_t* out;
    assert(mm_queue_pop(q, &out, 1000) == MM_OK);
    assert(out == b1);
    mm_buffer_unref(out);

    assert(mm_queue_pop(q, &out, 1000) == MM_OK);
    assert(out == b2);
    mm_buffer_unref(out);

    assert(mm_queue_size(q) == 0);

    mm_queue_destroy(q);
    mm_buffer_unref(b1);
    mm_buffer_unref(b2);
    PASS();
}

static void
test_queue_timeout(void)
{
    TEST("queue timeout");
    mm_queue_config_t cfg = {
        .mode        = MM_QUEUE_SYNC,
        .max_buffers = 1,
    };
    mm_queue_t* q = mm_queue_create(&cfg);
    assert(q != NULL);

    /* Pop from empty queue should timeout */
    mm_buffer_t* out;
    mm_result_t r = mm_queue_pop(q, &out, 10);
    assert(r == MM_TIMEOUT);

    mm_queue_destroy(q);
    PASS();
}

static void
test_queue_flush(void)
{
    TEST("queue flush");
    mm_queue_config_t cfg = {
        .mode        = MM_QUEUE_SYNC,
        .max_buffers = 5,
    };
    mm_queue_t* q = mm_queue_create(&cfg);

    mm_buffer_t* b1 = mm_buffer_create(MM_BUFFER_VIDEO_FRAME);
    mm_buffer_t* b2 = mm_buffer_create(MM_BUFFER_AUDIO_FRAME);
    mm_queue_push(q, b1, 1000);
    mm_queue_push(q, b2, 1000);

    mm_queue_flush(q);
    assert(mm_queue_size(q) == 0);

    mm_buffer_unref(b1);
    mm_buffer_unref(b2);
    mm_queue_destroy(q);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   Scheduler / Pipeline integration tests (Phase 2)
   ═══════════════════════════════════════════════════════════════ */
static void
mock_buf_destroy(mm_buffer_t* b)
{
    free(b->payload);
}

static mm_result_t
mock_source_process(mm_element_t* el, mm_buffer_t* in, mm_buffer_t** out)
{
    (void)in;
    int* counter = el->priv;
    if (*counter >= 5) {
        return MM_EOF;
    }

    mm_buffer_t* buf = mm_buffer_create(MM_BUFFER_USER);
    if (!buf) return MM_ERROR;

    int* data = malloc(sizeof(int));
    if (!data) {
        mm_buffer_unref(buf);
        return MM_ERROR;
    }
    *data = ++(*counter);
    buf->payload = data;
    buf->destroy = mock_buf_destroy;

    *out = buf;
    return MM_OK;
}

static mm_result_t
mock_transform_process(mm_element_t* el, mm_buffer_t* in, mm_buffer_t** out)
{
    (void)el;
    if (!in || !in->payload) return MM_ERROR;

    int val = *(int*)in->payload;

    mm_buffer_t* buf = mm_buffer_create(MM_BUFFER_USER);
    if (!buf) return MM_ERROR;

    int* data = malloc(sizeof(int));
    if (!data) {
        mm_buffer_unref(buf);
        return MM_ERROR;
    }
    *data = val * 2;
    buf->payload = data;
    buf->destroy = mock_buf_destroy;

    *out = buf;
    return MM_OK;
}

typedef struct {
    int count;
    int sum;
} mock_sink_t;

static mm_result_t
mock_sink_process(mm_element_t* el, mm_buffer_t* in, mm_buffer_t** out)
{
    (void)out;
    mock_sink_t* sink = el->priv;
    if (!in || !in->payload) return MM_ERROR;

    int val = *(int*)in->payload;
    sink->count++;
    sink->sum += val;

    return MM_OK;
}

static void
test_scheduler_single_threaded(void)
{
    TEST("scheduler single-threaded pipeline");

    mm_pipeline_t* pipe = mm_pipeline_create();
    mm_scheduler_config_t cfg = {
        .mode = MM_SCHEDULER_SINGLE_THREAD,
        .worker_threads = 1
    };
    mm_scheduler_t* sched = mm_scheduler_create(&cfg);
    mm_scheduler_attach(sched, pipe);

    int* source_counter = malloc(sizeof(int));
    *source_counter = 0;
    static mm_element_ops_t source_ops = {
        .name = "mock_source",
        .process = mock_source_process
    };
    mm_element_t* source = mm_element_create(&source_ops, source_counter);
    mm_pad_t* src_pad = mm_pad_create("src", MM_PAD_SRC);
    mm_element_add_pad(source, src_pad);

    static mm_element_ops_t transform_ops = {
        .name = "mock_transform",
        .process = mock_transform_process
    };
    mm_element_t* transform = mm_element_create(&transform_ops, NULL);
    mm_pad_t* trans_sink = mm_pad_create("sink", MM_PAD_SINK);
    mm_pad_t* trans_src = mm_pad_create("src", MM_PAD_SRC);
    mm_element_add_pad(transform, trans_sink);
    mm_element_add_pad(transform, trans_src);

    mock_sink_t* sink_data = calloc(1, sizeof(mock_sink_t));
    static mm_element_ops_t sink_ops = {
        .name = "mock_sink",
        .process = mock_sink_process
    };
    mm_element_t* sink = mm_element_create(&sink_ops, sink_data);
    mm_pad_t* sink_pad = mm_pad_create("sink", MM_PAD_SINK);
    mm_element_add_pad(sink, sink_pad);

    mm_pipeline_add(pipe, source);
    mm_pipeline_add(pipe, transform);
    mm_pipeline_add(pipe, sink);

    mm_pad_link(src_pad, trans_sink);
    mm_pad_link(trans_src, sink_pad);

    mm_pipeline_set_state(pipe, MM_STATE_PLAYING);
    mm_scheduler_run(sched);

    struct timespec ts = { .tv_sec = 0, .tv_nsec = 50000000 }; /* 50 ms */
    nanosleep(&ts, NULL);

    mm_pipeline_set_state(pipe, MM_STATE_NULL);
    mm_scheduler_stop(sched);

    assert(sink_data->count == 5);
    assert(sink_data->sum == 30);

    mm_scheduler_destroy(sched);
    mm_pipeline_destroy(pipe);

    PASS();
}

static void
test_queue_config_limits(void)
{
    TEST("queue config limits (bytes, duration, async mode)");

    /* Test 1: Max bytes in SYNC mode blocks/times out */
    mm_queue_config_t cfg_bytes = {
        .mode = MM_QUEUE_SYNC,
        .max_bytes = 100,
    };
    mm_queue_t* q1 = mm_queue_create(&cfg_bytes);
    mm_buffer_t* b1 = mm_buffer_create(MM_BUFFER_USER);
    b1->memory.size = 60;
    assert(mm_queue_push(q1, b1, 10) == MM_OK);
    
    mm_buffer_t* b2 = mm_buffer_create(MM_BUFFER_USER);
    b2->memory.size = 50; // Total is now 110, queue is now full
    assert(mm_queue_push(q1, b2, 10) == MM_OK);

    mm_buffer_t* b3 = mm_buffer_create(MM_BUFFER_USER);
    b3->memory.size = 10; // Queue is already full (110 >= 100), this must time out
    assert(mm_queue_push(q1, b3, 10) == MM_TIMEOUT);

    mm_queue_destroy(q1);
    mm_buffer_unref(b1);
    mm_buffer_unref(b2);
    mm_buffer_unref(b3);

    /* Test 2: Max duration in SYNC mode blocks/times out */
    mm_queue_config_t cfg_dur = {
        .mode = MM_QUEUE_SYNC,
        .max_duration = 1000,
    };
    mm_queue_t* q2 = mm_queue_create(&cfg_dur);
    mm_buffer_t* bd1 = mm_buffer_create(MM_BUFFER_USER);
    bd1->pts = 10000;
    bd1->duration = 0;
    assert(mm_queue_push(q2, bd1, 10) == MM_OK);

    mm_buffer_t* bd2 = mm_buffer_create(MM_BUFFER_USER);
    bd2->pts = 11500; // Duration is 1500 (>= 1000), queue is now full
    bd2->duration = 0;
    assert(mm_queue_push(q2, bd2, 10) == MM_OK);

    mm_buffer_t* bd3 = mm_buffer_create(MM_BUFFER_USER);
    bd3->pts = 12000; // Queue is already full (1500 >= 1000), this must time out
    bd3->duration = 0;
    assert(mm_queue_push(q2, bd3, 10) == MM_TIMEOUT);

    mm_queue_destroy(q2);
    mm_buffer_unref(bd1);
    mm_buffer_unref(bd2);
    mm_buffer_unref(bd3);

    /* Test 3: ASYNC mode drops buffers when full */
    mm_queue_config_t cfg_async = {
        .mode = MM_QUEUE_ASYNC,
        .max_buffers = 1,
    };
    mm_queue_t* q3 = mm_queue_create(&cfg_async);
    mm_buffer_t* ba1 = mm_buffer_create(MM_BUFFER_USER);
    assert(mm_queue_push(q3, ba1, 10) == MM_OK);

    mm_buffer_t* ba2 = mm_buffer_create(MM_BUFFER_USER);
    assert(mm_queue_push(q3, ba2, 10) == MM_ERROR);

    mm_queue_destroy(q3);
    mm_buffer_unref(ba1);
    mm_buffer_unref(ba2);

    PASS();
}

static void
test_scheduler_multi_threaded(void)
{
    TEST("scheduler multi-threaded pipeline with queues");

    mm_pipeline_t* pipe = mm_pipeline_create();
    mm_scheduler_config_t cfg = {
        .mode = MM_SCHEDULER_MULTI_THREAD,
        .worker_threads = 3
    };
    mm_scheduler_t* sched = mm_scheduler_create(&cfg);
    mm_scheduler_attach(sched, pipe);

    int* source_counter = malloc(sizeof(int));
    *source_counter = 0;
    static mm_element_ops_t source_ops = {
        .name = "mock_source",
        .process = mock_source_process
    };
    mm_element_t* source = mm_element_create(&source_ops, source_counter);
    mm_pad_t* src_pad = mm_pad_create("src", MM_PAD_SRC);
    mm_element_add_pad(source, src_pad);

    /* Add explicit queue elements */
    mm_element_t* q1 = mm_queue_element_create(NULL);
    mm_element_t* q2 = mm_queue_element_create(NULL);

    static mm_element_ops_t transform_ops = {
        .name = "mock_transform",
        .process = mock_transform_process
    };
    mm_element_t* transform = mm_element_create(&transform_ops, NULL);
    mm_pad_t* trans_sink = mm_pad_create("sink", MM_PAD_SINK);
    mm_pad_t* trans_src = mm_pad_create("src", MM_PAD_SRC);
    mm_element_add_pad(transform, trans_sink);
    mm_element_add_pad(transform, trans_src);

    mock_sink_t* sink_data = calloc(1, sizeof(mock_sink_t));
    static mm_element_ops_t sink_ops = {
        .name = "mock_sink",
        .process = mock_sink_process
    };
    mm_element_t* sink = mm_element_create(&sink_ops, sink_data);
    mm_pad_t* sink_pad = mm_pad_create("sink", MM_PAD_SINK);
    mm_element_add_pad(sink, sink_pad);

    mm_pipeline_add(pipe, source);
    mm_pipeline_add(pipe, q1);
    mm_pipeline_add(pipe, transform);
    mm_pipeline_add(pipe, q2);
    mm_pipeline_add(pipe, sink);

    mm_pad_link(src_pad, mm_element_get_pad(q1, "sink"));
    mm_pad_link(mm_element_get_pad(q1, "src"), trans_sink);
    mm_pad_link(trans_src, mm_element_get_pad(q2, "sink"));
    mm_pad_link(mm_element_get_pad(q2, "src"), sink_pad);

    mm_pipeline_set_state(pipe, MM_STATE_PLAYING);
    mm_scheduler_run(sched);

    struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 }; /* 100 ms to let explicit queues process */
    nanosleep(&ts, NULL);

    mm_pipeline_set_state(pipe, MM_STATE_NULL);
    mm_scheduler_stop(sched);

    assert(sink_data->count == 5);
    assert(sink_data->sum == 30);

    mm_scheduler_destroy(sched);
    mm_pipeline_destroy(pipe);

    PASS();
}

static void
test_pipeline_topological_sort_check(void)
{
    TEST("pipeline topological sort");

    mm_pipeline_t* pipe = mm_pipeline_create();

    mm_element_t* a = mm_element_create(&g_dummy_ops, NULL);
    mm_pad_t* a_src = mm_pad_create("src", MM_PAD_SRC);
    mm_element_add_pad(a, a_src);

    mm_element_t* b = mm_element_create(&g_dummy_ops, NULL);
    mm_pad_t* b_sink = mm_pad_create("sink", MM_PAD_SINK);
    mm_pad_t* b_src = mm_pad_create("src", MM_PAD_SRC);
    mm_element_add_pad(b, b_sink);
    mm_element_add_pad(b, b_src);

    mm_element_t* c = mm_element_create(&g_dummy_ops, NULL);
    mm_pad_t* c_sink = mm_pad_create("sink", MM_PAD_SINK);
    mm_element_add_pad(c, c_sink);

    mm_pad_link(a_src, b_sink);
    mm_pad_link(b_src, c_sink);

    mm_pipeline_add(pipe, c);
    mm_pipeline_add(pipe, b);
    mm_pipeline_add(pipe, a);

    assert(pipe->elements[0] == c);
    assert(pipe->elements[1] == b);
    assert(pipe->elements[2] == a);

    mm_pipeline_topological_sort(pipe);

    assert(pipe->elements[0] == a);
    assert(pipe->elements[1] == b);
    assert(pipe->elements[2] == c);

    mm_pipeline_destroy(pipe);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   Caps Negotiation tests (Phase 5)
   ═══════════════════════════════════════════════════════════════ */
static void
test_caps_basic(void)
{
    TEST("caps create / copy / destroy");
    mm_caps_t* caps = mm_caps_create();
    assert(caps != NULL);
    assert(caps->structs == NULL);

    mm_caps_struct_t* s1 = mm_caps_struct_create_video("video/x-raw", 640, 480, 30.0, "YUV420P");
    assert(s1 != NULL);
    assert(strcmp(s1->media_type, "video/x-raw") == 0);
    assert(s1->type == MM_CAPS_VIDEO);
    assert(s1->video.width == 640);
    assert(s1->video.height == 480);
    assert(s1->video.framerate == 30.0);
    assert(strcmp(s1->video.pixel_format, "YUV420P") == 0);

    mm_result_t ret = mm_caps_append(caps, s1);
    assert(ret == MM_OK);
    assert(caps->structs == s1);

    mm_caps_t* copy = mm_caps_copy(caps);
    assert(copy != NULL);
    assert(copy->structs != NULL);
    assert(copy->structs != s1);
    assert(strcmp(copy->structs->media_type, "video/x-raw") == 0);
    assert(copy->structs->video.width == 640);

    mm_caps_destroy(caps);
    mm_caps_destroy(copy);
    PASS();
}

static void
test_caps_intersection_video(void)
{
    TEST("caps intersection (video)");
    
    mm_caps_t* c1 = mm_caps_create();
    mm_caps_append(c1, mm_caps_struct_create_video("video/x-raw", 640, 480, 30.0, "YUV420P"));
    mm_caps_t* c2 = mm_caps_create();
    mm_caps_append(c2, mm_caps_struct_create_video("video/x-h264", 640, 480, 30.0, ""));
    
    mm_caps_t* res = mm_caps_intersect(c1, c2);
    assert(res != NULL);
    assert(res->structs == NULL);
    mm_caps_destroy(res);

    mm_caps_destroy(c2);
    c2 = mm_caps_create();
    mm_caps_append(c2, mm_caps_struct_create_video("video/x-raw", 0, 480, 0.0, ""));
    
    res = mm_caps_intersect(c1, c2);
    assert(res != NULL);
    assert(res->structs != NULL);
    assert(res->structs->video.width == 640);
    assert(res->structs->video.height == 480);
    assert(res->structs->video.framerate == 30.0);
    assert(strcmp(res->structs->video.pixel_format, "YUV420P") == 0);
    mm_caps_destroy(res);

    mm_caps_destroy(c2);
    c2 = mm_caps_create();
    mm_caps_append(c2, mm_caps_struct_create_video("video/x-raw", 1280, 480, 30.0, "YUV420P"));
    
    res = mm_caps_intersect(c1, c2);
    assert(res != NULL);
    assert(res->structs == NULL);
    mm_caps_destroy(res);

    mm_caps_destroy(c1);
    mm_caps_destroy(c2);
    PASS();
}

static void
test_caps_intersection_audio(void)
{
    TEST("caps intersection (audio)");
    
    mm_caps_t* c1 = mm_caps_create();
    mm_caps_append(c1, mm_caps_struct_create_audio("audio/x-raw", 2, 44100, "S16LE"));
    mm_caps_t* c2 = mm_caps_create();
    mm_caps_append(c2, mm_caps_struct_create_audio("audio/x-raw", 0, 0, ""));
    
    mm_caps_t* res = mm_caps_intersect(c1, c2);
    assert(res != NULL);
    assert(res->structs != NULL);
    assert(res->structs->audio.channels == 2);
    assert(res->structs->audio.sample_rate == 44100);
    assert(strcmp(res->structs->audio.format, "S16LE") == 0);
    
    mm_caps_destroy(res);
    mm_caps_destroy(c1);
    mm_caps_destroy(c2);
    PASS();
}

static void
test_caps_fixate(void)
{
    TEST("caps fixation");
    
    mm_caps_t* caps = mm_caps_create();
    mm_caps_append(caps, mm_caps_struct_create_video("video/x-raw", 0, 0, 0.0, ""));
    
    assert(!mm_caps_is_fixed(caps));
    
    mm_result_t ret = mm_caps_fixate(caps);
    assert(ret == MM_OK);
    
    assert(mm_caps_is_fixed(caps));
    assert(caps->structs->video.width == 640);
    assert(caps->structs->video.height == 480);
    assert(caps->structs->video.framerate == 30.0);
    assert(strcmp(caps->structs->video.pixel_format, "YUV420P") == 0);
    
    mm_caps_destroy(caps);
    PASS();
}

static mm_caps_t*
element_get_caps_cb(mm_element_t* el, mm_pad_t* pad, const mm_caps_t* filter)
{
    (void)el;
    (void)pad;
    (void)filter;
    mm_caps_t* caps = mm_caps_create();
    mm_caps_append(caps, mm_caps_struct_create_video("video/x-raw", 1920, 1080, 60.0, "NV12"));
    return caps;
}

static mm_element_ops_t g_query_ops = {
    .name = "query_element",
    .get_caps = element_get_caps_cb
};

static void
test_pad_negotiate_and_link(void)
{
    TEST("pad caps negotiation / link compatibility");
    
    mm_pad_t* src = mm_pad_create("src", MM_PAD_SRC);
    mm_pad_t* sink = mm_pad_create("sink", MM_PAD_SINK);
    
    mm_caps_t* src_template = mm_caps_create();
    mm_caps_append(src_template, mm_caps_struct_create_video("video/x-raw", 640, 480, 30.0, "YUV420P"));
    mm_pad_set_template_caps(src, src_template);
    
    mm_caps_t* sink_template = mm_caps_create();
    mm_caps_append(sink_template, mm_caps_struct_create_video("video/x-raw", 1280, 720, 30.0, "YUV420P"));
    mm_pad_set_template_caps(sink, sink_template);
    
    mm_result_t ret = mm_pad_link(src, sink);
    assert(ret == MM_ERROR);
    assert(src->peer == NULL);
    assert(sink->peer == NULL);
    
    mm_caps_destroy(sink_template);
    sink_template = mm_caps_create();
    mm_caps_append(sink_template, mm_caps_struct_create_video("video/x-raw", 0, 0, 0.0, ""));
    mm_pad_set_template_caps(sink, sink_template);
    
    ret = mm_pad_link(src, sink);
    assert(ret == MM_OK);
    assert(src->peer == sink);
    assert(sink->peer == src);
    
    assert(src->caps != NULL);
    assert(sink->caps != NULL);
    assert(mm_caps_is_fixed(src->caps));
    assert(src->caps->structs->video.width == 640);
    assert(src->caps->structs->video.height == 480);
    assert(strcmp(src->caps->structs->video.pixel_format, "YUV420P") == 0);
    
    mm_pad_destroy(src);
    mm_pad_destroy(sink);
    mm_caps_destroy(src_template);
    mm_caps_destroy(sink_template);
    
    mm_element_t* el = mm_element_create(&g_query_ops, NULL);
    mm_pad_t* query_pad = mm_pad_create("src", MM_PAD_SRC);
    mm_element_add_pad(el, query_pad);
    
    mm_caps_t* queried = mm_pad_get_caps(query_pad);
    assert(queried != NULL);
    assert(queried->structs != NULL);
    assert(queried->structs->video.width == 1920);
    assert(strcmp(queried->structs->video.pixel_format, "NV12") == 0);
    
    mm_caps_destroy(queried);
    mm_element_destroy(el);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   Event Bus tests (Phase 6)
   ═══════════════════════════════════════════════════════════════ */
static void
test_event_create_destroy(void)
{
    TEST("event create / destroy");
    
    mm_event_t* ev1 = mm_event_new_eos(NULL);
    assert(ev1 != NULL);
    assert(ev1->type == MM_EVENT_EOS);
    assert(ev1->src == NULL);
    mm_event_destroy(ev1);
    
    mm_event_t* ev2 = mm_event_new_error(NULL, MM_ERROR, "test error");
    assert(ev2 != NULL);
    assert(ev2->type == MM_EVENT_ERROR);
    assert(ev2->as.error.result == MM_ERROR);
    assert(strcmp(ev2->as.error.message, "test error") == 0);
    mm_event_destroy(ev2);
    
    mm_event_t* ev3 = mm_event_new_state_changed(NULL, MM_STATE_NULL, MM_STATE_READY);
    assert(ev3 != NULL);
    assert(ev3->type == MM_EVENT_STATE_CHANGED);
    assert(ev3->as.state_changed.old_state == MM_STATE_NULL);
    assert(ev3->as.state_changed.new_state == MM_STATE_READY);
    mm_event_destroy(ev3);
    
    PASS();
}

static void
test_bus_basic(void)
{
    TEST("bus basic post / pop");
    
    mm_bus_t* bus = mm_bus_create();
    assert(bus != NULL);
    
    mm_event_t* ev = mm_event_new_eos(NULL);
    mm_result_t r = mm_bus_post(bus, ev);
    assert(r == MM_OK);
    
    mm_event_t* popped = NULL;
    r = mm_bus_pop(bus, &popped, 0);
    assert(r == MM_OK);
    assert(popped != NULL);
    assert(popped->type == MM_EVENT_EOS);
    
    mm_event_destroy(popped);
    mm_bus_destroy(bus);
    
    PASS();
}

static void
test_bus_timeout(void)
{
    TEST("bus pop timeout");
    
    mm_bus_t* bus = mm_bus_create();
    assert(bus != NULL);
    
    mm_event_t* popped = NULL;
    mm_result_t r = mm_bus_pop(bus, &popped, 10);
    assert(r == MM_TIMEOUT);
    assert(popped == NULL);
    
    mm_bus_destroy(bus);
    
    PASS();
}

static volatile int g_handler_called = 0;
static mm_event_type_t g_last_event_type;

static void
test_bus_handler_cb(mm_bus_t* bus, mm_event_t* event, void* user_data)
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
    
    mm_bus_t* bus = mm_bus_create();
    assert(bus != NULL);
    
    g_handler_called = 0;
    
    mm_result_t r = mm_bus_set_handler(bus, test_bus_handler_cb, NULL);
    assert(r == MM_OK);
    
    mm_event_t* ev = mm_event_new_eos(NULL);
    r = mm_bus_post(bus, ev);
    assert(r == MM_OK);
    
    /* Sleep a bit to allow dispatch thread to run */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 50000000 }; /* 50 ms */
    nanosleep(&ts, NULL);
    
    assert(g_handler_called == 1);
    assert(g_last_event_type == MM_EVENT_EOS);
    
    /* Remove handler (stops thread) */
    r = mm_bus_set_handler(bus, NULL, NULL);
    assert(r == MM_OK);
    
    mm_bus_destroy(bus);
    
    PASS();
}

static void
test_pipeline_bus_events(void)
{
    TEST("pipeline/element lifecycle events on bus");
    
    mm_pipeline_t* pipe = mm_pipeline_create();
    assert(pipe != NULL);
    assert(pipe->bus != NULL);
    
    mm_element_ops_t ops = { .name = "test_el" };
    mm_element_t* el = mm_element_create(&ops, NULL);
    assert(el != NULL);
    
    mm_result_t r = mm_pipeline_add(pipe, el);
    assert(r == MM_OK);
    assert(el->bus == pipe->bus);
    
    /* Transition pipeline state */
    r = mm_pipeline_set_state(pipe, MM_STATE_READY);
    assert(r == MM_OK);
    
    /* We expect two events: element state changed, then pipeline state changed */
    mm_event_t* ev = NULL;
    r = mm_bus_pop(pipe->bus, &ev, 100);
    assert(r == MM_OK);
    assert(ev != NULL);
    assert(ev->type == MM_EVENT_STATE_CHANGED);
    assert(ev->src == el);
    assert(ev->as.state_changed.old_state == MM_STATE_NULL);
    assert(ev->as.state_changed.new_state == MM_STATE_READY);
    mm_event_destroy(ev);
    
    ev = NULL;
    r = mm_bus_pop(pipe->bus, &ev, 100);
    assert(r == MM_OK);
    assert(ev != NULL);
    assert(ev->type == MM_EVENT_STATE_CHANGED);
    assert(ev->src == NULL); // Pipeline itself
    assert(ev->as.state_changed.old_state == MM_STATE_NULL);
    assert(ev->as.state_changed.new_state == MM_STATE_READY);
    mm_event_destroy(ev);
    
    mm_pipeline_destroy(pipe); // also destroys el and bus
    
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   Dynamic Plugins tests (Phase 7)
   ═══════════════════════════════════════════════════════════════ */
static void
test_plugin_registry_basic(void)
{
    TEST("plugin registry initialization and scanning");
    
    mm_result_t r = mm_plugin_registry_init();
    assert(r == MM_OK);
    
    r = mm_plugin_registry_scan("/workspace/build/plugins");
    assert(r == MM_OK);
    
    setenv("ZSTREAMER_PLUGIN_PATH", "/workspace/build/plugins", 1);
    r = mm_plugin_registry_scan_env();
    assert(r == MM_OK);
    
    PASS();
}

static void
test_element_factory_refcounting(void)
{
    TEST("element factory make and plugin refcounting");
    
    mm_plugin_registry_init();
    mm_plugin_registry_scan("/workspace/build/plugins");
    
    mm_element_t* filesink = mm_element_factory_make("filesink");
    assert(filesink != NULL);
    assert(filesink->plugin != NULL);
    assert(strcmp(filesink->ops->name, "filesink") == 0);
    
    mm_element_t* v4l2source = mm_element_factory_make("v4l2src");
    assert(v4l2source != NULL);
    assert(v4l2source->plugin != NULL);
    assert(strcmp(v4l2source->ops->name, "v4l2src") == 0);
    
    mm_element_t* alsasource = mm_element_factory_make("alsasrc");
    assert(alsasource != NULL);
    assert(alsasource->plugin != NULL);
    assert(strcmp(alsasource->ops->name, "alsasrc") == 0);
    
    mm_element_t* h264encoder = mm_element_factory_make("h264enc");
    assert(h264encoder != NULL);
    assert(h264encoder->plugin != NULL);
    assert(strcmp(h264encoder->ops->name, "h264enc") == 0);
    
    mm_element_t* aacencoder = mm_element_factory_make("aacenc");
    assert(aacencoder != NULL);
    assert(aacencoder->plugin != NULL);
    assert(strcmp(aacencoder->ops->name, "aacenc") == 0);
    
    mm_element_t* mp4muxer = mm_element_factory_make("mp4mux");
    assert(mp4muxer != NULL);
    assert(mp4muxer->plugin != NULL);
    assert(strcmp(mp4muxer->ops->name, "mp4mux") == 0);
    
    mm_plugin_t* filesink_plugin = filesink->plugin;
    assert(filesink_plugin->refcount == 2);
    
    mm_element_destroy(filesink);
    assert(filesink_plugin->refcount == 1);
    
    mm_element_destroy(v4l2source);
    mm_element_destroy(alsasource);
    mm_element_destroy(h264encoder);
    mm_element_destroy(aacencoder);
    mm_element_destroy(mp4muxer);
    
    mm_plugin_registry_deinit();
    
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   Main
   ═══════════════════════════════════════════════════════════════ */
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

    /* ── Summary ── */
    printf("\n──────────────────────────────────────────────────\n");
    printf("  %d / %d tests passed\n", g_tests_passed, g_tests_run);
    printf("──────────────────────────────────────────────────\n\n");

    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
