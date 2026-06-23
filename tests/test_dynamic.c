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

static void on_bus_event(zst_bus_t* bus, zst_event_t* ev, void* user_data)
{
    (void)bus; (void)user_data;
    if (ev->type == ZST_EVENT_PAD_ADDED) {
        g_pad_added_count++;
        printf("Bus: Pad added: %s\n", ev->as.pad_added.pad->name);
    } else if (ev->type == ZST_EVENT_PAD_REMOVED) {
        g_pad_removed_count++;
        printf("Bus: Pad removed: %s\n", ev->as.pad_removed.pad->name);
    }
}

static zst_element_ops_t g_test_ops = {.name = "test"};

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

    zst_pad_t** snapshot = NULL;
    uint32_t count = 0;
    zst_element_snapshot_src_pads(el, &snapshot, &count);
    assert(count == 1);
    assert(snapshot[0] == pad);
    zst_element_pad_snapshot_free(snapshot, count);

    zst_stream_info_t query_info;
    assert(zst_element_get_stream_info(el, 0, &query_info) == ZST_OK);
    assert(query_info.id == 0); /* Fallback in default impl */
    assert(strcmp(query_info.name, "src_0") == 0);
    free(query_info.name);

    zst_element_remove_dynamic_pad(el, pad);
    assert(el->nb_src_pads == 0);

    /* Process bus events */
    zst_event_t* ev = NULL;
    while (zst_bus_pop(bus, &ev, 1) == ZST_OK) {
        on_bus_event(bus, ev, NULL);
        zst_event_destroy(ev);
    }

    assert(g_pad_added_count == 1);
    assert(g_pad_removed_count == 1);

    zst_pipeline_destroy(pipe);

    printf("Dynamic test passed!\n");
    return 0;
}
