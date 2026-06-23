#include "zst_pipeline.h"
#include "zst_element.h"
#include "zst_pad.h"
#include "zst_bus.h"
#include "zst_stream.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

static int g_pad_added_count = 0;
static int g_pad_removed_count = 0;
static int g_stream_added_count = 0;
static int g_stream_removed_count = 0;
static int g_sink_event_count = 0;

static void on_bus_event(zst_bus_t* bus, zst_event_t* ev, void* user_data)
{
    (void)bus; (void)user_data;
    if (ev->type == ZST_EVENT_PAD_ADDED) {
        g_pad_added_count++;
        printf("Bus: Pad added: %s\n", ev->as.pad_added.pad->name);
    } else if (ev->type == ZST_EVENT_PAD_REMOVED) {
        g_pad_removed_count++;
        printf("Bus: Pad removed: %s\n", ev->as.pad_removed.pad->name);
    } else if (ev->type == ZST_EVENT_STREAM_ADDED) {
        g_stream_added_count++;
        assert(ev->as.stream_status.stream.id == 100);
    } else if (ev->type == ZST_EVENT_STREAM_REMOVED) {
        g_stream_removed_count++;
        assert(ev->as.stream_removed.stream_id == 100);
    }
}

static zst_result_t sink_event(zst_element_t* el, zst_pad_t* sink_pad, zst_pad_event_t* event)
{
    (void)el; (void)sink_pad;
    if (event->type == ZST_PAD_EVENT_STREAM_START) {
        assert(event->as.stream_start.stream_id == 100);
        g_sink_event_count++;
    }
    return ZST_OK;
}

static zst_element_ops_t g_test_ops = {.name = "test"};
static zst_element_ops_t g_sink_ops = {.name = "sink", .event = sink_event};

int main()
{
    zst_pipeline_t* pipe = zst_pipeline_create();
    zst_bus_t* bus = zst_pipeline_get_bus(pipe);
    /* DON'T use handler, use manual pop to avoid thread timing issues in this simple test */
    // zst_bus_set_handler(bus, on_bus_event, NULL);

    zst_element_t* el = zst_element_create(&g_test_ops, NULL);
    zst_pipeline_add(pipe, el);

    zst_pad_t* pad = zst_pad_create("src_0", ZST_PAD_SRC);
    zst_stream_info_t info = {
        .struct_size = sizeof(info),
        .id = 100,
        .kind = ZST_MEDIA_VIDEO,
        .name = "Test Stream"
    };

    zst_element_add_dynamic_pad(el, pad, &info);
    assert(el->nb_src_pads == 1);
    assert(zst_element_get_stream_count(el) == 1);

    zst_pad_t** snapshot = NULL;
    uint32_t count = 0;
    zst_element_snapshot_src_pads(el, &snapshot, &count);
    assert(count == 1);
    assert(snapshot[0] == pad);
    zst_element_pad_snapshot_free(snapshot, count);

    zst_stream_info_t query_info;
    assert(zst_element_get_stream_info(el, 0, &query_info) == ZST_OK);
    assert(query_info.id == 100);
    assert(query_info.kind == ZST_MEDIA_VIDEO);
    assert(strcmp(query_info.name, "Test Stream") == 0);
    zst_stream_info_clear(&query_info);
    assert(zst_element_get_stream_pad(el, 100) == pad);

    zst_pad_event_t* stream_start = zst_pad_event_new_stream_start(100);
    assert(zst_pad_push_event(pad, stream_start) == ZST_OK);
    zst_pad_event_unref(stream_start);

    zst_element_t* sink_el = zst_element_create(&g_sink_ops, NULL);
    zst_pad_t* sink_pad = zst_pad_create("sink", ZST_PAD_SINK);
    zst_element_add_pad(sink_el, sink_pad);
    zst_pipeline_add(pipe, sink_el);
    assert(zst_pad_link(pad, sink_pad) == ZST_OK);
    assert(g_sink_event_count == 1);

    zst_element_remove_dynamic_pad(el, pad);
    assert(sink_pad->peer == NULL);
    assert(el->nb_src_pads == 0);

    /* Process bus events */
    zst_event_t* ev = NULL;
    while (zst_bus_pop(bus, &ev, 1) == ZST_OK) {
        on_bus_event(bus, ev, NULL);
        zst_event_destroy(ev);
    }

    assert(g_pad_added_count == 1);
    assert(g_pad_removed_count == 1);
    assert(g_stream_added_count == 1);
    assert(g_stream_removed_count == 1);

    zst_pipeline_destroy(pipe);

    printf("Dynamic test passed!\n");
    return 0;
}
