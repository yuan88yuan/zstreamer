#include <stdio.h>
#include <stdlib.h>

#include "zst_pipeline.h"
#include "zst_log.h"
#include "zst_scheduler.h"
#include "zst_pad.h"
#include "zst_element.h"
#include "zst_queue.h"
#include "zst_element_factory.h"
#include "zst_plugin.h"

int main(void)
{
    zst_pipeline_t* pipe;
    zst_scheduler_t* sched;

    zst_element_t* video_src;
    zst_element_t* text_overlay;
    zst_element_t* q_video_enc;
    zst_element_t* h264_enc;
    zst_element_t* sink;

    zst_scheduler_config_t sched_cfg = {
        .mode = ZST_SCHEDULER_MULTI_THREAD,
        .worker_threads = 4
    };

    pipe = zst_pipeline_create();
    sched = zst_scheduler_create(&sched_cfg);

    zst_plugin_registry_init();
    zst_plugin_registry_scan("plugins");

    video_src = zst_element_factory_make("v4l2src");
    text_overlay = zst_element_factory_make("textoverlay");
    zst_element_set_property_string(text_overlay, "text", "Integration Test\nTimestamp: 00:00:00");
    zst_element_set_property_string(text_overlay, "color", "#00FF00"); // green
    zst_element_set_property_string(text_overlay, "position", "bottom-right");
    zst_element_set_property_string(text_overlay, "font_size", "64");

    q_video_enc = zst_queue_element_create(NULL);
    h264_enc = zst_element_factory_make("x264enc");
    sink = zst_element_factory_make("filesink");
    zst_element_set_property_string(sink, "path", "output_text.h264");

    zst_pipeline_add(pipe, video_src);
    zst_pipeline_add(pipe, text_overlay);
    zst_pipeline_add(pipe, h264_enc);
    zst_pipeline_add(pipe, q_video_enc);
    zst_pipeline_add(pipe, sink);

    zst_pad_link(
        zst_element_get_pad(video_src, "src"),
        zst_element_get_pad(text_overlay, "sink"));

    zst_pad_link(
        zst_element_get_pad(text_overlay, "src"),
        zst_element_get_pad(h264_enc, "sink"));

    zst_pad_link(
        zst_element_get_pad(h264_enc, "src"),
        zst_element_get_pad(q_video_enc, "sink"));

    zst_pad_link(
        zst_element_get_pad(q_video_enc, "src"),
        zst_element_get_pad(sink, "sink"));

    zst_pipeline_set_state(pipe, ZST_STATE_READY);
    zst_pipeline_set_state(pipe, ZST_STATE_PLAYING);

    zst_scheduler_attach(sched, pipe);
    zst_scheduler_run(sched);

    ZST_LOG_INFO("example", "recording video with text overlay to output_text.h264... press enter to stop");

    getchar();

    zst_pipeline_set_state(pipe, ZST_STATE_NULL);
    zst_scheduler_stop(sched);
    zst_scheduler_destroy(sched);
    zst_pipeline_destroy(pipe);

    return 0;
}
