/*=============================================================================
    h265_encoder.c — FFmpeg libavcodec H.265 video encoder implementation
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>

#include "zst_element.h"
#include "zst_buffer.h"
#include "zst_buffer_pool.h"

typedef struct {
    AVCodecContext* codec_ctx;
    AVFrame*        frame;
    int             initialized;
    zst_buffer_pool_t* pool;
    uint32_t        width;
    uint32_t        height;
} h265_encoder_t;

static zst_result_t
h265_open(zst_element_t* el)
{
    h265_encoder_t* s = el->priv;
    s->codec_ctx = NULL;
    s->frame = NULL;
    s->initialized = 0;
    s->pool = NULL;
    s->width = 0;
    s->height = 0;
    return ZST_OK;
}

static zst_result_t
h265_close(zst_element_t* el)
{
    h265_encoder_t* s = el->priv;
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
h265_init_encoder(h265_encoder_t* s, uint32_t width, uint32_t height)
{
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_HEVC);
    if (!codec) return ZST_ERROR;

    s->codec_ctx = avcodec_alloc_context3(codec);
    if (!s->codec_ctx) return ZST_ERROR;

    s->codec_ctx->width = width;
    s->codec_ctx->height = height;
    s->codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    s->codec_ctx->time_base = (AVRational){1, 30};
    s->codec_ctx->bit_rate = 800000;
    s->codec_ctx->max_b_frames = 0;
    s->codec_ctx->gop_size = 30;
    av_opt_set(s->codec_ctx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(s->codec_ctx->priv_data, "tune", "zerolatency", 0);

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

    s->frame->format = s->codec_ctx->pix_fmt;
    s->frame->width = s->codec_ctx->width;
    s->frame->height = s->codec_ctx->height;
    s->frame->pts = 0;

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
        .buffer_size = (size_t)width * height * 3 / 2,
        .buffer_type = ZST_BUFFER_VIDEO_PACKET
    };
    s->pool = zst_buffer_pool_create(NULL, &pool_cfg);
    if (!s->pool) {
        av_frame_free(&s->frame);
        s->frame = NULL;
        avcodec_free_context(&s->codec_ctx);
        s->codec_ctx = NULL;
        return ZST_ERROR;
    }

    s->width = width;
    s->height = height;
    s->initialized = 1;
    return ZST_OK;
}

static zst_result_t
h265_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    h265_encoder_t* s = el->priv;
    if (!in || !out) return ZST_ERROR;
    *out = NULL;

    if (in->flags & ZST_BUFFER_FLAG_EOS) {
        zst_buffer_t* eos_buf = zst_buffer_create(ZST_BUFFER_VIDEO_PACKET);
        if (!eos_buf) return ZST_ERROR;
        eos_buf->flags |= ZST_BUFFER_FLAG_EOS;
        *out = eos_buf;
        return ZST_OK;
    }

    zst_video_frame_t* frame = in->payload;
    if (!frame || !frame->plane[0] || !frame->plane[1] || !frame->plane[2]) return ZST_ERROR;

    if (!s->initialized) {
        if (h265_init_encoder(s, frame->width, frame->height) != ZST_OK) {
            return ZST_ERROR;
        }
    }

    if (frame->format != AV_PIX_FMT_YUV420P) {
        return ZST_ERROR;
    }

    if (av_frame_make_writable(s->frame) < 0) {
        return ZST_ERROR;
    }
    s->frame->pts = in->pts;

    int y_linesize = s->frame->linesize[0];
    int uv_linesize = s->frame->linesize[1];
    int width = (int)s->width;
    int height = (int)s->height;
    int half_width = width / 2;
    int half_height = height / 2;

    for (int row = 0; row < height; row++) {
        memcpy(s->frame->data[0] + (size_t)row * y_linesize,
               frame->plane[0] + (size_t)row * frame->stride[0],
               width);
    }

    for (int row = 0; row < half_height; row++) {
        memcpy(s->frame->data[1] + (size_t)row * uv_linesize,
               frame->plane[1] + (size_t)row * frame->stride[1],
               half_width);
        memcpy(s->frame->data[2] + (size_t)row * uv_linesize,
               frame->plane[2] + (size_t)row * frame->stride[2],
               half_width);
    }

    if (avcodec_send_frame(s->codec_ctx, s->frame) < 0) {
        return ZST_ERROR;
    }

    AVPacket* av_pkt = av_packet_alloc();
    if (!av_pkt) return ZST_ERROR;

    int ret = avcodec_receive_packet(s->codec_ctx, av_pkt);
    if (ret == AVERROR(EAGAIN)) {
        av_packet_free(&av_pkt);
        *out = NULL;
        return ZST_OK;
    }
    if (ret < 0) {
        av_packet_free(&av_pkt);
        return ZST_ERROR;
    }

    zst_buffer_t* pkt = NULL;
    if (zst_buffer_pool_acquire(s->pool, &pkt, 0, 0) != ZST_OK) {
        av_packet_free(&av_pkt);
        return ZST_ERROR;
    }

    if ((size_t)av_pkt->size > pkt->memory.size) {
        zst_buffer_unref(pkt);
        av_packet_free(&av_pkt);
        return ZST_ERROR;
    }

    memcpy(pkt->memory.data, av_pkt->data, av_pkt->size);
    pkt->memory.size = av_pkt->size;
    pkt->pts = av_pkt->pts;
    pkt->dts = av_pkt->dts;

    av_packet_free(&av_pkt);
    *out = pkt;
    return ZST_OK;
}

static zst_element_ops_t g_ops = {
    .name    = "h265enc",
    .open    = h265_open,
    .close   = h265_close,
    .process = h265_process,
};

zst_element_t*
zst_h265_encoder_create(void)
{
    zst_element_t* el;
    h265_encoder_t* priv;
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
    if (strcmp(name, "h265enc") == 0) {
        return zst_h265_encoder_create();
    }
    return NULL;
}

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "h265encoder_plugin",
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
