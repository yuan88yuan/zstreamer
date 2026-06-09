/*=============================================================================
    h264_encoder.c — x264 H.264 video encoder implementation
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <x264.h>

#include "mm_element.h"
#include "mm_buffer.h"

typedef struct {
    x264_t*         x264;
    x264_param_t    param;
    x264_picture_t  pic_in;
    x264_picture_t  pic_out;
    uint32_t        width;
    uint32_t        height;
    int             initialized;
} h264_encoder_t;

static mm_result_t
h264_open(mm_element_t* el)
{
    h264_encoder_t* s = el->priv;
    s->initialized = 0;
    s->x264 = NULL;
    return MM_OK;
}

static mm_result_t
h264_close(mm_element_t* el)
{
    h264_encoder_t* s = el->priv;
    if (s->x264) {
        x264_encoder_close(s->x264);
        x264_picture_clean(&s->pic_in);
        s->x264 = NULL;
    }
    s->initialized = 0;
    return MM_OK;
}

static mm_result_t
h264_init_encoder(h264_encoder_t* s, uint32_t width, uint32_t height)
{
    s->width = width;
    s->height = height;

    /* Preset ultrafast, tune zerolatency */
    if (x264_param_default_preset(&s->param, "ultrafast", "zerolatency") < 0) {
        return MM_ERROR;
    }

    s->param.i_csp = X264_CSP_I420;
    s->param.i_width = width;
    s->param.i_height = height;
    s->param.b_vfr_input = 0;
    s->param.i_fps_num = 30;
    s->param.i_fps_den = 1;
    
    /* Rate control: CRF 23 */
    s->param.rc.i_rc_method = X264_RC_CRF;
    s->param.rc.f_rf_constant = 23.0;

    /* Apply profile high */
    if (x264_param_apply_profile(&s->param, "high") < 0) {
        return MM_ERROR;
    }

    s->x264 = x264_encoder_open(&s->param);
    if (!s->x264) {
        return MM_ERROR;
    }

    if (x264_picture_alloc(&s->pic_in, s->param.i_csp, s->param.i_width, s->param.i_height) < 0) {
        x264_encoder_close(s->x264);
        s->x264 = NULL;
        return MM_ERROR;
    }

    s->initialized = 1;
    return MM_OK;
}

static void
h264_buf_free(mm_buffer_t* buf)
{
    if (buf && buf->memory.data) {
        free(buf->memory.data);
        buf->memory.data = NULL;
    }
}

static mm_result_t
h264_process(mm_element_t* el, mm_buffer_t* in, mm_buffer_t** out)
{
    h264_encoder_t* s = el->priv;
    if (!in) return MM_ERROR;

    /* If we get EOS, pass it downstream */
    if (in->flags & MM_BUFFER_FLAG_EOS) {
        mm_buffer_t* eos_buf = mm_buffer_create(MM_BUFFER_VIDEO_PACKET);
        if (eos_buf) {
            eos_buf->flags |= MM_BUFFER_FLAG_EOS;
            *out = eos_buf;
            return MM_OK;
        }
        return MM_ERROR;
    }

    /* Deduce width and height from the incoming frame payload */
    mm_video_frame_t* frame = in->payload;
    if (!frame) return MM_ERROR;

    if (!s->initialized) {
        if (h264_init_encoder(s, frame->width, frame->height) != MM_OK) {
            return MM_ERROR;
        }
    }

    /* Copy raw YUV planes into x264_picture_t */
    int y_size = s->width * s->height;
    int uv_size = y_size / 4;
    memcpy(s->pic_in.img.plane[0], frame->plane[0], y_size);
    memcpy(s->pic_in.img.plane[1], frame->plane[1], uv_size);
    memcpy(s->pic_in.img.plane[2], frame->plane[2], uv_size);

    s->pic_in.i_pts = in->pts;

    x264_nal_t* nals = NULL;
    int i_nals = 0;
    int frame_size = x264_encoder_encode(s->x264, &nals, &i_nals, &s->pic_in, &s->pic_out);
    if (frame_size < 0) {
        return MM_ERROR;
    }

    if (frame_size > 0 && nals) {
        mm_buffer_t* pkt = mm_buffer_create(MM_BUFFER_VIDEO_PACKET);
        if (!pkt) return MM_ERROR;

        uint8_t* enc_data = malloc(frame_size);
        if (!enc_data) {
            mm_buffer_unref(pkt);
            return MM_ERROR;
        }

        /* Concatenate all NAL units */
        uint8_t* ptr = enc_data;
        for (int i = 0; i < i_nals; i++) {
            memcpy(ptr, nals[i].p_payload, nals[i].i_payload);
            ptr += nals[i].i_payload;
        }

        pkt->memory.type = MM_MEMORY_CPU;
        pkt->memory.data = enc_data;
        pkt->memory.size = frame_size;
        pkt->pts = s->pic_out.i_pts;
        pkt->dts = s->pic_out.i_dts;
        pkt->destroy = h264_buf_free;

        *out = pkt;
    } else {
        *out = NULL;
    }

    return MM_OK;
}

static mm_element_ops_t g_ops = {
    .name    = "h264enc",
    .open    = h264_open,
    .close   = h264_close,
    .process = h264_process,
};

mm_element_t*
mm_h264_encoder_create(void)
{
    mm_element_t* el;
    h264_encoder_t* priv;
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

#ifdef BUILDING_PLUGIN
#include "mm_plugin.h"
#include <string.h>

static mm_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "h264enc") == 0) {
        return mm_h264_encoder_create();
    }
    return NULL;
}

static mm_plugin_t g_plugin = {
    .desc = {
        .name = "h264encoder_plugin",
        .author = "Antigravity",
        .version = "1.0.0",
        .init = NULL,
        .deinit = NULL
    },
    .create_element = plugin_create_element
};

MM_PLUGIN_EXPORT
mm_plugin_t*
mm_get_plugin(void)
{
    mm_plugin_t* p = malloc(sizeof(*p));
    if (p) {
        *p = g_plugin;
    }
    return p;
}
#endif