/*=============================================================================
    aac_decoder.c — FFmpeg libavcodec AAC audio decoder implementation
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavutil/samplefmt.h>
#include <libavutil/channel_layout.h>

#include "zst_element.h"
#include "zst_buffer.h"
#include "zst_buffer_pool.h"

typedef struct {
    AVCodecContext* codec_ctx;
    AVFrame*        frame;
    int             initialized;
    zst_buffer_pool_t* pool;
    int             channels;
    int             sample_rate;
    int             format;
} aac_decoder_t;

static zst_result_t
aacdec_open(zst_element_t* el)
{
    aac_decoder_t* s = el->priv;
    s->codec_ctx = NULL;
    s->frame = NULL;
    s->initialized = 0;
    s->pool = NULL;
    s->channels = 0;
    s->sample_rate = 0;
    s->format = AV_SAMPLE_FMT_NONE;
    return ZST_OK;
}

static zst_result_t
aacdec_close(zst_element_t* el)
{
    aac_decoder_t* s = el->priv;
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
aacdec_init_decoder(aac_decoder_t* s)
{
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    if (!codec) return ZST_ERROR;

    s->codec_ctx = avcodec_alloc_context3(codec);
    if (!s->codec_ctx) return ZST_ERROR;

    if (avcodec_open2(s->codec_ctx, codec, NULL) < 0) {
        avcodec_free_context(&s->codec_ctx);
        s->codec_ctx = NULL;
        return ZST_ERROR;
    }

    s->frame = av_frame_alloc();
    if (!s->frame) {
        avcodec_free_context(&s->codec_ctx);
        s->codec_ctx = NULL;
        return ZST_ERROR;
    }

    s->initialized = 1;
    return ZST_OK;
}

static zst_result_t
aacdec_update_pool(aac_decoder_t* s, int channels, int sample_rate, int format, int nb_samples)
{
    if (s->pool && s->channels == channels && s->sample_rate == sample_rate && s->format == format) {
        return ZST_OK;
    }

    if (s->pool) {
        zst_buffer_pool_destroy(s->pool);
        s->pool = NULL;
    }

    int size = av_samples_get_buffer_size(NULL, channels, nb_samples, format, 1);
    if (size < 0) size = 8192 * channels * av_get_bytes_per_sample(format); /* Fallback */

    zst_buffer_pool_config_t pool_cfg = {
        .min_buffers = 2,
        .max_buffers = 8,
        .buffer_size = size,
        .buffer_type = ZST_BUFFER_AUDIO_FRAME
    };

    s->pool = zst_buffer_pool_create(NULL, &pool_cfg);
    if (!s->pool) return ZST_ERROR;

    s->channels = channels;
    s->sample_rate = sample_rate;
    s->format = format;

    return ZST_OK;
}

static zst_result_t
aacdec_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    aac_decoder_t* s = el->priv;
    *out = NULL;

    if (!in) {
        return ZST_ERROR;
    }

    if (!s->initialized) {
        if (aacdec_init_decoder(s) != ZST_OK) return ZST_ERROR;
    }

    AVPacket* av_pkt = NULL;
    if (in->flags & ZST_BUFFER_FLAG_EOS) {
        av_pkt = NULL; /* Flush packet */
    } else {
        av_pkt = av_packet_alloc();
        if (!av_pkt) return ZST_ERROR;
        av_pkt->data = in->memory.data;
        av_pkt->size = in->memory.size;
        av_pkt->pts = in->pts;
        av_pkt->dts = in->dts;
    }

    int ret = avcodec_send_packet(s->codec_ctx, av_pkt);
    if (av_pkt) {
        av_packet_free(&av_pkt);
    }

    if (ret < 0 && ret != AVERROR_EOF) {
        return ZST_ERROR;
    }

    while (1) {
        ret = avcodec_receive_frame(s->codec_ctx, s->frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            return ZST_ERROR;
        }

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 37, 100)
        int channels = s->frame->ch_layout.nb_channels;
#else
        int channels = s->frame->channels;
#endif

        if (aacdec_update_pool(s, channels, s->frame->sample_rate, s->frame->format, s->frame->nb_samples) != ZST_OK) {
            return ZST_ERROR;
        }

        zst_buffer_t* abuf = NULL;
        if (zst_buffer_pool_acquire(s->pool, &abuf, 0, 0) != ZST_OK) {
            return ZST_ERROR;
        }

        abuf->pts = s->frame->pts;
        if (s->frame->best_effort_timestamp != AV_NOPTS_VALUE && abuf->pts == (zst_time_t)AV_NOPTS_VALUE) {
            abuf->pts = s->frame->best_effort_timestamp;
        }

        zst_audio_frame_t* a_frame = abuf->payload;
        a_frame->channels = channels;
        a_frame->sample_rate = s->frame->sample_rate;
        a_frame->format = s->frame->format;
        a_frame->nb_samples = s->frame->nb_samples;
        a_frame->data = abuf->memory.data;

        int size = av_samples_get_buffer_size(NULL, channels, s->frame->nb_samples, s->frame->format, 1);
        uint8_t* dst_data[8] = {0};
        int dst_linesize[8] = {0};
        av_samples_fill_arrays(dst_data, dst_linesize, abuf->memory.data, channels, s->frame->nb_samples, s->frame->format, 1);
        av_samples_copy(dst_data, (uint8_t * const *)s->frame->data, 0, 0, s->frame->nb_samples, channels, s->frame->format);
        abuf->memory.size = size;

        zst_pad_push(el->src_pads[0], abuf);
    }

    if (in->flags & ZST_BUFFER_FLAG_EOS) {
        zst_buffer_t* eos_buf = zst_buffer_create(ZST_BUFFER_AUDIO_FRAME);
        if (eos_buf) {
            eos_buf->flags |= ZST_BUFFER_FLAG_EOS;
            zst_pad_push(el->src_pads[0], eos_buf);
        }
    }

    return ZST_OK;
}

static zst_element_ops_t g_ops = {
    .name    = "aacdec",
    .open    = aacdec_open,
    .close   = aacdec_close,
    .process = aacdec_process,
};

zst_element_t*
zst_aac_decoder_create(void)
{
    zst_element_t* el;
    aac_decoder_t* priv;
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

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "aacdec") == 0) {
        return zst_aac_decoder_create();
    }
    return NULL;
}

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "aacdecoder_plugin",
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
