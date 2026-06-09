/*=============================================================================
    aac_encoder.c — FFmpeg libavcodec AAC audio encoder implementation
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>

#include "zst_element.h"
#include "zst_buffer.h"
#include "zst_buffer_pool.h"

typedef struct {
    AVCodecContext* codec_ctx;
    AVFrame*        frame;
    int             initialized;
    zst_buffer_pool_t* pool;
} aac_encoder_t;

static zst_result_t
aac_open(zst_element_t* el)
{
    aac_encoder_t* s = el->priv;
    s->codec_ctx = NULL;
    s->frame = NULL;
    s->initialized = 0;
    s->pool = NULL;
    return ZST_OK;
}

static zst_result_t
aac_close(zst_element_t* el)
{
    aac_encoder_t* s = el->priv;
    if (s->pool) {
        zst_buffer_pool_destroy(s->pool);
        s->pool = NULL;
    }
    if (s->codec_ctx) {
        avcodec_free_context(&s->codec_ctx);
        s->codec_ctx = NULL;
    }
    if (s->frame) {
        av_frame_free(&s->frame);
        s->frame = NULL;
    }
    s->initialized = 0;
    return ZST_OK;
}

static zst_result_t
aac_init_encoder(aac_encoder_t* s)
{
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) return ZST_ERROR;

    s->codec_ctx = avcodec_alloc_context3(codec);
    if (!s->codec_ctx) return ZST_ERROR;

    s->codec_ctx->sample_rate = 44100;
    s->codec_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP; // Float planar (native for FFmpeg AAC)
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 37, 100)
    av_channel_layout_default(&s->codec_ctx->ch_layout, 2);
#else
    s->codec_ctx->channels = 2;
    s->codec_ctx->channel_layout = AV_CH_LAYOUT_STEREO;
#endif
    s->codec_ctx->bit_rate = 128000;

    if (avcodec_open2(s->codec_ctx, codec, NULL) < 0) {
        avcodec_free_context(&s->codec_ctx);
        s->codec_ctx = NULL;
        return ZST_ERROR;
    }

    s->frame = av_frame_alloc();
    s->frame->nb_samples = 1024;
    s->frame->format = AV_SAMPLE_FMT_FLTP;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 37, 100)
    av_channel_layout_default(&s->frame->ch_layout, 2);
#else
    s->frame->channels = 2;
    s->frame->channel_layout = AV_CH_LAYOUT_STEREO;
#endif

    if (av_frame_get_buffer(s->frame, 0) < 0) {
        av_frame_free(&s->frame);
        s->frame = NULL;
        avcodec_free_context(&s->codec_ctx);
        s->codec_ctx = NULL;
        return ZST_ERROR;
    }

    zst_buffer_pool_config_t pool_cfg = {
        .min_buffers = 2,
        .max_buffers = 8,
        .buffer_size = 16384, // Safe upper bound for AAC packet (typical < 8192)
        .buffer_type = ZST_BUFFER_AUDIO_PACKET
    };
    s->pool = zst_buffer_pool_create(NULL, &pool_cfg);
    if (!s->pool) {
        av_frame_free(&s->frame);
        s->frame = NULL;
        avcodec_free_context(&s->codec_ctx);
        s->codec_ctx = NULL;
        return ZST_ERROR;
    }

    s->initialized = 1;
    return ZST_OK;
}

static zst_result_t
aac_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    aac_encoder_t* s = el->priv;
    if (!in) return ZST_ERROR;

    if (in->flags & ZST_BUFFER_FLAG_EOS) {
        zst_buffer_t* eos_buf = zst_buffer_create(ZST_BUFFER_AUDIO_PACKET);
        if (eos_buf) {
            eos_buf->flags |= ZST_BUFFER_FLAG_EOS;
            *out = eos_buf;
            return ZST_OK;
        }
        return ZST_ERROR;
    }

    zst_audio_frame_t* a_frame = in->payload;
    if (!a_frame) return ZST_ERROR;

    if (!s->initialized) {
        if (aac_init_encoder(s) != ZST_OK) return ZST_ERROR;
    }

    /* Convert S16 interleaved to FLTP float planar */
    int16_t* src = (int16_t*)a_frame->data;
    float* dst_l = (float*)s->frame->data[0];
    float* dst_r = (float*)s->frame->data[1];

    for (uint32_t i = 0; i < a_frame->nb_samples; i++) {
        dst_l[i] = (float)src[i * 2] / 32768.0f;
        dst_r[i] = (float)src[i * 2 + 1] / 32768.0f;
    }

    s->frame->pts = in->pts;

    if (avcodec_send_frame(s->codec_ctx, s->frame) < 0) {
        return ZST_ERROR;
    }

    AVPacket* av_pkt = av_packet_alloc();
    if (!av_pkt) return ZST_ERROR;

    int ret = avcodec_receive_packet(s->codec_ctx, av_pkt);
    if (ret == 0) {
        zst_buffer_t* pkt = NULL;
        if (zst_buffer_pool_acquire(s->pool, &pkt, 0) != ZST_OK) {
            av_packet_free(&av_pkt);
            return ZST_ERROR;
        }

        uint8_t* data = pkt->memory.data;
        memcpy(data, av_pkt->data, av_pkt->size);

        pkt->memory.size = av_pkt->size;
        pkt->pts = av_pkt->pts;
        pkt->dts = av_pkt->dts;

        *out = pkt;
    } else if (ret == AVERROR(EAGAIN)) {
        *out = NULL;
    } else {
        av_packet_free(&av_pkt);
        return ZST_ERROR;
    }

    av_packet_free(&av_pkt);
    return ZST_OK;
}

static zst_element_ops_t g_ops = {
    .name    = "aacenc",
    .open    = aac_open,
    .close   = aac_close,
    .process = aac_process,
};

zst_element_t*
zst_aac_encoder_create(void)
{
    zst_element_t* el;
    aac_encoder_t* priv;
    zst_pad_t* sink;
    zst_pad_t* src;

    priv = calloc(1, sizeof(*priv));
    el = zst_element_create(&g_ops, priv);
    sink = zst_pad_create("sink", ZST_PAD_SINK);
    src  = zst_pad_create("src",  ZST_PAD_SRC);

    zst_element_add_pad(el, sink);
    zst_element_add_pad(el, src);
    return el;
}

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"
#include <string.h>

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "aacenc") == 0) {
        return zst_aac_encoder_create();
    }
    return NULL;
}

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "aacencoder_plugin",
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
