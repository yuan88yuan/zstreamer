#include "zst_pipeline.h"
#include "zst_element.h"
#include "zst_pad.h"
#include "zst_scheduler.h"
#include "zst_buffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>
#include <pthread.h>

static void mock_buf_destroy(zst_buffer_t* b) {
    free(b->payload);
}

static zst_result_t
mock_source_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    (void)in;
    int* counter = el->priv;
    if (*counter >= 100) {
        return ZST_EOF;
    }

    /* Pacing: sleep for 1 millisecond to prevent generating all buffers instantly */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 }; /* 1 ms */
    nanosleep(&ts, NULL);

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
    *data = val * 10;
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
    if (!in || !in->payload) return ZST_ERROR;

    mock_sink_t* sink_data = el->priv;
    int val = *(int*)in->payload;
    sink_data->count++;
    sink_data->sum += val;

    return ZST_OK;
}

int main()
{
    printf("Starting dynamic reconfiguration test...\n");

    zst_pipeline_t* pipe = zst_pipeline_create();
    zst_scheduler_config_t cfg = {
        .mode = ZST_SCHEDULER_MULTI_THREAD,
        .worker_threads = 2
    };
    zst_scheduler_t* sched = zst_scheduler_create(&cfg);
    zst_scheduler_attach(sched, pipe);

    /* Create source */
    int* source_counter = malloc(sizeof(int));
    *source_counter = 0;
    static zst_element_ops_t source_ops = {
        .name = "mock_source",
        .process = mock_source_process
    };
    zst_element_t* source = zst_element_create(&source_ops, source_counter);
    zst_pad_t* src_pad = zst_pad_create("src", ZST_PAD_SRC);
    zst_element_add_pad(source, src_pad);

    /* Create sink */
    mock_sink_t* sink_data = calloc(1, sizeof(mock_sink_t));
    static zst_element_ops_t sink_ops = {
        .name = "mock_sink",
        .process = mock_sink_process
    };
    zst_element_t* sink = zst_element_create(&sink_ops, sink_data);
    zst_pad_t* sink_pad = zst_pad_create("sink", ZST_PAD_SINK);
    zst_element_add_pad(sink, sink_pad);

    /* Add elements & link source -> sink directly first */
    zst_pipeline_add(pipe, source);
    zst_pipeline_add(pipe, sink);
    assert(zst_pad_link(src_pad, sink_pad) == ZST_OK);

    /* Start pipeline and scheduler */
    assert(zst_pipeline_set_state(pipe, ZST_STATE_PLAYING) == ZST_OK);
    assert(zst_scheduler_run(sched) == ZST_OK);

    /* Let it run for a brief moment */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 15000000 }; /* 15 ms */
    nanosleep(&ts, NULL);

    /* Verify we processed some items directly (e.g. 1, 2, 3...) */
    assert(sink_data->count > 0);
    int initial_count = sink_data->count;
    int initial_sum = sink_data->sum;
    printf("Before reconfiguration: processed %d buffers (sum = %d)\n", initial_count, initial_sum);

    /* Create dynamic transform element */
    static zst_element_ops_t transform_ops = {
        .name = "mock_transform",
        .process = mock_transform_process
    };
    zst_element_t* transform = zst_element_create(&transform_ops, NULL);
    zst_pad_t* trans_sink = zst_pad_create("sink", ZST_PAD_SINK);
    zst_pad_t* trans_src = zst_pad_create("src", ZST_PAD_SRC);
    zst_element_add_pad(transform, trans_sink);
    zst_element_add_pad(transform, trans_src);

    /* Reconfigure transaction */
    assert(zst_pipeline_reconfigure_begin(pipe) == ZST_OK);

    /* Add transform dynamically */
    assert(zst_pipeline_add_element_dynamic(pipe, transform) == ZST_OK);

    /* Refuse stale/wrong sink arguments without unlinking the current peer. */
    assert(zst_pipeline_unlink_pads_dynamic(pipe, src_pad, trans_sink) == ZST_ERROR);
    assert(zst_pad_is_linked(src_pad));

    /* Unlink src_pad and sink_pad */
    assert(zst_pipeline_unlink_pads_dynamic(pipe, src_pad, sink_pad) == ZST_OK);

    /* Link src_pad -> trans_sink and trans_src -> sink_pad */
    assert(zst_pipeline_link_pads_dynamic(pipe, src_pad, trans_sink) == ZST_OK);
    assert(zst_pipeline_link_pads_dynamic(pipe, trans_src, sink_pad) == ZST_OK);

    /* Finish reconfiguration */
    assert(zst_pipeline_reconfigure_end(pipe) == ZST_OK);

    printf("Reconfiguration completed. Letting the pipeline run...\n");

    /* Let it run more to process remaining elements through transform */
    nanosleep(&ts, NULL);

    /* Stop pipeline */
    zst_pipeline_set_state(pipe, ZST_STATE_NULL);
    zst_scheduler_stop(sched);

    int final_count = sink_data->count;
    int final_sum = sink_data->sum;
    printf("After reconfiguration: final processed count = %d, final sum = %d\n", final_count, final_sum);

    /* Some buffers should have been multiplied by 10 */
    assert(final_count > initial_count);
    assert(final_sum > initial_sum);
    int diff_count = final_count - initial_count;
    int min_expected_diff_sum = diff_count * 10;
    assert(final_sum - initial_sum >= min_expected_diff_sum);

    /* Cleanup */
    zst_scheduler_destroy(sched);
    zst_pipeline_destroy(pipe);

    printf("Dynamic reconfiguration test PASSED!\n");
    return 0;
}
