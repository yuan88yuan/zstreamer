/*=============================================================================
    rtmp_sink.c — RTMP sink element using FFmpeg/libavformat
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>

#include "zst_element.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_caps.h"
#include "zst_log.h"
#include "zst_bus.h"

typedef struct {
    AVFormatContext* fc;
    int              video_stream_idx;
    int              audio_stream_idx;
    int              video_eos;
    int              audio_eos;
    int              header_written;
    char             url[512];

    int              video_linked;
    int              audio_linked;
} rtmp_sink_t;

static zst_result_t
rtmp_sink_write_header(zst_element_t* el)
{
    rtmp_sink_t* s = el->priv;

    if (avformat_alloc_output_context2(&s->fc, NULL, "flv", s->url) < 0 || !s->fc) {
        ZST_LOG_ERROR("rtmpsink", "Failed to create FLV output context");
        return ZST_ERROR;
    }

    if (avio_open2(&s->fc->pb, s->url, AVIO_FLAG_WRITE, NULL, NULL) < 0) {
        ZST_LOG_ERROR("rtmpsink", "Failed to open RTMP URL: %s", s->url);
        avformat_free_context(s->fc);
        s->fc = NULL;
        return ZST_ERROR;
    }

    int stream_count = 0;

    if (s->video_linked) {
        AVStream* st = avformat_new_stream(s->fc, NULL);
        if (!st) return ZST_ERROR;
        st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        st->codecpar->codec_id = AV_CODEC_ID_H264;
        st->time_base = (AVRational){1, 1000};
        s->video_stream_idx = stream_count++;
    }

    if (s->audio_linked) {
        AVStream* st = avformat_new_stream(s->fc, NULL);
        if (!st) return ZST_ERROR;
        st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        st->codecpar->codec_id = AV_CODEC_ID_AAC;
        st->time_base = (AVRational){1, 1000};
        s->audio_stream_idx = stream_count++;
    }

    if (avformat_write_header(s->fc, NULL) < 0) {
        ZST_LOG_ERROR("rtmpsink", "Failed to write header to RTMP stream");
        return ZST_ERROR;
    }

    s->header_written = 1;
    s->video_eos = 0;
    s->audio_eos = 0;
    ZST_LOG_INFO("rtmpsink", "Started RTMP stream to %s", s->url);
    return ZST_OK;
}

static zst_result_t
rtmp_sink_write(zst_element_t* el, zst_buffer_t* buf, int stream_idx)
{
    rtmp_sink_t* s = el->priv;

    if (!s->header_written) return ZST_ERROR;

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return ZST_ERROR;

    int packet_size = (int)buf->memory.size;

    if (av_new_packet(pkt, packet_size) < 0) {
        av_packet_free(&pkt);
        return ZST_ERROR;
    }
    memcpy(pkt->data, buf->memory.data, packet_size);

    pkt->pts = av_rescale_q(buf->pts, (AVRational){1, 1000000000}, s->fc->streams[stream_idx]->time_base);
    pkt->dts = av_rescale_q(buf->dts, (AVRational){1, 1000000000}, s->fc->streams[stream_idx]->time_base);
    pkt->duration = av_rescale_q(buf->duration, (AVRational){1, 1000000000}, s->fc->streams[stream_idx]->time_base);
    pkt->stream_index = stream_idx;

    if (av_interleaved_write_frame(s->fc, pkt) < 0) {
        av_packet_free(&pkt);
        return ZST_ERROR;
    }

    av_packet_free(&pkt);
    return ZST_OK;
}

static void
rtmp_sink_check_eos(zst_element_t* el)
{
    rtmp_sink_t* s = el->priv;
    int all_eos = 1;
    if (s->video_linked && !s->video_eos) all_eos = 0;
    if (s->audio_linked && !s->audio_eos) all_eos = 0;

    if (all_eos && el->bus) {
        zst_bus_post(el->bus, zst_event_new_eos(el));
    }
}

static zst_result_t
rtmp_sink_video_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    zst_element_t* el = pad->parent;
    rtmp_sink_t* s = el->priv;

    if (buf->flags & ZST_BUFFER_FLAG_EOS) {
        s->video_eos = 1;
        rtmp_sink_check_eos(el);
        return ZST_OK;
    }

    if (!s->header_written) {
        if (rtmp_sink_write_header(el) != ZST_OK) return ZST_ERROR;
    }

    if (s->video_stream_idx >= 0) {
        return rtmp_sink_write(el, buf, s->video_stream_idx);
    }
    return ZST_OK;
}

static zst_result_t
rtmp_sink_audio_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    zst_element_t* el = pad->parent;
    rtmp_sink_t* s = el->priv;

    if (buf->flags & ZST_BUFFER_FLAG_EOS) {
        s->audio_eos = 1;
        rtmp_sink_check_eos(el);
        return ZST_OK;
    }

    if (!s->header_written) {
        if (rtmp_sink_write_header(el) != ZST_OK) return ZST_ERROR;
    }

    if (s->audio_stream_idx >= 0) {
        return rtmp_sink_write(el, buf, s->audio_stream_idx);
    }
    return ZST_OK;
}

static zst_result_t
rtmp_sink_start(zst_element_t* el)
{
    rtmp_sink_t* s = el->priv;
    if (s->url[0] == '\0') {
        ZST_LOG_ERROR("rtmpsink", "RTMP URL not set");
        return ZST_ERROR;
    }

    zst_pad_t* video_pad = zst_element_get_pad(el, "video");
    zst_pad_t* audio_pad = zst_element_get_pad(el, "audio");
    s->video_linked = (video_pad && video_pad->peer) ? 1 : 0;
    s->audio_linked = (audio_pad && audio_pad->peer) ? 1 : 0;
    s->video_stream_idx = -1;
    s->audio_stream_idx = -1;
    s->header_written = 0;
    s->video_eos = 0;
    s->audio_eos = 0;
    return ZST_OK;
}

static zst_result_t
rtmp_sink_stop(zst_element_t* el)
{
    rtmp_sink_t* s = el->priv;
    if (s->fc && s->header_written) {
        av_write_trailer(s->fc);
    }

    if (s->fc) {
        if (s->fc->pb) {
            avio_closep(&s->fc->pb);
        }
        avformat_free_context(s->fc);
        s->fc = NULL;
    }
    s->header_written = 0;
    return ZST_OK;
}

static zst_result_t
rtmp_sink_set_property(zst_element_t* el, const char* name, const char* value)
{
    rtmp_sink_t* s = el->priv;
    if (strcmp(name, "url") == 0) {
        snprintf(s->url, sizeof(s->url), "%s", value);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t
rtmp_sink_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    rtmp_sink_t* s = el->priv;
    if (strcmp(name, "url") == 0) {
        snprintf(value_out, max_len, "%s", s->url);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_element_ops_t g_ops = {
    .name  = "rtmpsink",
    .start = rtmp_sink_start,
    .stop  = rtmp_sink_stop,
    .set_property = rtmp_sink_set_property,
    .get_property = rtmp_sink_get_property,
};

zst_element_t*
zst_rtmp_sink_create(void)
{
    zst_element_t* el;
    rtmp_sink_t* priv;
    zst_pad_t* video;
    zst_pad_t* audio;

    priv = calloc(1, sizeof(*priv));

    el = zst_element_create(&g_ops, priv);

    video = zst_pad_create("video", ZST_PAD_SINK);
    audio = zst_pad_create("audio", ZST_PAD_SINK);

    video->push = rtmp_sink_video_push;
    audio->push = rtmp_sink_audio_push;

    zst_element_add_pad(el, video);
    zst_element_add_pad(el, audio);

    return el;
}

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "rtmpsink") == 0) {
        return zst_rtmp_sink_create();
    }
    return NULL;
}

static const zst_pad_template_t g_rtmpsink_pads[] = {
    { "video", ZST_PAD_SINK, "video/x-h264" },
    { "audio", ZST_PAD_SINK, "audio/aac" }
};

static const zst_element_desc_t g_rtmpsink_elements[] = {
    {
        .name = "rtmpsink",
        .long_name = "RTMP Sink",
        .category = "Sink/Network",
        .description = "Publishes audio/video to an RTMP endpoint",
        .author = "zstreamer",
        .properties = NULL,
        .nb_properties = 0,
        .pads = g_rtmpsink_pads,
        .nb_pads = sizeof(g_rtmpsink_pads) / sizeof(g_rtmpsink_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "rtmpsink_plugin",
        .author = "zstreamer",
        .version = "1.0.0",
        .init = NULL,
        .deinit = NULL
    },
    .create_element = plugin_create_element
};

ZST_PLUGIN_EXPORT
const zst_element_desc_t*
zst_get_plugin_elements(uint32_t* nb_elements_out)
{
    if (nb_elements_out) {
        *nb_elements_out = sizeof(g_rtmpsink_elements) / sizeof(g_rtmpsink_elements[0]);
    }
    return g_rtmpsink_elements;
}

ZST_PLUGIN_EXPORT
zst_plugin_t*
zst_get_plugin(void)
{
    zst_plugin_t* p = malloc(sizeof(*p));
    if (p) {
        *p = g_plugin;
    }
    return p;
}
#endif
