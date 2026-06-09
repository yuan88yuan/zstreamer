#include <stdio.h>
#include <stdlib.h>

#include "mm_pipeline.h"
#include "mm_scheduler.h"
#include "mm_pad.h"
#include "mm_element.h"
#include "mm_queue.h"

/* plugin elements */
mm_element_t* mm_v4l2_source_create(void);
mm_element_t* mm_alsa_source_create(void);

mm_element_t* mm_h264_encoder_create(void);
mm_element_t* mm_aac_encoder_create(void);

mm_element_t* mm_mp4_muxer_create(void);
mm_element_t* mm_file_sink_create(const char* path);

int main(void)
{
    mm_pipeline_t* pipe;

    mm_scheduler_t* sched;

    mm_element_t* video_src;
    mm_element_t* audio_src;

    mm_element_t* q_video_src;
    mm_element_t* q_video_enc;
    mm_element_t* q_audio_src;
    mm_element_t* q_audio_enc;
    mm_element_t* q_mux;

    mm_element_t* h264_enc;
    mm_element_t* aac_enc;

    mm_element_t* mux;
    mm_element_t* sink;

    mm_scheduler_config_t sched_cfg = {
        .mode = MM_SCHEDULER_MULTI_THREAD,
        .worker_threads = 4
    };

    /*--------------------------------------------------------
        create pipeline
    --------------------------------------------------------*/
    pipe = mm_pipeline_create();

    sched = mm_scheduler_create(&sched_cfg);

    /*--------------------------------------------------------
        create elements
    --------------------------------------------------------*/

    video_src = mm_v4l2_source_create();
    audio_src = mm_alsa_source_create();

    q_video_src = mm_queue_element_create(NULL);
    q_video_enc = mm_queue_element_create(NULL);
    q_audio_src = mm_queue_element_create(NULL);
    q_audio_enc = mm_queue_element_create(NULL);
    q_mux       = mm_queue_element_create(NULL);

    h264_enc = mm_h264_encoder_create();
    aac_enc = mm_aac_encoder_create();

    mux = mm_mp4_muxer_create();

    sink = mm_file_sink_create("output.mp4");

    /*--------------------------------------------------------
        add elements
    --------------------------------------------------------*/

    mm_pipeline_add(pipe, video_src);
    mm_pipeline_add(pipe, q_video_src);
    mm_pipeline_add(pipe, h264_enc);
    mm_pipeline_add(pipe, q_video_enc);
    mm_pipeline_add(pipe, mux);
    mm_pipeline_add(pipe, q_mux);
    mm_pipeline_add(pipe, sink);

    mm_pipeline_add(pipe, audio_src);
    mm_pipeline_add(pipe, q_audio_src);
    mm_pipeline_add(pipe, aac_enc);
    mm_pipeline_add(pipe, q_audio_enc);

    /*--------------------------------------------------------
        link video path
    --------------------------------------------------------*/

    mm_pad_link(
        mm_element_get_pad(video_src, "src"),
        mm_element_get_pad(q_video_src, "sink"));

    mm_pad_link(
        mm_element_get_pad(q_video_src, "src"),
        mm_element_get_pad(h264_enc, "sink"));

    mm_pad_link(
        mm_element_get_pad(h264_enc, "src"),
        mm_element_get_pad(q_video_enc, "sink"));

    mm_pad_link(
        mm_element_get_pad(q_video_enc, "src"),
        mm_element_get_pad(mux, "video"));

    /*--------------------------------------------------------
        link audio path
    --------------------------------------------------------*/

    mm_pad_link(
        mm_element_get_pad(audio_src, "src"),
        mm_element_get_pad(q_audio_src, "sink"));

    mm_pad_link(
        mm_element_get_pad(q_audio_src, "src"),
        mm_element_get_pad(aac_enc, "sink"));

    mm_pad_link(
        mm_element_get_pad(aac_enc, "src"),
        mm_element_get_pad(q_audio_enc, "sink"));

    mm_pad_link(
        mm_element_get_pad(q_audio_enc, "src"),
        mm_element_get_pad(mux, "audio"));

    /*--------------------------------------------------------
        link mux -> sink
    --------------------------------------------------------*/

    mm_pad_link(
        mm_element_get_pad(mux, "src"),
        mm_element_get_pad(q_mux, "sink"));

    mm_pad_link(
        mm_element_get_pad(q_mux, "src"),
        mm_element_get_pad(sink, "sink"));

    /*--------------------------------------------------------
        start pipeline
    --------------------------------------------------------*/

    mm_pipeline_set_state(pipe, MM_STATE_READY);

    mm_pipeline_set_state(pipe, MM_STATE_PLAYING);

    mm_scheduler_attach(sched, pipe);

    mm_scheduler_run(sched);

    printf("recording...\n");

    getchar();

    /*--------------------------------------------------------
        stop
    --------------------------------------------------------*/

    mm_pipeline_set_state(pipe, MM_STATE_NULL);

    mm_scheduler_stop(sched);

    mm_scheduler_destroy(sched);

    mm_pipeline_destroy(pipe);

    return 0;
}