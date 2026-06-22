/*=============================================================================
    oneapi_encoder.c — Intel oneAPI/oneVPL hardware video encoder
=============================================================================*/

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mfxvideo.h>
#include <mfxdispatcher.h>

#include "zst_buffer.h"
#include "zst_buffer_pool.h"
#include "zst_element.h"
#include "zstreamer/elements/zst_oneapi_encoder.h"

#define ZST_ONEAPI_ALIGN16(v) (((v) + 15u) & ~15u)

typedef struct {
    mfxLoader loader;
    mfxSession session;
    mfxVideoParam params;
    mfxFrameSurface1 surface;
    mfxBitstream bitstream;
    uint8_t* nv12;
    uint8_t* bitstream_data;
    size_t bitstream_capacity;
    zst_buffer_pool_t* pool;
    uint32_t width;
    uint32_t height;
    uint32_t aligned_width;
    uint32_t aligned_height;
    int initialized;
    int eos_draining;
    char codec[16];
    char profile[32];
    char level[16];
    int64_t bitrate;
    int gop_size;
    int fps_num;
    int fps_den;
} oneapi_encoder_t;

static void oneapi_packet_release(void* priv)
{
    free(priv);
}

static mfxU32 oneapi_codec_id(const oneapi_encoder_t* s)
{
    if (strcmp(s->codec, "h265") == 0 || strcmp(s->codec, "hevc") == 0) return MFX_CODEC_HEVC;
    return MFX_CODEC_AVC;
}

static mfxU16 oneapi_profile_id(const oneapi_encoder_t* s)
{
    if (s->profile[0] == '\0') return MFX_PROFILE_UNKNOWN;
    if (oneapi_codec_id(s) == MFX_CODEC_HEVC) {
        if (strcmp(s->profile, "main10") == 0 || strcmp(s->profile, "main-10") == 0) return MFX_PROFILE_HEVC_MAIN10;
        if (strcmp(s->profile, "rext") == 0) return MFX_PROFILE_HEVC_REXT;
        if (strcmp(s->profile, "scc") == 0) return MFX_PROFILE_HEVC_SCC;
        return MFX_PROFILE_HEVC_MAIN;
    }
    if (strcmp(s->profile, "baseline") == 0) return MFX_PROFILE_AVC_BASELINE;
    if (strcmp(s->profile, "constrained-baseline") == 0) return MFX_PROFILE_AVC_CONSTRAINED_BASELINE;
    if (strcmp(s->profile, "main") == 0) return MFX_PROFILE_AVC_MAIN;
    if (strcmp(s->profile, "high10") == 0) return MFX_PROFILE_AVC_HIGH10;
    return MFX_PROFILE_AVC_HIGH;
}

static mfxU16 oneapi_level_id(const oneapi_encoder_t* s)
{
    if (s->level[0] == '\0') return MFX_LEVEL_UNKNOWN;
    int major = 0;
    int minor = 0;
    if (sscanf(s->level, "%d.%d", &major, &minor) >= 1 && major > 0) {
        return (mfxU16)(major * 10 + minor);
    }
    int raw = atoi(s->level);
    return raw > 0 ? (mfxU16)raw : MFX_LEVEL_UNKNOWN;
}

static zst_result_t oneapi_open(zst_element_t* el)
{
    oneapi_encoder_t* s = el->priv;
    s->loader = NULL;
    s->session = NULL;
    s->initialized = 0;
    s->eos_draining = 0;
    s->pool = NULL;
    s->nv12 = NULL;
    s->bitstream_data = NULL;
    s->bitstream_capacity = 0;
    return ZST_OK;
}

static zst_result_t oneapi_close(zst_element_t* el)
{
    oneapi_encoder_t* s = el->priv;
    if (s->initialized) MFXVideoENCODE_Close(s->session);
    if (s->session) MFXClose(s->session);
    if (s->loader) MFXUnload(s->loader);
    if (s->pool) zst_buffer_pool_destroy(s->pool);
    free(s->nv12);
    free(s->bitstream_data);
    s->session = NULL;
    s->loader = NULL;
    s->pool = NULL;
    s->nv12 = NULL;
    s->bitstream_data = NULL;
    s->initialized = 0;
    return ZST_OK;
}

static zst_result_t oneapi_create_hardware_session(oneapi_encoder_t* s)
{
    s->loader = MFXLoad();
    if (!s->loader) return ZST_ERROR;

    mfxVariant impl_value;
    memset(&impl_value, 0, sizeof(impl_value));
    impl_value.Version.Version = MFX_VARIANT_VERSION;
    impl_value.Type = MFX_VARIANT_TYPE_U32;
    impl_value.Data.U32 = MFX_IMPL_TYPE_HARDWARE;

    mfxConfig cfg = MFXCreateConfig(s->loader);
    if (!cfg) return ZST_ERROR;
    mfxStatus sts = MFXSetConfigFilterProperty(cfg, (const mfxU8*)"mfxImplDescription.Impl", impl_value);
    if (sts < MFX_ERR_NONE) return ZST_ERROR;

    memset(&impl_value, 0, sizeof(impl_value));
    impl_value.Version.Version = MFX_VARIANT_VERSION;
    impl_value.Type = MFX_VARIANT_TYPE_U32;
    impl_value.Data.U32 = oneapi_codec_id(s);
    cfg = MFXCreateConfig(s->loader);
    if (!cfg) return ZST_ERROR;
    sts = MFXSetConfigFilterProperty(cfg, (const mfxU8*)"mfxImplDescription.mfxEncoderDescription.encoder.CodecID", impl_value);
    if (sts < MFX_ERR_NONE) return ZST_ERROR;

    sts = MFXCreateSession(s->loader, 0, &s->session);
    return sts >= MFX_ERR_NONE ? ZST_OK : ZST_ERROR;
}

static zst_result_t oneapi_init_encoder(oneapi_encoder_t* s, uint32_t width, uint32_t height)
{
    if (oneapi_create_hardware_session(s) != ZST_OK) return ZST_ERROR;

    s->width = width;
    s->height = height;
    s->aligned_width = ZST_ONEAPI_ALIGN16(width);
    s->aligned_height = ZST_ONEAPI_ALIGN16(height);
    size_t nv12_size = (size_t)s->aligned_width * s->aligned_height * 3 / 2;
    s->bitstream_capacity = (size_t)width * height;
    if (s->bitstream_capacity < 1024 * 1024) s->bitstream_capacity = 1024 * 1024;

    s->nv12 = calloc(1, nv12_size);
    s->bitstream_data = malloc(s->bitstream_capacity);
    if (!s->nv12 || !s->bitstream_data) return ZST_ERROR;

    memset(&s->params, 0, sizeof(s->params));
    s->params.mfx.CodecId = oneapi_codec_id(s);
    s->params.mfx.TargetUsage = MFX_TARGETUSAGE_BALANCED;
    s->params.mfx.TargetKbps = (mfxU16)((s->bitrate > 0 ? s->bitrate : 4000000) / 1000);
    s->params.mfx.RateControlMethod = MFX_RATECONTROL_CBR;
    s->params.mfx.GopPicSize = (mfxU16)(s->gop_size > 0 ? s->gop_size : 30);
    s->params.mfx.IdrInterval = 1;
    s->params.mfx.CodecProfile = oneapi_profile_id(s);
    s->params.mfx.CodecLevel = oneapi_level_id(s);
    s->params.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
    s->params.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    s->params.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    s->params.mfx.FrameInfo.CropW = (mfxU16)width;
    s->params.mfx.FrameInfo.CropH = (mfxU16)height;
    s->params.mfx.FrameInfo.Width = (mfxU16)s->aligned_width;
    s->params.mfx.FrameInfo.Height = (mfxU16)s->aligned_height;
    s->params.mfx.FrameInfo.FrameRateExtN = (mfxU32)(s->fps_num > 0 ? s->fps_num : 30);
    s->params.mfx.FrameInfo.FrameRateExtD = (mfxU32)(s->fps_den > 0 ? s->fps_den : 1);
    s->params.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY;
    s->params.AsyncDepth = 1;

    mfxStatus sts = MFXVideoENCODE_Init(s->session, &s->params);
    if (sts < MFX_ERR_NONE) return ZST_ERROR;

    memset(&s->surface, 0, sizeof(s->surface));
    s->surface.Info = s->params.mfx.FrameInfo;
    s->surface.Data.Y = s->nv12;
    s->surface.Data.UV = s->nv12 + (size_t)s->aligned_width * s->aligned_height;
    s->surface.Data.Pitch = (mfxU16)s->aligned_width;

    zst_buffer_pool_config_t pool_cfg = {
        .min_buffers = 2,
        .max_buffers = 8,
        .buffer_size = s->bitstream_capacity,
        .buffer_type = ZST_BUFFER_VIDEO_PACKET
    };
    s->pool = zst_buffer_pool_create(NULL, &pool_cfg);
    if (!s->pool) return ZST_ERROR;

    s->initialized = 1;
    s->eos_draining = 0;
    return ZST_OK;
}

static void oneapi_copy_yuv420p_to_nv12(oneapi_encoder_t* s, const zst_video_frame_t* frame)
{
    uint8_t* y_dst = s->nv12;
    uint8_t* uv_dst = s->nv12 + (size_t)s->aligned_width * s->aligned_height;
    for (uint32_t r = 0; r < s->height; r++) {
        memcpy(y_dst + (size_t)r * s->aligned_width,
               frame->plane[0] + (size_t)r * frame->stride[0],
               s->width);
    }
    for (uint32_t r = 0; r < s->height / 2; r++) {
        const uint8_t* u = frame->plane[1] + (size_t)r * frame->stride[1];
        const uint8_t* v = frame->plane[2] + (size_t)r * frame->stride[2];
        uint8_t* uv = uv_dst + (size_t)r * s->aligned_width;
        for (uint32_t c = 0; c < s->width / 2; c++) {
            uv[c * 2] = u[c];
            uv[c * 2 + 1] = v[c];
        }
    }
}

static zst_result_t oneapi_encode(oneapi_encoder_t* s, mfxFrameSurface1* surface, zst_buffer_t** out)
{
    mfxSyncPoint syncp = NULL;
    memset(&s->bitstream, 0, sizeof(s->bitstream));
    s->bitstream.Data = s->bitstream_data;
    s->bitstream.MaxLength = (mfxU32)s->bitstream_capacity;

    mfxStatus sts = MFXVideoENCODE_EncodeFrameAsync(s->session, NULL, surface, &s->bitstream, &syncp);
    if (sts == MFX_ERR_MORE_DATA) {
        *out = NULL;
        return ZST_OK;
    }
    if (sts < MFX_ERR_NONE) return ZST_ERROR;
    if (!syncp) {
        *out = NULL;
        return ZST_OK;
    }
    sts = MFXVideoCORE_SyncOperation(s->session, syncp, 60000);
    if (sts < MFX_ERR_NONE) return ZST_ERROR;
    if (s->bitstream.DataLength == 0) {
        *out = NULL;
        return ZST_OK;
    }

    zst_buffer_t* pkt = zst_buffer_create(ZST_BUFFER_VIDEO_PACKET);
    if (!pkt) return ZST_ERROR;
    uint8_t* data = malloc(s->bitstream.DataLength);
    if (!data) {
        zst_buffer_unref(pkt);
        return ZST_ERROR;
    }
    memcpy(data, s->bitstream.Data + s->bitstream.DataOffset, s->bitstream.DataLength);
    pkt->memory.type = ZST_MEMORY_CPU;
    pkt->memory.data = data;
    pkt->memory.size = s->bitstream.DataLength;
    pkt->memory.priv = data;
    pkt->memory.release = oneapi_packet_release;
    *out = pkt;
    return ZST_OK;
}

static zst_result_t oneapi_copy_frame_to_surface(oneapi_encoder_t* s, const zst_buffer_t* in, const zst_video_frame_t* frame)
{
    if (in->memory.type == ZST_MEMORY_ONEAPI) {
        return ZST_ERROR_NOT_IMPLEMENTED;
    }
    if (!frame->plane[0] || !frame->plane[1] || !frame->plane[2]) return ZST_ERROR;
    oneapi_copy_yuv420p_to_nv12(s, frame);
    return ZST_OK;
}

static zst_result_t oneapi_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    oneapi_encoder_t* s = el->priv;
    if (!out) return ZST_ERROR;
    *out = NULL;
    if (in && (in->flags & ZST_BUFFER_FLAG_EOS)) {
        if (s->initialized) {
            zst_result_t r = oneapi_encode(s, NULL, out);
            if (r != ZST_OK || *out) return r;
        }
        zst_buffer_t* eos = zst_buffer_create(ZST_BUFFER_VIDEO_PACKET);
        if (!eos) return ZST_ERROR;
        eos->flags |= ZST_BUFFER_FLAG_EOS;
        *out = eos;
        return ZST_OK;
    }
    if (!in || !in->payload) return ZST_ERROR;
    zst_video_frame_t* frame = in->payload;
    if (!s->initialized && oneapi_init_encoder(s, frame->width, frame->height) != ZST_OK) return ZST_ERROR;

    zst_result_t copy_result = oneapi_copy_frame_to_surface(s, in, frame);
    if (copy_result != ZST_OK) return copy_result;
    s->surface.Data.TimeStamp = in->pts;
    zst_result_t r = oneapi_encode(s, &s->surface, out);
    if (r == ZST_OK && *out) {
        (*out)->pts = in->pts;
        (*out)->dts = in->dts;
        (*out)->duration = in->duration;
    }
    return r;
}

static int oneapi_is_config_property(const char* name)
{
    return name && (strcmp(name, "codec") == 0 || strcmp(name, "bitrate") == 0 ||
                    strcmp(name, "gop-size") == 0 || strcmp(name, "gop") == 0 ||
                    strcmp(name, "fps") == 0 || strcmp(name, "profile") == 0 ||
                    strcmp(name, "level") == 0);
}

static zst_result_t oneapi_set_property(zst_element_t* el, const char* name, const char* value)
{
    oneapi_encoder_t* s = el->priv;
    if (!name || !value) return ZST_ERROR;
    if (s->initialized && oneapi_is_config_property(name)) return ZST_ERROR;
    if (strcmp(name, "codec") == 0) {
        if (strcmp(value, "h264") != 0 && strcmp(value, "avc") != 0 &&
            strcmp(value, "h265") != 0 && strcmp(value, "hevc") != 0) return ZST_ERROR;
        snprintf(s->codec, sizeof(s->codec), "%s", value);
        return ZST_OK;
    }
    if (strcmp(name, "bitrate") == 0) {
        s->bitrate = atoll(value);
        if (s->bitrate < 0) s->bitrate = 0;
        return ZST_OK;
    }
    if (strcmp(name, "gop-size") == 0 || strcmp(name, "gop") == 0) {
        s->gop_size = atoi(value);
        if (s->gop_size < 1) s->gop_size = 1;
        return ZST_OK;
    }
    if (strcmp(name, "fps") == 0) {
        int num = 0, den = 1;
        if (sscanf(value, "%d/%d", &num, &den) >= 1 && num > 0) {
            s->fps_num = num;
            s->fps_den = den > 0 ? den : 1;
            return ZST_OK;
        }
        return ZST_ERROR;
    }
    if (strcmp(name, "profile") == 0) {
        snprintf(s->profile, sizeof(s->profile), "%s", value);
        return ZST_OK;
    }
    if (strcmp(name, "level") == 0) {
        snprintf(s->level, sizeof(s->level), "%s", value);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t oneapi_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    oneapi_encoder_t* s = el->priv;
    if (!name || !value_out || max_len == 0) return ZST_ERROR;
    if (strcmp(name, "codec") == 0) snprintf(value_out, max_len, "%s", s->codec);
    else if (strcmp(name, "bitrate") == 0) snprintf(value_out, max_len, "%" PRId64, s->bitrate);
    else if (strcmp(name, "gop-size") == 0 || strcmp(name, "gop") == 0) snprintf(value_out, max_len, "%d", s->gop_size);
    else if (strcmp(name, "fps") == 0) snprintf(value_out, max_len, "%d/%d", s->fps_num, s->fps_den);
    else if (strcmp(name, "profile") == 0) snprintf(value_out, max_len, "%s", s->profile);
    else if (strcmp(name, "level") == 0) snprintf(value_out, max_len, "%s", s->level);
    else return ZST_ERROR;
    return ZST_OK;
}

static zst_buffer_pool_t* oneapi_get_pool(zst_element_t* el)
{
    oneapi_encoder_t* s = el->priv;
    return s->pool;
}

static zst_element_ops_t g_ops = {
    .name = "oneapienc",
    .open = oneapi_open,
    .close = oneapi_close,
    .process = oneapi_process,
    .set_property = oneapi_set_property,
    .get_property = oneapi_get_property,
    .get_pool = oneapi_get_pool
};

zst_element_t* zst_oneapi_encoder_create(void)
{
    oneapi_encoder_t* priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;
    snprintf(priv->codec, sizeof(priv->codec), "h264");
    snprintf(priv->profile, sizeof(priv->profile), "high");
    priv->level[0] = '\0';
    priv->bitrate = 4000000;
    priv->gop_size = 30;
    priv->fps_num = 30;
    priv->fps_den = 1;

    zst_element_t* el = zst_element_create(&g_ops, priv);
    if (!el) {
        free(priv);
        return NULL;
    }
    zst_element_add_pad(el, zst_pad_create("sink", ZST_PAD_SINK));
    zst_element_add_pad(el, zst_pad_create("src", ZST_PAD_SRC));
    return el;
}

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"

static zst_element_t* plugin_create_element(const char* name)
{
    if (name && strcmp(name, "oneapienc") == 0) return zst_oneapi_encoder_create();
    return NULL;
}

static const zst_pad_template_t g_oneapienc_pads[] = {
    { "sink", ZST_PAD_SINK, "video/x-raw" },
    { "src", ZST_PAD_SRC, "video/x-h264" }
};

static const zst_element_desc_t g_oneapienc_elements[] = {
    {
        .name = "oneapienc",
        .long_name = "Intel oneAPI Video Encoder",
        .category = "Codec/Encoder",
        .description = "Encodes raw video using Intel oneVPL hardware acceleration",
        .author = "zstreamer",
        .properties = NULL,
        .nb_properties = 0,
        .pads = g_oneapienc_pads,
        .nb_pads = sizeof(g_oneapienc_pads) / sizeof(g_oneapienc_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = { .name = "oneapienc_plugin", .author = "zstreamer", .version = "1.0.0" },
    .create_element = plugin_create_element
};

ZST_PLUGIN_EXPORT const zst_element_desc_t* zst_get_plugin_elements(uint32_t* nb_elements_out)
{
    if (nb_elements_out) *nb_elements_out = sizeof(g_oneapienc_elements) / sizeof(g_oneapienc_elements[0]);
    return g_oneapienc_elements;
}

ZST_PLUGIN_EXPORT zst_plugin_t* zst_get_plugin(void)
{
    zst_plugin_t* p = malloc(sizeof(*p));
    if (p) *p = g_plugin;
    return p;
}
#endif
