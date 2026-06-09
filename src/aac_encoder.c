/*=============================================================================
    aac_encoder.c — FFmpeg libavcodec AAC audio encoder implementation
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>

#include "mm_element.h"
#include "mm_buffer.h"

typedef struct {
    AVCodecContext* codec_ctx;
    AVFrame*        frame;
    int             initialized;
} aac_encoder_t;

static void
aac_buf_free(mm_buffer_t* buf)
{
    if (buf && buf->memory.data) {
        free(buf->memory.data);
        buf->memory.data = NULL;
    }
}

static mm_result_t
aac_open(mm_element_t* el)
{
    aac_encoder_t* s = el->priv;
    s->codec_ctx = NULL;
    s->frame = NULL;
    s->initialized = 0;
    return MM_OK;
}

static mm_result_t
aac_close(mm_element_t* el)
{
    aac_encoder_t* s = el->priv;
    if (s->codec_ctx) {
        avcodec_free_context(&s->codec_ctx);
        s->codec_ctx = NULL;
    }
    if (s->frame) {
        av_frame_free(&s->frame);
        s->frame = NULL;
    }
    s->initialized = 0;
    return MM_OK;
}

static mm_result_t
aac_init_encoder(aac_encoder_t* s)
{
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) return MM_ERROR;

    s->codec_ctx = avcodec_alloc_context3(codec);
    if (!s->codec_ctx) return MM_ERROR;

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
        return MM_ERROR;
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
        return MM_ERROR;
    }

    s->initialized = 1;
    return MM_OK;
}

static mm_result_t
aac_process(mm_element_t* el, mm_buffer_t* in, mm_buffer_t** out)
{
    aac_encoder_t* s = el->priv;
    if (!in) return MM_ERROR;

    if (in->flags & MM_BUFFER_FLAG_EOS) {
        mm_buffer_t* eos_buf = mm_buffer_create(MM_BUFFER_AUDIO_PACKET);
        if (eos_buf) {
            eos_buf->flags |= MM_BUFFER_FLAG_EOS;
            *out = eos_buf;
            return MM_OK;
        }
        return MM_ERROR;
    }

    mm_audio_frame_t* a_frame = in->payload;
    if (!a_frame) return MM_ERROR;

    if (!s->initialized) {
        if (aac_init_encoder(s) != MM_OK) return MM_ERROR;
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
        return MM_ERROR;
    }

    AVPacket* av_pkt = av_packet_alloc();
    if (!av_pkt) return MM_ERROR;

    int ret = avcodec_receive_packet(s->codec_ctx, av_pkt);
    if (ret == 0) {
        mm_buffer_t* pkt = mm_buffer_create(MM_BUFFER_AUDIO_PACKET);
        if (!pkt) {
            av_packet_free(&av_pkt);
            return MM_ERROR;
        }

        uint8_t* data = malloc(av_pkt->size);
        if (!data) {
            mm_buffer_unref(pkt);
            av_packet_free(&av_pkt);
            return MM_ERROR;
        }
        memcpy(data, av_pkt->data, av_pkt->size);

        pkt->memory.type = MM_MEMORY_CPU;
        pkt->memory.data = data;
        pkt->memory.size = av_pkt->size;
        pkt->pts = av_pkt->pts;
        pkt->dts = av_pkt->dts;
        pkt->destroy = aac_buf_free;

        *out = pkt;
    } else if (ret == AVERROR(EAGAIN)) {
        *out = NULL;
    } else {
        av_packet_free(&av_pkt);
        return MM_ERROR;
    }

    av_packet_free(&av_pkt);
    return MM_OK;
}

static mm_element_ops_t g_ops = {
    .name    = "aacenc",
    .open    = aac_open,
    .close   = aac_close,
    .process = aac_process,
};

mm_element_t*
mm_aac_encoder_create(void)
{
    mm_element_t* el;
    aac_encoder_t* priv;
    mm_pad_t* sink;
    mm_pad_t* src;

    priv = calloc(1, sizeof(*priv));
    el = mm_element_create(&g_ops, priv);
    sink = mm_pad_create("sink", MM_PAD_SINK);
    src  = mm_pad_create("src",  MM_PAD_SRC);

    mm_element_add_pad(el, sink);
    mm_element_add_pad(el, src);
    return el;
}
