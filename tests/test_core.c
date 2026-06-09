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

    /* ── Summary ── */
    printf("\n──────────────────────────────────────────────────\n");
    printf("  %d / %d tests passed\n", g_tests_passed, g_tests_run);
    printf("──────────────────────────────────────────────────\n\n");

    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
