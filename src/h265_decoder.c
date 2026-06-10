/*=============================================================================
    h265_decoder.c — FFmpeg libavcodec H.265 video decoder implementation
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>

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
    int             format;
} h265_decoder_t;

static zst_result_t
h265_open(zst_element_t* el)
{
    h265_decoder_t* s = el->priv;
    s->codec_ctx = NULL;
    s->frame = NULL;
    s->initialized = 0;
    s->pool = NULL;
    s->width = 0;
    s->height = 0;
    s->format = AV_PIX_FMT_NONE;
    return ZST_OK;
}

static zst_result_t
h265_close(zst_element_t* el)
{
    h265_decoder_t* s = el->priv;
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
h265_init_decoder(h265_decoder_t* s)
{
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
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
h265_update_pool(h265_decoder_t* s, int width, int height, int format)
{
    if (s->pool && s->width == (uint32_t)width && s->height == (uint32_t)height && s->format == format) {
        return ZST_OK;
    }

    if (s->pool) {
        zst_buffer_pool_destroy(s->pool);
        s->pool = NULL;
    }

    int size = av_image_get_buffer_size(format, width, height, 1);
    if (size < 0) return ZST_ERROR;

    zst_buffer_pool_config_t pool_cfg = {
        .min_buffers = 2,
        .max_buffers = 8,
        .buffer_size = (size_t)size,
        .buffer_type = ZST_BUFFER_VIDEO_FRAME
    };
    s->pool = zst_buffer_pool_create(NULL, &pool_cfg);
    if (!s->pool) return ZST_ERROR;

    s->width = width;
    s->height = height;
    s->format = format;
    return ZST_OK;
}

static zst_result_t
h265_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    h265_decoder_t* s = el->priv;
    *out = NULL;

    if (!in) {
        return ZST_ERROR;
    }

    if (!s->initialized) {
        if (h265_init_decoder(s) != ZST_OK) return ZST_ERROR;
    }

    AVPacket* av_pkt = NULL;
    if (in->flags & ZST_BUFFER_FLAG_EOS) {
        av_pkt = NULL;
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

        if (h265_update_pool(s, s->frame->width, s->frame->height, s->frame->format) != ZST_OK) {
            return ZST_ERROR;
        }

        zst_buffer_t* vbuf = NULL;
        if (zst_buffer_pool_acquire(s->pool, &vbuf, 0, 0) != ZST_OK) {
            return ZST_ERROR;
        }

        vbuf->pts = s->frame->pts;
        if (s->frame->best_effort_timestamp != AV_NOPTS_VALUE && vbuf->pts == (zst_time_t)AV_NOPTS_VALUE) {
            vbuf->pts = s->frame->best_effort_timestamp;
        }

        zst_video_frame_t* v_frame = vbuf->payload;
        v_frame->width = s->frame->width;
        v_frame->height = s->frame->height;
        v_frame->format = s->frame->format;

        uint8_t* dst_data[4] = {0};
        int dst_linesize[4] = {0};
        av_image_fill_arrays(dst_data, dst_linesize, vbuf->memory.data, s->frame->format, s->frame->width, s->frame->height, 1);
        av_image_copy(dst_data, dst_linesize, (const uint8_t**)s->frame->data, s->frame->linesize, s->frame->format, s->frame->width, s->frame->height);

        for (int i = 0; i < 4; i++) {
            v_frame->plane[i] = dst_data[i];
            v_frame->stride[i] = dst_linesize[i];
        }

        zst_pad_push(el->src_pads[0], vbuf);
    }

    if (in->flags & ZST_BUFFER_FLAG_EOS) {
        zst_buffer_t* eos_buf = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
        if (eos_buf) {
            eos_buf->flags |= ZST_BUFFER_FLAG_EOS;
            zst_pad_push(el->src_pads[0], eos_buf);
        }
    }

    return ZST_OK;
}

static zst_element_ops_t g_ops = {
    .name    = "h265dec",
    .open    = h265_open,
    .close   = h265_close,
    .process = h265_process,
};

zst_element_t*
zst_h265_decoder_create(void)
{
    zst_element_t* el;
    h265_decoder_t* priv;
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
    if (strcmp(name, "h265dec") == 0) {
        return zst_h265_decoder_create();
    }
    return NULL;
}

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "h265decoder_plugin",
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
