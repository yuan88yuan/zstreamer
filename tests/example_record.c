#include <stdio.h>
#include <stdlib.h>

#include "zst_pipeline.h"
#include "zst_log.h"
#include "zst_scheduler.h"
#include "zst_pad.h"
#include "zst_element.h"
#include "zst_queue.h"

/* plugin elements */
zst_element_t* zst_v4l2_source_create(void);
zst_element_t* zst_alsa_source_create(void);

zst_element_t* zst_h264_encoder_create(void);
zst_element_t* zst_aac_encoder_create(void);

zst_element_t* zst_mp4_muxer_create(void);
zst_element_t* zst_file_sink_create(const char* path);

int main(void)
{
    zst_pipeline_t* pipe;

    zst_scheduler_t* sched;

    zst_element_t* video_src;
    zst_element_t* audio_src;

    zst_element_t* q_video_src;
    zst_element_t* q_video_enc;
    zst_element_t* q_audio_src;
    zst_element_t* q_audio_enc;
    zst_element_t* q_mux;

    zst_element_t* h264_enc;
    zst_element_t* aac_enc;

    zst_element_t* mux;
    zst_element_t* sink;

    zst_scheduler_config_t sched_cfg = {
        .mode = ZST_SCHEDULER_MULTI_THREAD,
        .worker_threads = 4
    };

    /*--------------------------------------------------------
        create pipeline
    --------------------------------------------------------*/
    pipe = zst_pipeline_create();

    sched = zst_scheduler_create(&sched_cfg);

    /*--------------------------------------------------------
        create elements
    --------------------------------------------------------*/

    video_src = zst_v4l2_source_create();
    audio_src = zst_alsa_source_create();

    q_video_src = zst_queue_element_create(NULL);
    q_video_enc = zst_queue_element_create(NULL);
    q_audio_src = zst_queue_element_create(NULL);
    q_audio_enc = zst_queue_element_create(NULL);
    q_mux       = zst_queue_element_create(NULL);

    h264_enc = zst_h264_encoder_create();
    aac_enc = zst_aac_encoder_create();

    mux = zst_mp4_muxer_create();

    sink = zst_file_sink_create("output.mp4");

    /*--------------------------------------------------------
        add elements
    --------------------------------------------------------*/

    zst_pipeline_add(pipe, video_src);
    zst_pipeline_add(pipe, q_video_src);
    zst_pipeline_add(pipe, h264_enc);
    zst_pipeline_add(pipe, q_video_enc);
    zst_pipeline_add(pipe, mux);
    zst_pipeline_add(pipe, q_mux);
    zst_pipeline_add(pipe, sink);

    zst_pipeline_add(pipe, audio_src);
    zst_pipeline_add(pipe, q_audio_src);
    zst_pipeline_add(pipe, aac_enc);
    zst_pipeline_add(pipe, q_audio_enc);

    /*--------------------------------------------------------
        link video path
    --------------------------------------------------------*/

    zst_pad_link(
        zst_element_get_pad(video_src, "src"),
        zst_element_get_pad(q_video_src, "sink"));

    zst_pad_link(
        zst_element_get_pad(q_video_src, "src"),
        zst_element_get_pad(h264_enc, "sink"));

    zst_pad_link(
        zst_element_get_pad(h264_enc, "src"),
        zst_element_get_pad(q_video_enc, "sink"));

    zst_pad_link(
        zst_element_get_pad(q_video_enc, "src"),
        zst_element_get_pad(mux, "video"));

    /*--------------------------------------------------------
        link audio path
    --------------------------------------------------------*/

    zst_pad_link(
        zst_element_get_pad(audio_src, "src"),
        zst_element_get_pad(q_audio_src, "sink"));

    zst_pad_link(
        zst_element_get_pad(q_audio_src, "src"),
        zst_element_get_pad(aac_enc, "sink"));

    zst_pad_link(
        zst_element_get_pad(aac_enc, "src"),
        zst_element_get_pad(q_audio_enc, "sink"));

    zst_pad_link(
        zst_element_get_pad(q_audio_enc, "src"),
        zst_element_get_pad(mux, "audio"));

    /*--------------------------------------------------------
        link mux -> sink
    --------------------------------------------------------*/

    zst_pad_link(
        zst_element_get_pad(mux, "src"),
        zst_element_get_pad(q_mux, "sink"));

    zst_pad_link(
        zst_element_get_pad(q_mux, "src"),
        zst_element_get_pad(sink, "sink"));

    /*--------------------------------------------------------
        start pipeline
    --------------------------------------------------------*/

    zst_pipeline_set_state(pipe, ZST_STATE_READY);

    zst_pipeline_set_state(pipe, ZST_STATE_PLAYING);

    zst_scheduler_attach(sched, pipe);

    zst_scheduler_run(sched);

    ZST_LOG_INFO("example", "recording...");

    getchar();

    /*--------------------------------------------------------
        stop
    --------------------------------------------------------*/

    zst_pipeline_set_state(pipe, ZST_STATE_NULL);

    zst_scheduler_stop(sched);

    zst_scheduler_destroy(sched);

    zst_pipeline_destroy(pipe);

    return 0;
}