/*=============================================================================
    rtsp_source.c — RTSP source element using FFmpeg/libavformat
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>

#include "zst_element.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_caps.h"
#include "zst_log.h"

typedef struct {
    AVFormatContext* fmt_ctx;
    int              video_stream_index;
    int              audio_stream_index;
    char             url[512];
    char             transport[32];
    zst_pad_t*       video_pad;
    zst_pad_t*       audio_pad;
    zst_caps_t*      video_caps;
    zst_caps_t*      audio_caps;
} rtsp_source_t;

static const char*
rtsp_codec_id_to_media_type(enum AVCodecID codec_id)
{
    switch (codec_id) {
    case AV_CODEC_ID_H264:
        return "video/x-h264";
    case AV_CODEC_ID_HEVC:
        return "video/x-h265";
    case AV_CODEC_ID_AAC:
        return "audio/aac";
    case AV_CODEC_ID_MP3:
        return "audio/mpeg";
    default:
        return NULL;
    }
}

static zst_caps_t*
rtsp_caps_for_video_stream(AVStream* stream)
{
    if (!stream || !stream->codecpar) return NULL;

    const char* media_type = rtsp_codec_id_to_media_type(stream->codecpar->codec_id);
    if (!media_type) return NULL;

    double framerate = 0.0;
    if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0) {
        framerate = av_q2d(stream->avg_frame_rate);
    }

    zst_caps_t* caps = zst_caps_create();
    if (!caps) return NULL;

    zst_caps_append(caps, zst_caps_struct_create_video(
        media_type,
        stream->codecpar->width,
        stream->codecpar->height,
        framerate,
        ""));

    return caps;
}

static zst_caps_t*
rtsp_caps_for_audio_stream(AVStream* stream)
{
    if (!stream || !stream->codecpar) return NULL;

    const char* media_type = rtsp_codec_id_to_media_type(stream->codecpar->codec_id);
    if (!media_type) return NULL;

    zst_caps_t* caps = zst_caps_create();
    if (!caps) return NULL;

    zst_caps_append(caps, zst_caps_struct_create_audio(
        media_type,
        stream->codecpar->channels,
        stream->codecpar->sample_rate,
        ""));

    return caps;
}

static zst_caps_t*
rtsp_video_fallback_caps(void)
{
    zst_caps_t* caps = zst_caps_create();
    if (!caps) return NULL;
    zst_caps_append(caps, zst_caps_struct_create_video("video/x-h264", 0, 0, 0.0, ""));
    zst_caps_append(caps, zst_caps_struct_create_video("video/x-h265", 0, 0, 0.0, ""));
    return caps;
}

static zst_caps_t*
rtsp_audio_fallback_caps(void)
{
    zst_caps_t* caps = zst_caps_create();
    if (!caps) return NULL;
    zst_caps_append(caps, zst_caps_struct_create_audio("audio/aac", 0, 0, ""));
    return caps;
}

static zst_buffer_t*
rtsp_buffer_from_packet(AVPacket* pkt, uint32_t type)
{
    if (!pkt) return NULL;

    zst_buffer_t* buf = zst_buffer_create(type);
    if (!buf) return NULL;

    if (pkt->size > 0) {
        void* data = malloc((size_t)pkt->size);
        if (!data) {
            zst_buffer_unref(buf);
            return NULL;
        }
        memcpy(data, pkt->data, pkt->size);
        buf->memory.data = data;
        buf->memory.size = (size_t)pkt->size;
        buf->memory.priv = data;
        buf->memory.release = free;
    }

    buf->pts = (zst_time_t)pkt->pts;
    buf->dts = (zst_time_t)pkt->dts;
    buf->duration = (zst_time_t)pkt->duration;
    buf->flags = 0;

    return buf;
}

static zst_caps_t*
rtsp_source_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)filter;

    if (!el || !pad) return NULL;
    rtsp_source_t* s = el->priv;
    if (!s) return NULL;

    if (pad == s->video_pad) {
        if (s->video_caps) {
            return zst_caps_copy(s->video_caps);
        }
        return rtsp_video_fallback_caps();
    }

    if (pad == s->audio_pad) {
        if (s->audio_caps) {
            return zst_caps_copy(s->audio_caps);
        }
        return rtsp_audio_fallback_caps();
    }

    return NULL;
}

static zst_result_t
rtsp_source_open(zst_element_t* el)
{
    rtsp_source_t* s = el->priv;
    if (!s) return ZST_ERROR;

    s->fmt_ctx = NULL;
    s->video_stream_index = -1;
    s->audio_stream_index = -1;
    zst_caps_destroy(s->video_caps);
    zst_caps_destroy(s->audio_caps);
    s->video_caps = NULL;
    s->audio_caps = NULL;

    if (s->url[0] == '\0') {
        ZST_LOG_ERROR("rtspsrc", "no RTSP url configured");
        return ZST_ERROR;
    }

    static int network_initialized = 0;
    if (!network_initialized) {
        avformat_network_init();
        network_initialized = 1;
    }

    AVDictionary* opts = NULL;
    if (s->transport[0]) {
        av_dict_set(&opts, "rtsp_transport", s->transport, 0);
    }
    av_dict_set(&opts, "stimeout", "2000000", 0);
    av_dict_set(&opts, "fflags", "nobuffer", 0);
    av_dict_set(&opts, "max_delay", "500000", 0);

    int ret = avformat_open_input(&s->fmt_ctx, s->url, NULL, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        char errbuf[128] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        ZST_LOG_ERROR("rtspsrc", "failed to open RTSP input (%s): %s", s->url, errbuf);
        return ZST_ERROR;
    }

    if (avformat_find_stream_info(s->fmt_ctx, NULL) < 0) {
        ZST_LOG_ERROR("rtspsrc", "failed to read stream info");
        avformat_close_input(&s->fmt_ctx);
        s->fmt_ctx = NULL;
        return ZST_ERROR;
    }

    for (unsigned i = 0; i < s->fmt_ctx->nb_streams; i++) {
        AVStream* stream = s->fmt_ctx->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && s->video_stream_index < 0) {
            s->video_stream_index = (int)i;
            s->video_caps = rtsp_caps_for_video_stream(stream);
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && s->audio_stream_index < 0) {
            s->audio_stream_index = (int)i;
            s->audio_caps = rtsp_caps_for_audio_stream(stream);
        }
    }

    if (s->video_stream_index < 0 && s->audio_stream_index < 0) {
        ZST_LOG_ERROR("rtspsrc", "no audio or video stream found");
        avformat_close_input(&s->fmt_ctx);
        s->fmt_ctx = NULL;
        return ZST_ERROR;
    }

    if (s->video_pad && !s->video_pad->caps && s->video_caps) {
        zst_pad_set_caps(s->video_pad, s->video_caps);
    }
    if (s->audio_pad && !s->audio_pad->caps && s->audio_caps) {
        zst_pad_set_caps(s->audio_pad, s->audio_caps);
    }

    return ZST_OK;
}

static zst_result_t
rtsp_source_close(zst_element_t* el)
{
    rtsp_source_t* s = el->priv;
    if (!s) return ZST_ERROR;

    if (s->fmt_ctx) {
        avformat_close_input(&s->fmt_ctx);
        s->fmt_ctx = NULL;
    }

    zst_caps_destroy(s->video_caps);
    zst_caps_destroy(s->audio_caps);
    s->video_caps = NULL;
    s->audio_caps = NULL;

    return ZST_OK;
}

static zst_result_t
rtsp_source_start(zst_element_t* el)
{
    (void)el;
    return ZST_OK;
}

static zst_result_t
rtsp_source_stop(zst_element_t* el)
{
    (void)el;
    return ZST_OK;
}

static zst_result_t
rtsp_source_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    (void)in;
    if (out) *out = NULL;

    rtsp_source_t* s = el->priv;
    if (!s || !s->fmt_ctx) return ZST_ERROR;

    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;

    int ret = av_read_frame(s->fmt_ctx, &pkt);
    if (ret == AVERROR_EOF) {
        return ZST_EOF;
    }
    if (ret < 0) {
        char errbuf[128] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        ZST_LOG_ERROR("rtspsrc", "read frame failed: %s", errbuf);
        return ZST_ERROR;
    }

    zst_buffer_t* buf = NULL;
    zst_pad_t* target_pad = NULL;

    if (pkt.stream_index == s->video_stream_index) {
        buf = rtsp_buffer_from_packet(&pkt, ZST_BUFFER_VIDEO_PACKET);
        target_pad = s->video_pad;
    } else if (pkt.stream_index == s->audio_stream_index) {
        buf = rtsp_buffer_from_packet(&pkt, ZST_BUFFER_AUDIO_PACKET);
        target_pad = s->audio_pad;
    }

    av_packet_unref(&pkt);

    if (!buf) {
        return ZST_OK;
    }

    if (target_pad && target_pad->peer) {
        zst_pad_push(target_pad, buf);
    }
    zst_buffer_unref(buf);

    return ZST_OK;
}

static zst_result_t
rtsp_source_set_property(zst_element_t* el, const char* name, const char* value)
{
    if (!el || !name || !value) return ZST_ERROR;
    rtsp_source_t* s = el->priv;
    if (!s) return ZST_ERROR;

    if (strcmp(name, "url") == 0) {
        strncpy(s->url, value, sizeof(s->url) - 1);
        s->url[sizeof(s->url) - 1] = '\0';
        return ZST_OK;
    }

    if (strcmp(name, "transport") == 0) {
        strncpy(s->transport, value, sizeof(s->transport) - 1);
        s->transport[sizeof(s->transport) - 1] = '\0';
        return ZST_OK;
    }

    return ZST_ERROR;
}

static zst_result_t
rtsp_source_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    if (!el || !name || !value_out || max_len == 0) return ZST_ERROR;
    rtsp_source_t* s = el->priv;
    if (!s) return ZST_ERROR;

    if (strcmp(name, "url") == 0) {
        strncpy(value_out, s->url, max_len - 1);
        value_out[max_len - 1] = '\0';
        return ZST_OK;
    }

    if (strcmp(name, "transport") == 0) {
        strncpy(value_out, s->transport, max_len - 1);
        value_out[max_len - 1] = '\0';
        return ZST_OK;
    }

    return ZST_ERROR;
}

static zst_element_ops_t g_ops = {
    .name = "rtspsrc",
    .open = rtsp_source_open,
    .close = rtsp_source_close,
    .start = rtsp_source_start,
    .stop = rtsp_source_stop,
    .process = rtsp_source_process,
    .get_caps = rtsp_source_get_caps,
    .set_property = rtsp_source_set_property,
    .get_property = rtsp_source_get_property,
};

zst_element_t*
zst_rtsp_source_create(const char* url)
{
    rtsp_source_t* priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;

    if (url) {
        strncpy(priv->url, url, sizeof(priv->url) - 1);
        priv->url[sizeof(priv->url) - 1] = '\0';
    }
    strncpy(priv->transport, "tcp", sizeof(priv->transport) - 1);

    zst_element_t* el = zst_element_create(&g_ops, priv);
    if (!el) {
        free(priv);
        return NULL;
    }

    priv->video_pad = zst_pad_create("video", ZST_PAD_SRC);
    priv->audio_pad = zst_pad_create("audio", ZST_PAD_SRC);
    if (!priv->video_pad || !priv->audio_pad) {
        zst_element_destroy(el);
        return NULL;
    }

    if (zst_element_add_pad(el, priv->video_pad) != ZST_OK ||
        zst_element_add_pad(el, priv->audio_pad) != ZST_OK) {
        zst_element_destroy(el);
        return NULL;
    }

    return el;
}

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"
#include <string.h>

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "rtspsrc") == 0) {
        return zst_rtsp_source_create(NULL);
    }
    return NULL;
}

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "rtspsrc_plugin",
        .author = "Antigravity",
        .version = "1.0.0",
        .init = NULL,
        .deinit = NULL
    },
    .create_element = plugin_create_element
};

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
