/*=============================================================================
    vaapi_video_decoder.c — Linux VA-API video decoder implementation
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext_drm.h>
#include <libdrm/drm_fourcc.h>

#include "zst_element.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_buffer_pool.h"
#include "zst_caps.h"
#include "zst_log.h"
#include "zst_allocator.h"
#include "zstreamer/elements/zst_vaapi_video_decoder.h"

#define VAAPI_DEC_DEFAULT_DEVICE "/dev/dri/renderD128"

typedef struct {
    AVCodecContext* codec_ctx;
    AVBufferRef*    hw_device_ctx;
    AVFrame*        hw_frame;
    int             initialized;
    zst_buffer_pool_t* pool;
    uint32_t        width;
    uint32_t        height;
    int             format;
    zst_pad_t*      sinkpad;
    zst_pad_t*      srcpad;

    char            device[256];     /* DRM render node path */
    char            codec[32];       /* h264 or h265 */
    char            memory_type[32]; /* cpu or dmabuf */
} vaapi_video_decoder_t;

typedef struct {
    AVFrame* hw_frame;
    AVFrame* drm_frame;
    zst_allocator_t* allocator;
    void* ptr;
} vaapi_dec_dmabuf_ctx_t;

static void release_dmabuf_ctx(void* priv)
{
    vaapi_dec_dmabuf_ctx_t* ctx = (vaapi_dec_dmabuf_ctx_t*)priv;
    if (ctx) {
        if (ctx->allocator && ctx->ptr) {
            zst_allocator_free(ctx->allocator, ctx->ptr);
        }
        if (ctx->allocator) {
            zst_allocator_unref(ctx->allocator);
        }
        if (ctx->drm_frame) {
            av_frame_free(&ctx->drm_frame);
        }
        if (ctx->hw_frame) {
            av_frame_free(&ctx->hw_frame);
        }
        free(ctx);
    }
}

static void dec_buf_free(zst_buffer_t* buf)
{
    if (buf && buf->payload) {
        free(buf->payload);
        buf->payload = NULL;
    }
}

static void destroy_dmabuf_frame(zst_buffer_t* buf)
{
    if (buf->payload) {
        free(buf->payload);
        buf->payload = NULL;
    }
}

static int is_annexb(const uint8_t* data, size_t size)
{
    if (!data || size < 4) return 0;
    return (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01) ||
           (size >= 4 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x01);
}

static int lengths_4byte_valid(const uint8_t* data, size_t size)
{
    size_t pos = 0;
    int nals = 0;
    while (pos + 4 <= size) {
        uint32_t n = ((uint32_t)data[pos] << 24) |
                     ((uint32_t)data[pos + 1] << 16) |
                     ((uint32_t)data[pos + 2] << 8) |
                     (uint32_t)data[pos + 3];
        pos += 4;
        if (n == 0 || n > size - pos) return 0;
        pos += n;
        nals++;
    }
    return pos == size && nals > 0;
}

static zst_result_t
packet_from_buffer(zst_buffer_t* in, AVPacket* pkt)
{
    if (!pkt || !in) return ZST_ERROR;

    pkt->pts = in->pts;
    pkt->dts = in->dts;
    pkt->duration = in->duration;

    const uint8_t* data = (const uint8_t*)in->memory.data;
    size_t size = in->memory.size;
    if (!data || size == 0) return ZST_ERROR;

    if (!is_annexb(data, size) && lengths_4byte_valid(data, size)) {
        if (av_new_packet(pkt, (int)size) < 0) return ZST_ERROR;
        size_t pos = 0;
        uint8_t* out = pkt->data;
        while (pos + 4 <= size) {
            uint32_t n = ((uint32_t)data[pos] << 24) |
                         ((uint32_t)data[pos + 1] << 16) |
                         ((uint32_t)data[pos + 2] << 8) |
                         (uint32_t)data[pos + 3];
            pos += 4;
            *out++ = 0x00;
            *out++ = 0x00;
            *out++ = 0x00;
            *out++ = 0x01;
            memcpy(out, data + pos, n);
            out += n;
            pos += n;
        }
    } else {
        pkt->data = (uint8_t*)data;
        pkt->size = (int)size;
    }

    return ZST_OK;
}

static zst_result_t
vaapi_dec_update_pool(vaapi_video_decoder_t* s, int width, int height, int format)
{
    if (s->pool && s->width == (uint32_t)width && s->height == (uint32_t)height && s->format == format) {
        return ZST_OK;
    }

    if (s->pool) {
        zst_buffer_pool_destroy(s->pool);
        s->pool = NULL;
    }

    int size = av_image_get_buffer_size((enum AVPixelFormat)format, width, height, 1);
    if (size < 0) return ZST_ERROR;

    zst_buffer_pool_config_t pool_cfg = {
        .min_buffers = 2,
        .max_buffers = 8,
        .buffer_size = (size_t)size,
        .buffer_type = ZST_BUFFER_VIDEO_FRAME
    };

    s->pool = zst_buffer_pool_create(NULL, &pool_cfg);
    if (!s->pool) return ZST_ERROR;

    s->width = (uint32_t)width;
    s->height = (uint32_t)height;
    s->format = format;

    return ZST_OK;
}

static enum AVPixelFormat vaapi_get_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts)
{
    (void)ctx;
    const enum AVPixelFormat *p;
    for (p = pix_fmts; *p != -1; p++) {
        if (*p == AV_PIX_FMT_VAAPI) {
            return *p;
        }
    }
    return AV_PIX_FMT_NONE;
}

static zst_result_t
vaapi_dec_init_decoder(vaapi_video_decoder_t* s)
{
    enum AVCodecID codec_id = (strcmp(s->codec, "h265") == 0 || strcmp(s->codec, "hevc") == 0) ?
                              AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
    const AVCodec* codec = avcodec_find_decoder(codec_id);
    if (!codec) return ZST_ERROR;

    s->codec_ctx = avcodec_alloc_context3(codec);
    if (!s->codec_ctx) return ZST_ERROR;

    s->codec_ctx->hw_device_ctx = av_buffer_ref(s->hw_device_ctx);
    s->codec_ctx->get_format = vaapi_get_format;
    s->codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    s->codec_ctx->thread_count = 1;

    if (avcodec_open2(s->codec_ctx, codec, NULL) < 0) {
        avcodec_free_context(&s->codec_ctx);
        s->codec_ctx = NULL;
        return ZST_ERROR;
    }

    s->initialized = 1;
    return ZST_OK;
}

static zst_result_t
vaapi_dec_emit_buffer(zst_element_t* el, zst_buffer_t* buf, zst_buffer_t** out)
{
    if (!buf) return ZST_ERROR;

    if (el->nb_src_pads > 0 && el->src_pads[0]->peer) {
        zst_result_t ret = zst_pad_push(el->src_pads[0], buf);
        zst_buffer_unref(buf);
        return ret;
    }

    if (out && *out == NULL) {
        *out = buf;
        return ZST_OK;
    }

    zst_buffer_unref(buf);
    return ZST_OK;
}

static zst_result_t
vaapi_dec_emit_frame(zst_element_t* el, vaapi_video_decoder_t* s, zst_buffer_t** out)
{
    int width = s->hw_frame->width;
    int height = s->hw_frame->height;

    if (strcmp(s->memory_type, "dmabuf") == 0) {
        AVFrame* drm_frame = av_frame_alloc();
        if (!drm_frame) return ZST_ERROR;

        drm_frame->format = AV_PIX_FMT_DRM_PRIME;
        int ret = av_hwframe_map(drm_frame, s->hw_frame, AV_HWFRAME_MAP_READ);
        if (ret < 0) {
            av_frame_free(&drm_frame);
            ZST_LOG_WARN("vaapidec", "Failed to map VA-API frame to DRM PRIME/DMABUF");
            return ZST_ERROR;
        }

        AVDRMFrameDescriptor* desc = (AVDRMFrameDescriptor*)drm_frame->data[0];
        if (!desc || desc->nb_objects < 1) {
            av_frame_free(&drm_frame);
            return ZST_ERROR;
        }

        int fd = desc->objects[0].fd;
        size_t total_size = desc->objects[0].size;

        zst_allocator_t* allocator = zst_allocator_dmabuf_create();
        if (!allocator) {
            av_frame_free(&drm_frame);
            return ZST_ERROR;
        }

        void* ptr = zst_allocator_dmabuf_import(allocator, fd, total_size);
        if (!ptr) {
            zst_allocator_unref(allocator);
            av_frame_free(&drm_frame);
            return ZST_ERROR;
        }

        vaapi_dec_dmabuf_ctx_t* ctx = malloc(sizeof(*ctx));
        if (!ctx) {
            zst_allocator_free(allocator, ptr);
            zst_allocator_unref(allocator);
            av_frame_free(&drm_frame);
            return ZST_ERROR;
        }

        ctx->hw_frame = av_frame_clone(s->hw_frame);
        ctx->drm_frame = drm_frame;
        ctx->allocator = allocator;
        ctx->ptr = ptr;

        zst_video_frame_t* v_frame = calloc(1, sizeof(*v_frame));
        if (!v_frame) {
            release_dmabuf_ctx(ctx);
            return ZST_ERROR;
        }

        v_frame->width = width;
        v_frame->height = height;
        v_frame->format = AV_PIX_FMT_NV12;

        if (desc->nb_layers > 0) {
            uint32_t offset0 = desc->layers[0].planes[0].offset;
            v_frame->plane[0] = (uint8_t*)ptr + offset0;
            v_frame->stride[0] = desc->layers[0].planes[0].pitch;

            if (desc->layers[0].nb_planes > 1) {
                uint32_t offset1 = desc->layers[0].planes[1].offset;
                v_frame->plane[1] = (uint8_t*)ptr + offset1;
                v_frame->stride[1] = desc->layers[0].planes[1].pitch;
            }
        }

        zst_buffer_t* vbuf = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
        if (!vbuf) {
            free(v_frame);
            release_dmabuf_ctx(ctx);
            return ZST_ERROR;
        }

        vbuf->memory.type = ZST_MEMORY_DMABUF;
        vbuf->memory.data = ptr;
        vbuf->memory.size = total_size;
        vbuf->memory.priv = ctx;
        vbuf->memory.release = release_dmabuf_ctx;

        vbuf->payload = v_frame;
        vbuf->destroy = destroy_dmabuf_frame;

        int64_t pts = s->hw_frame->best_effort_timestamp;
        if (pts == AV_NOPTS_VALUE) pts = s->hw_frame->pts;
        if (pts != AV_NOPTS_VALUE) {
            vbuf->pts = (zst_time_t)pts;
            vbuf->dts = (zst_time_t)pts;
        }
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 16, 100)
        vbuf->duration = s->hw_frame->duration;
#else
        vbuf->duration = s->hw_frame->pkt_duration;
#endif

        return vaapi_dec_emit_buffer(el, vbuf, out);
    } else {
        AVFrame* sw_frame = av_frame_alloc();
        if (!sw_frame) return ZST_ERROR;

        sw_frame->format = AV_PIX_FMT_NV12;
        sw_frame->width = width;
        sw_frame->height = height;

        int ret = av_hwframe_transfer_data(sw_frame, s->hw_frame, 0);
        if (ret < 0) {
            av_frame_free(&sw_frame);
            ZST_LOG_WARN("vaapidec", "Failed to transfer VA-API hardware surface to CPU");
            return ZST_ERROR;
        }

        if (vaapi_dec_update_pool(s, width, height, sw_frame->format) != ZST_OK) {
            av_frame_free(&sw_frame);
            return ZST_ERROR;
        }

        zst_buffer_t* vbuf = NULL;
        if (zst_buffer_pool_acquire(s->pool, &vbuf, 0, 0) != ZST_OK) {
            av_frame_free(&sw_frame);
            return ZST_ERROR;
        }

        int64_t pts = s->hw_frame->best_effort_timestamp;
        if (pts == AV_NOPTS_VALUE) pts = s->hw_frame->pts;
        if (pts != AV_NOPTS_VALUE) {
            vbuf->pts = (zst_time_t)pts;
            vbuf->dts = (zst_time_t)pts;
        }
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 16, 100)
        vbuf->duration = s->hw_frame->duration;
#else
        vbuf->duration = s->hw_frame->pkt_duration;
#endif

        zst_video_frame_t* v_frame = vbuf->payload;
        if (!v_frame) {
            v_frame = calloc(1, sizeof(*v_frame));
            if (!v_frame) {
                zst_buffer_unref(vbuf);
                av_frame_free(&sw_frame);
                return ZST_ERROR;
            }
            vbuf->payload = v_frame;
            vbuf->destroy = dec_buf_free;
        }

        v_frame->width = (uint32_t)width;
        v_frame->height = (uint32_t)height;
        v_frame->format = (uint32_t)sw_frame->format;

        uint8_t* dst_data[4] = {0};
        int dst_linesize[4] = {0};
        int fill_ret = av_image_fill_arrays(dst_data, dst_linesize, vbuf->memory.data,
                                            (enum AVPixelFormat)sw_frame->format,
                                            width, height, 1);
        if (fill_ret < 0) {
            zst_buffer_unref(vbuf);
            av_frame_free(&sw_frame);
            return ZST_ERROR;
        }
        vbuf->memory.size = (size_t)fill_ret;

        av_image_copy(dst_data, dst_linesize,
                      (const uint8_t**)sw_frame->data, sw_frame->linesize,
                      (enum AVPixelFormat)sw_frame->format,
                      width, height);

        for (int i = 0; i < 4; i++) {
            v_frame->plane[i] = dst_data[i];
            v_frame->stride[i] = (uint32_t)dst_linesize[i];
        }

        av_frame_free(&sw_frame);
        return vaapi_dec_emit_buffer(el, vbuf, out);
    }
}

static zst_result_t
vaapi_dec_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    vaapi_video_decoder_t* s = el->priv;
    if (out) *out = NULL;

    if (!in) {
        return ZST_ERROR;
    }

    if (!s->initialized) {
        if (vaapi_dec_init_decoder(s) != ZST_OK) return ZST_ERROR;
    }

    AVPacket* av_pkt = NULL;
    AVPacket stack_pkt = {0};
    if (!(in->flags & ZST_BUFFER_FLAG_EOS)) {
        if (packet_from_buffer(in, &stack_pkt) != ZST_OK) {
            return ZST_ERROR;
        }
        av_pkt = &stack_pkt;
    }

    int ret = avcodec_send_packet(s->codec_ctx, av_pkt);
    if (ret == AVERROR(EAGAIN)) {
        while (1) {
            int recv_ret = avcodec_receive_frame(s->codec_ctx, s->hw_frame);
            if (recv_ret == AVERROR(EAGAIN) || recv_ret == AVERROR_EOF) {
                break;
            } else if (recv_ret < 0) {
                avcodec_flush_buffers(s->codec_ctx);
                if (av_pkt && av_pkt->buf) av_packet_unref(av_pkt);
                return recv_ret == AVERROR_INVALIDDATA ? ZST_AGAIN : ZST_ERROR;
            }

            zst_result_t emit_ret = vaapi_dec_emit_frame(el, s, out);
            av_frame_unref(s->hw_frame);
            if (emit_ret != ZST_OK) {
                if (av_pkt && av_pkt->buf) av_packet_unref(av_pkt);
                return emit_ret;
            }
        }
        ret = avcodec_send_packet(s->codec_ctx, av_pkt);
    }

    if (av_pkt && av_pkt->buf) {
        av_packet_unref(av_pkt);
    }

    if (ret < 0 && ret != AVERROR_EOF) {
        avcodec_flush_buffers(s->codec_ctx);
        return ret == AVERROR_INVALIDDATA ? ZST_AGAIN : ZST_ERROR;
    }

    while (1) {
        ret = avcodec_receive_frame(s->codec_ctx, s->hw_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            avcodec_flush_buffers(s->codec_ctx);
            return ret == AVERROR_INVALIDDATA ? ZST_AGAIN : ZST_ERROR;
        }

        zst_result_t emit_ret = vaapi_dec_emit_frame(el, s, out);
        av_frame_unref(s->hw_frame);
        if (emit_ret != ZST_OK) {
            return emit_ret;
        }
    }

    if (in->flags & ZST_BUFFER_FLAG_EOS) {
        zst_buffer_t* eos_buf = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
        if (!eos_buf) return ZST_ERROR;
        eos_buf->flags |= ZST_BUFFER_FLAG_EOS;
        return vaapi_dec_emit_buffer(el, eos_buf, out);
    }

    return ZST_OK;
}

static zst_result_t
vaapi_dec_open(zst_element_t* el)
{
    vaapi_video_decoder_t* s = el->priv;
    s->initialized = 0;
    s->codec_ctx = NULL;
    s->hw_device_ctx = NULL;
    s->hw_frame = av_frame_alloc();
    if (!s->hw_frame) return ZST_ERROR;
    s->pool = NULL;
    s->width = 0;
    s->height = 0;

    const char* device = s->device[0] ? s->device : NULL;
    int ret = av_hwdevice_ctx_create(&s->hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, device, NULL, 0);
    if (ret < 0) {
        char err[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err, sizeof(err));
        ZST_LOG_WARN("vaapidec", "VA-API device unavailable (%s): %s", device ? device : "default", err);
        return ZST_ERROR;
    }

    return ZST_OK;
}

static zst_result_t
vaapi_dec_close(zst_element_t* el)
{
    vaapi_video_decoder_t* s = el->priv;
    if (s->pool) {
        zst_buffer_pool_destroy(s->pool);
        s->pool = NULL;
    }
    if (s->codec_ctx) {
        avcodec_free_context(&s->codec_ctx);
        s->codec_ctx = NULL;
    }
    if (s->hw_device_ctx) {
        av_buffer_unref(&s->hw_device_ctx);
    }
    if (s->hw_frame) {
        av_frame_free(&s->hw_frame);
        s->hw_frame = NULL;
    }
    s->initialized = 0;
    return ZST_OK;
}

static zst_result_t
vaapi_dec_sink_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    if (!pad || !pad->parent || !buf) return ZST_ERROR;
    zst_buffer_t* out = NULL;
    zst_result_t ret = vaapi_dec_process(pad->parent, buf, &out);
    if (out) {
        if (pad->parent->nb_src_pads > 0 && pad->parent->src_pads[0]->peer) {
            zst_result_t push_ret = zst_pad_push(pad->parent->src_pads[0], out);
            zst_buffer_unref(out);
            if (ret == ZST_OK) ret = push_ret;
        } else {
            zst_buffer_unref(out);
        }
    }
    return ret;
}

static zst_caps_t*
vaapi_dec_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)filter;
    vaapi_video_decoder_t* s = el->priv;
    zst_caps_t* caps = zst_caps_create();
    if (!caps) return NULL;

    if (pad == s->sinkpad) {
        zst_caps_append(caps, zst_caps_struct_create_video("video/x-h264", 0, 0, 0.0, ""));
        zst_caps_append(caps, zst_caps_struct_create_video("video/x-h265", 0, 0, 0.0, ""));
    } else if (pad == s->srcpad) {
        zst_caps_append(caps, zst_caps_struct_create_video("video/x-raw",
                                                           (int)s->width,
                                                           (int)s->height,
                                                           0.0,
                                                           "NV12"));
    }

    return caps;
}

static zst_buffer_pool_t*
vaapi_dec_get_pool(zst_element_t* el)
{
    vaapi_video_decoder_t* s = el->priv;
    return s->pool;
}

static zst_result_t
vaapi_dec_set_property(zst_element_t* el, const char* name, const char* value)
{
    vaapi_video_decoder_t* s = el->priv;
    if (!name || !value) return ZST_ERROR;

    if (strcmp(name, "device") == 0) {
        snprintf(s->device, sizeof(s->device), "%s", value);
    } else if (strcmp(name, "codec") == 0) {
        snprintf(s->codec, sizeof(s->codec), "%s", value);
    } else if (strcmp(name, "memory-type") == 0) {
        if (strcmp(value, "cpu") == 0 || strcmp(value, "dmabuf") == 0) {
            snprintf(s->memory_type, sizeof(s->memory_type), "%s", value);
        } else {
            return ZST_ERROR;
        }
    } else {
        return ZST_ERROR;
    }
    return ZST_OK;
}

static zst_result_t
vaapi_dec_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    vaapi_video_decoder_t* s = el->priv;
    if (!name || !value_out || max_len == 0) return ZST_ERROR;

    if (strcmp(name, "device") == 0) {
        snprintf(value_out, max_len, "%s", s->device);
    } else if (strcmp(name, "codec") == 0) {
        snprintf(value_out, max_len, "%s", s->codec);
    } else if (strcmp(name, "memory-type") == 0) {
        snprintf(value_out, max_len, "%s", s->memory_type);
    } else {
        return ZST_ERROR;
    }
    return ZST_OK;
}

static zst_element_ops_t g_vaapi_dec_ops = {
    .name = "vaapidec",
    .open = vaapi_dec_open,
    .close = vaapi_dec_close,
    .process = vaapi_dec_process,
    .get_caps = vaapi_dec_get_caps,
    .set_property = vaapi_dec_set_property,
    .get_property = vaapi_dec_get_property,
    .get_pool = vaapi_dec_get_pool
};

zst_element_t*
zst_vaapi_video_decoder_create(void)
{
    vaapi_video_decoder_t* priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;

    snprintf(priv->device, sizeof(priv->device), "%s", VAAPI_DEC_DEFAULT_DEVICE);
    snprintf(priv->codec, sizeof(priv->codec), "h264");
    snprintf(priv->memory_type, sizeof(priv->memory_type), "cpu");

    zst_element_t* el = zst_element_create(&g_vaapi_dec_ops, priv);
    if (!el) {
        free(priv);
        return NULL;
    }

    priv->sinkpad = zst_pad_create("sink", ZST_PAD_SINK);
    priv->srcpad = zst_pad_create("src", ZST_PAD_SRC);
    if (!priv->sinkpad || !priv->srcpad) {
        zst_element_destroy(el);
        return NULL;
    }
    priv->sinkpad->push = vaapi_dec_sink_push;

    zst_element_add_pad(el, priv->sinkpad);
    zst_element_add_pad(el, priv->srcpad);
    return el;
}

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "vaapidec") == 0 || strcmp(name, "vaapi_video_decoder") == 0) {
        return zst_vaapi_video_decoder_create();
    }
    return NULL;
}

static const zst_property_spec_t g_vaapidec_properties[] = {
    { "device", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, VAAPI_DEC_DEFAULT_DEVICE, "DRM render node path" },
    { "codec", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "h264", "h264 or h265" },
    { "memory-type", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "cpu", "Output memory type: cpu, dmabuf" }
};

static const zst_pad_template_t g_vaapidec_pads[] = {
    { "sink", ZST_PAD_SINK, "video/x-h264" },
    { "sink", ZST_PAD_SINK, "video/x-h265" },
    { "src", ZST_PAD_SRC, "video/x-raw" }
};

static const zst_element_desc_t g_vaapidec_elements[] = {
    {
        .name = "vaapidec",
        .long_name = "VA-API Video Decoder",
        .category = "Codec/Decoder",
        .description = "Hardware H.264/H.265 video decoder using Linux VA-API",
        .author = "zstreamer",
        .properties = g_vaapidec_properties,
        .nb_properties = sizeof(g_vaapidec_properties) / sizeof(g_vaapidec_properties[0]),
        .pads = g_vaapidec_pads,
        .nb_pads = sizeof(g_vaapidec_pads) / sizeof(g_vaapidec_pads[0]),
        .create = NULL
    },
    {
        .name = "vaapi_video_decoder",
        .long_name = "VA-API Video Decoder",
        .category = "Codec/Decoder",
        .description = "Alias for vaapidec",
        .author = "zstreamer",
        .properties = g_vaapidec_properties,
        .nb_properties = sizeof(g_vaapidec_properties) / sizeof(g_vaapidec_properties[0]),
        .pads = g_vaapidec_pads,
        .nb_pads = sizeof(g_vaapidec_pads) / sizeof(g_vaapidec_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "vaapidec_plugin",
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
    if (nb_elements_out) *nb_elements_out = sizeof(g_vaapidec_elements) / sizeof(g_vaapidec_elements[0]);
    return g_vaapidec_elements;
}

ZST_PLUGIN_EXPORT
zst_plugin_t*
zst_get_plugin(void)
{
    zst_plugin_t* p = malloc(sizeof(*p));
    if (p) *p = g_plugin;
    return p;
}
#endif
