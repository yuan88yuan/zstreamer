/*=============================================================================
    rtmp_source.c — RTMP source element using FFmpeg/libavformat
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>

#include "zst_element.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_caps.h"
#include "zst_log.h"
#include "zst_bus.h"
#include "zst_buffer_pool.h"
#include "zst_clock.h"

typedef struct {
    AVFormatContext* fc;
    char             url[512];
    pthread_t        thread;
    int              running;
    int              video_stream_idx;
    int              audio_stream_idx;
    zst_pad_t*       video_pad;
    zst_pad_t*       audio_pad;
    pthread_mutex_t  lock;  /* protects push to downstream */
} rtmp_source_t;

static int
rtmp_source_interrupt_cb(void* ctx)
{
    rtmp_source_t* s = ctx;
    return s && !__atomic_load_n(&s->running, __ATOMIC_ACQUIRE);
}

static void*
rtmp_source_thread(void* user_data)
{
    zst_element_t* el = user_data;
    rtmp_source_t* s = el->priv;
    AVPacket* pkt = av_packet_alloc();

    while (__atomic_load_n(&s->running, __ATOMIC_ACQUIRE)) {
        int ret = av_read_frame(s->fc, pkt);
        if (ret < 0) {
            ZST_LOG_INFO("rtmpsrc", "RTMP source EOF or error (%d)", ret);
            break;
        }

        zst_pad_t* pad = NULL;
        if (pkt->stream_index == s->video_stream_idx) {
            pad = s->video_pad;
        } else if (pkt->stream_index == s->audio_stream_idx) {
            pad = s->audio_pad;
        }

        pthread_mutex_lock(&s->lock);
        int still_running = __atomic_load_n(&s->running, __ATOMIC_ACQUIRE);
        if (still_running && pad && pad->peer) {
            int btype = (pkt->stream_index == s->video_stream_idx)
                      ? ZST_BUFFER_VIDEO_PACKET : ZST_BUFFER_AUDIO_PACKET;
            zst_buffer_t* buf = zst_buffer_create(btype);
            if (buf) {
                buf->memory.data = malloc(pkt->size);
                buf->memory.size = pkt->size;
                buf->memory.priv = buf->memory.data;
                buf->memory.release = free;
                memcpy(buf->memory.data, pkt->data, pkt->size);

                AVRational tb = s->fc->streams[pkt->stream_index]->time_base;
                buf->pts = av_rescale_q(pkt->pts, tb, (AVRational){1, 1000000000});
                buf->dts = av_rescale_q(pkt->dts, tb, (AVRational){1, 1000000000});
                buf->duration = av_rescale_q(pkt->duration, tb, (AVRational){1, 1000000000});

                zst_pad_push(pad, buf);
                zst_buffer_unref(buf);
            }
        }
        pthread_mutex_unlock(&s->lock);
        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);

    pthread_mutex_lock(&s->lock);
    // Push EOS on active pads
    if (s->video_pad && s->video_pad->peer) {
        zst_buffer_t* eos_buf = zst_buffer_create(ZST_BUFFER_USER);
        if (eos_buf) {
            eos_buf->flags |= ZST_BUFFER_FLAG_EOS;
            zst_pad_push(s->video_pad, eos_buf);
            zst_buffer_unref(eos_buf);
        }
    }
    if (s->audio_pad && s->audio_pad->peer) {
        zst_buffer_t* eos_buf = zst_buffer_create(ZST_BUFFER_USER);
        if (eos_buf) {
            eos_buf->flags |= ZST_BUFFER_FLAG_EOS;
            zst_pad_push(s->audio_pad, eos_buf);
            zst_buffer_unref(eos_buf);
        }
    }
    pthread_mutex_unlock(&s->lock);

    if (el->bus) {
        zst_bus_post(el->bus, zst_event_new_eos(el));
    }

    __atomic_store_n(&s->running, 0, __ATOMIC_RELEASE);
    return NULL;
}

static zst_result_t
rtmp_source_open(zst_element_t* el)
{
    rtmp_source_t* s = el->priv;
    if (!s) return ZST_ERROR;
    if (s->url[0] == '\0') {
        ZST_LOG_ERROR("rtmpsrc", "RTMP URL not set");
        return ZST_ERROR;
    }

    pthread_mutex_init(&s->lock, NULL);

    s->fc = avformat_alloc_context();
    if (!s->fc) {
        pthread_mutex_destroy(&s->lock);
        return ZST_ERROR;
    }
    s->fc->interrupt_callback.callback = rtmp_source_interrupt_cb;
    s->fc->interrupt_callback.opaque = s;

    if (avformat_open_input(&s->fc, s->url, NULL, NULL) < 0) {
        ZST_LOG_ERROR("rtmpsrc", "Failed to open RTMP URL: %s", s->url);
        avformat_free_context(s->fc);
        s->fc = NULL;
        pthread_mutex_destroy(&s->lock);
        return ZST_ERROR;
    }

    if (avformat_find_stream_info(s->fc, NULL) < 0) {
        ZST_LOG_ERROR("rtmpsrc", "Failed to find stream info");
        avformat_close_input(&s->fc);
        s->fc = NULL;
        pthread_mutex_destroy(&s->lock);
        return ZST_ERROR;
    }

    s->video_stream_idx = -1;
    s->audio_stream_idx = -1;
    s->video_pad = zst_element_get_pad(el, "video");
    s->audio_pad = zst_element_get_pad(el, "audio");

    for (unsigned int i = 0; i < s->fc->nb_streams; i++) {
        AVStream* st = s->fc->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && s->video_stream_idx < 0) {
            s->video_stream_idx = i;
            if (s->video_pad) {
                const char* vmt = "video/x-raw";
                if (st->codecpar->codec_id == AV_CODEC_ID_H264)
                    vmt = "video/x-h264";
                else if (st->codecpar->codec_id == AV_CODEC_ID_HEVC)
                    vmt = "video/x-h265";
                zst_caps_t* vcaps = zst_caps_create();
                zst_caps_append(vcaps, zst_caps_struct_create_video(vmt, 0, 0, 0, ""));
                zst_pad_set_caps(s->video_pad, vcaps);
                zst_caps_destroy(vcaps);
            }
        } else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && s->audio_stream_idx < 0) {
            s->audio_stream_idx = i;
            if (s->audio_pad) {
                const char* amt = (st->codecpar->codec_id == AV_CODEC_ID_AAC)
                                ? "audio/aac" : "audio/x-raw";
                zst_caps_t* acaps = zst_caps_create();
                zst_caps_append(acaps, zst_caps_struct_create_audio(amt, 0, 0, ""));
                zst_pad_set_caps(s->audio_pad, acaps);
                zst_caps_destroy(acaps);
            }
        }
    }

    return ZST_OK;
}

static zst_result_t
rtmp_source_close(zst_element_t* el)
{
    rtmp_source_t* s = el->priv;
    if (!s) return ZST_ERROR;

    if (s->fc) {
        avformat_close_input(&s->fc);
        s->fc = NULL;
    }
    pthread_mutex_destroy(&s->lock);
    return ZST_OK;
}

static zst_result_t
rtmp_source_start(zst_element_t* el)
{
    rtmp_source_t* s = el->priv;
    if (!s) return ZST_ERROR;

    __atomic_store_n(&s->running, 1, __ATOMIC_RELEASE);
    pthread_create(&s->thread, NULL, rtmp_source_thread, el);

    return ZST_OK;
}

static zst_result_t
rtmp_source_stop(zst_element_t* el)
{
    rtmp_source_t* s = el->priv;
    if (!s) return ZST_ERROR;

    if (__atomic_load_n(&s->running, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&s->running, 0, __ATOMIC_RELEASE);
        pthread_mutex_lock(&s->lock);
        pthread_mutex_unlock(&s->lock);
        pthread_join(s->thread, NULL);
    }

    return ZST_OK;
}

static zst_result_t
rtmp_source_set_property(zst_element_t* el, const char* name, const char* value)
{
    if (!el || !el->priv || !name || !value) return ZST_ERROR;
    rtmp_source_t* s = el->priv;
    if (strcmp(name, "url") == 0) {
        snprintf(s->url, sizeof(s->url), "%s", value);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t
rtmp_source_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    if (!el || !el->priv || !name || !value_out || max_len == 0) return ZST_ERROR;
    rtmp_source_t* s = el->priv;
    if (strcmp(name, "url") == 0) {
        snprintf(value_out, max_len, "%s", s->url);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_caps_t*
rtmp_source_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)el;
    (void)filter;
    if (!pad) return NULL;

    zst_caps_t* caps = zst_caps_create();
    if (!caps) return NULL;

    if (strcmp(pad->name, "video") == 0) {
        zst_caps_append(caps, zst_caps_struct_create_video("video/x-h264", 0, 0, 0.0, ""));
        zst_caps_append(caps, zst_caps_struct_create_video("video/x-h265", 0, 0, 0.0, ""));
        return caps;
    }

    if (strcmp(pad->name, "audio") == 0) {
        zst_caps_append(caps, zst_caps_struct_create_audio("audio/aac", 0, 0, ""));
        return caps;
    }

    zst_caps_destroy(caps);
    return NULL;
}

static zst_element_ops_t g_ops = {
    .name  = "rtmpsrc",
    .open  = rtmp_source_open,
    .close = rtmp_source_close,
    .start = rtmp_source_start,
    .stop  = rtmp_source_stop,
    .set_property = rtmp_source_set_property,
    .get_property = rtmp_source_get_property,
    .get_caps = rtmp_source_get_caps,
};

zst_element_t*
zst_rtmp_source_create(const char* url)
{
    rtmp_source_t* priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;

    if (url) {
        snprintf(priv->url, sizeof(priv->url), "%s", url);
    }

    zst_element_t* el = zst_element_create(&g_ops, priv);
    if (!el) {
        free(priv);
        return NULL;
    }

    zst_pad_t* video = zst_pad_create("video", ZST_PAD_SRC);
    zst_pad_t* audio = zst_pad_create("audio", ZST_PAD_SRC);

    if (!video || !audio) {
        if (video) zst_pad_destroy(video);
        if (audio) zst_pad_destroy(audio);
        zst_element_destroy(el);
        return NULL;
    }

    if (zst_element_add_pad(el, video) != ZST_OK) {
        zst_pad_destroy(video);
        zst_pad_destroy(audio);
        zst_element_destroy(el);
        return NULL;
    }

    if (zst_element_add_pad(el, audio) != ZST_OK) {
        zst_pad_destroy(audio);
        zst_element_destroy(el);
        return NULL;
    }

    return el;
}

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "rtmpsrc") == 0) {
        return zst_rtmp_source_create(NULL);
    }
    return NULL;
}

static const zst_pad_template_t g_rtmpsrc_pads[] = {
    { "video", ZST_PAD_SRC, "ANY" },
    { "audio", ZST_PAD_SRC, "ANY" }
};

static const zst_property_spec_t g_rtmpsrc_properties[] = {
    { "url", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "RTMP Endpoint URL" }
};

static const zst_element_desc_t g_rtmpsrc_elements[] = {
    {
        .name = "rtmpsrc",
        .long_name = "RTMP Source",
        .category = "Source/Network",
        .description = "Receives audio/video from an RTMP endpoint",
        .author = "zstreamer",
        .properties = g_rtmpsrc_properties,
        .nb_properties = sizeof(g_rtmpsrc_properties) / sizeof(g_rtmpsrc_properties[0]),
        .pads = g_rtmpsrc_pads,
        .nb_pads = sizeof(g_rtmpsrc_pads) / sizeof(g_rtmpsrc_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "rtmpsrc_plugin",
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
        *nb_elements_out = sizeof(g_rtmpsrc_elements) / sizeof(g_rtmpsrc_elements[0]);
    }
    return g_rtmpsrc_elements;
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
