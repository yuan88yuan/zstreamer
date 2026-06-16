/*=============================================================================
    nv_video_encoder.c — NVIDIA V4L2 video encoder implementation
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>
#include <errno.h>
#include <linux/videodev2.h>

#include "zst_element.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_buffer_pool.h"
#include "zst_caps.h"
#include "zst_log.h"

/* NVIDIA-Specific Controls */
#ifndef V4L2_CID_MPEG_BASE
#define V4L2_CID_MPEG_BASE 0x00990900
#endif

#ifndef V4L2_PIX_FMT_H265
#define V4L2_PIX_FMT_H265 v4l2_fourcc('H', '2', '6', '5')
#endif

#define V4L2_CID_MPEG_VIDEOENC_HW_PRESET_TYPE_PARAM   (V4L2_CID_MPEG_BASE+540)
#define V4L2_CID_MPEG_VIDEOENC_PROFILE                (V4L2_CID_MPEG_BASE+527)

typedef struct {
    int             fd;
    int             initialized;
    zst_buffer_pool_t* pool;
    uint32_t        width;
    uint32_t        height;
    zst_pad_t*      sinkpad;
    zst_pad_t*      srcpad;

    char            codec[32];      /* "h264" or "h265" */
    char            preset[32];
    char            profile[32];
    int64_t         bitrate;
    int             gop_size;

    struct v4l2_buffer* capture_buffers;
    struct v4l2_buffer* output_buffers;
    uint32_t        nb_capture_buffers;
    uint32_t        nb_output_buffers;
} nv_video_encoder_t;

static zst_result_t
nv_video_encoder_open(zst_element_t* el)
{
    nv_video_encoder_t* s = el->priv;
    s->fd = open("/dev/nvhost-msenc", O_RDWR | O_NONBLOCK);
    if (s->fd < 0) {
        ZST_LOG_ERROR("nvenc", "Failed to open /dev/nvhost-msenc (ensure you are on Jetson)");
        return ZST_ERROR;
    }
    s->initialized = 0;
    s->pool = NULL;
    return ZST_OK;
}

static zst_result_t
nv_video_encoder_close(zst_element_t* el)
{
    nv_video_encoder_t* s = el->priv;
    if (s->fd >= 0) {
        if (s->capture_buffers) {
            for (uint32_t i = 0; i < s->nb_capture_buffers; i++) {
                if (s->capture_buffers[i].m.userptr && s->capture_buffers[i].m.userptr != (unsigned long)MAP_FAILED) {
                    munmap((void*)s->capture_buffers[i].m.userptr, s->capture_buffers[i].length);
                }
                if (s->capture_buffers[i].m.planes) {
                    free(s->capture_buffers[i].m.planes);
                }
            }
            free(s->capture_buffers);
            s->capture_buffers = NULL;
        }
        if (s->output_buffers) {
            for (uint32_t i = 0; i < s->nb_output_buffers; i++) {
                if (s->output_buffers[i].m.planes) {
                    for(int j = 0; j < 3; j++) {
                        if (s->output_buffers[i].m.planes[j].m.userptr && s->output_buffers[i].m.planes[j].m.userptr != (unsigned long)MAP_FAILED) {
                            munmap((void*)s->output_buffers[i].m.planes[j].m.userptr, s->output_buffers[i].m.planes[j].length);
                        }
                    }
                    free(s->output_buffers[i].m.planes);
                }
            }
            free(s->output_buffers);
            s->output_buffers = NULL;
        }
        close(s->fd);
        s->fd = -1;
    }
    if (s->pool) {
        zst_buffer_pool_destroy(s->pool);
        s->pool = NULL;
    }
    s->initialized = 0;
    return ZST_OK;
}

static zst_result_t
nv_video_encoder_start(zst_element_t* el)
{
    nv_video_encoder_t* s = el->priv;
    if (s->fd < 0) return ZST_ERROR;
    return ZST_OK;
}

static zst_result_t
nv_video_encoder_stop(zst_element_t* el)
{
    nv_video_encoder_t* s = el->priv;
    if (s->fd >= 0 && s->initialized) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        ioctl(s->fd, VIDIOC_STREAMOFF, &type);
        type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        ioctl(s->fd, VIDIOC_STREAMOFF, &type);
    }
    return ZST_OK;
}

static zst_result_t
nv_video_encoder_init_v4l2(nv_video_encoder_t* s, uint32_t width, uint32_t height)
{
    s->width = width;
    s->height = height;

    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = width;
    fmt.fmt.pix_mp.height = height;
    if (strcmp(s->codec, "h265") == 0) {
        fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H265;
    } else {
        fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
    }
    fmt.fmt.pix_mp.num_planes = 1;
    if (ioctl(s->fd, VIDIOC_S_FMT, &fmt) < 0) return ZST_ERROR;

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    fmt.fmt.pix_mp.width = width;
    fmt.fmt.pix_mp.height = height;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420M;
    fmt.fmt.pix_mp.num_planes = 3;
    if (ioctl(s->fd, VIDIOC_S_FMT, &fmt) < 0) return ZST_ERROR;

    struct v4l2_ext_control ctrl = {0};
    struct v4l2_ext_controls ctrls = {0};
    ctrls.count = 1;
    ctrls.controls = &ctrl;

    ctrl.id = V4L2_CID_MPEG_VIDEO_BITRATE;
    ctrl.value = s->bitrate;
    ioctl(s->fd, VIDIOC_S_EXT_CTRLS, &ctrls);

    struct v4l2_requestbuffers req = {0};
    req.count = 6;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(s->fd, VIDIOC_REQBUFS, &req) < 0) return ZST_ERROR;
    s->nb_capture_buffers = req.count;
    s->capture_buffers = calloc(req.count, sizeof(struct v4l2_buffer));
    for (uint32_t i = 0; i < req.count; i++) {
        struct v4l2_buffer buf = {0};
        struct v4l2_plane* planes = calloc(1, sizeof(struct v4l2_plane));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.length = 1;
        buf.m.planes = planes;
        if (ioctl(s->fd, VIDIOC_QUERYBUF, &buf) < 0) return ZST_ERROR;
        // Allocate space for mapping in capture_buffers
        s->capture_buffers[i].length = planes[0].length;
        s->capture_buffers[i].m.userptr = (unsigned long)mmap(NULL, planes[0].length, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd, planes[0].m.mem_offset);
        s->capture_buffers[i].m.planes = planes;
    }

    memset(&req, 0, sizeof(req));
    req.count = 6;
    req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(s->fd, VIDIOC_REQBUFS, &req) < 0) return ZST_ERROR;
    s->nb_output_buffers = req.count;
    s->output_buffers = calloc(req.count, sizeof(struct v4l2_buffer));
    for (uint32_t i = 0; i < req.count; i++) {
        struct v4l2_buffer buf = {0};
        struct v4l2_plane* planes = calloc(3, sizeof(struct v4l2_plane));
        buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.length = 3;
        buf.m.planes = planes;
        if (ioctl(s->fd, VIDIOC_QUERYBUF, &buf) < 0) return ZST_ERROR;
        // Map output planes
        for (int j = 0; j < 3; j++) {
            planes[j].m.userptr = (unsigned long)mmap(NULL, planes[j].length, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd, planes[j].m.mem_offset);
        }
        s->output_buffers[i].m.planes = planes;
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(s->fd, VIDIOC_STREAMON, &type) < 0) return ZST_ERROR;
    type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (ioctl(s->fd, VIDIOC_STREAMON, &type) < 0) return ZST_ERROR;

    zst_buffer_pool_config_t pool_cfg = {
        .min_buffers = 2,
        .max_buffers = 16,
        .buffer_size = width * height,
        .buffer_type = ZST_BUFFER_VIDEO_PACKET
    };
    s->pool = zst_buffer_pool_create(NULL, &pool_cfg);

    s->initialized = 1;
    return ZST_OK;
}

static zst_result_t
nv_video_encoder_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    nv_video_encoder_t* s = el->priv;
    if (out) *out = NULL;
    if (!in || s->fd < 0) return ZST_ERROR;

    zst_video_frame_t* frame = in->payload;
    if (!frame) return ZST_ERROR;

    if (!s->initialized) {
        if (nv_video_encoder_init_v4l2(s, frame->width, frame->height) != ZST_OK) {
            return ZST_ERROR;
        }
    }

    struct v4l2_buffer vbuf_out = {0};
    struct v4l2_plane planes_out[3] = {0};
    vbuf_out.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    vbuf_out.memory = V4L2_MEMORY_MMAP;
    vbuf_out.length = 3;
    vbuf_out.m.planes = planes_out;
    vbuf_out.timestamp.tv_sec = in->pts / 1000000000;
    vbuf_out.timestamp.tv_usec = (in->pts % 1000000000) / 1000;

    int dq_out = ioctl(s->fd, VIDIOC_DQBUF, &vbuf_out);
    if (dq_out < 0 && errno != EAGAIN) {
        struct pollfd pfd = { .fd = s->fd, .events = POLLOUT };
        poll(&pfd, 1, 100);
        dq_out = ioctl(s->fd, VIDIOC_DQBUF, &vbuf_out);
    }

    if (dq_out >= 0 || errno == EAGAIN) {
        if (dq_out < 0) {
            static int fallback_out_idx = 0;
            vbuf_out.index = fallback_out_idx;
            fallback_out_idx = (fallback_out_idx + 1) % s->nb_output_buffers;
        }

        for (int j = 0; j < 3; j++) {
            size_t bytes = (j == 0) ? frame->stride[0] * s->height : frame->stride[j] * (s->height / 2);
            memcpy((void*)s->output_buffers[vbuf_out.index].m.planes[j].m.userptr, frame->plane[j], bytes);
            planes_out[j].bytesused = bytes;
            planes_out[j].length = s->output_buffers[vbuf_out.index].m.planes[j].length;
        }

        ioctl(s->fd, VIDIOC_QBUF, &vbuf_out);
    }

    struct v4l2_buffer vbuf_cap = {0};
    struct v4l2_plane planes_cap[1] = {0};
    vbuf_cap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    vbuf_cap.memory = V4L2_MEMORY_MMAP;
    vbuf_cap.length = 1;
    vbuf_cap.m.planes = planes_cap;

    struct pollfd pfd_cap = { .fd = s->fd, .events = POLLIN };
    if (poll(&pfd_cap, 1, 100) > 0) {
        if (ioctl(s->fd, VIDIOC_DQBUF, &vbuf_cap) == 0) {
            zst_buffer_t* pkt = NULL;
            if (zst_buffer_pool_acquire(s->pool, &pkt, 0, 0) == ZST_OK) {
                pkt->memory.size = planes_cap[0].bytesused;
                memcpy(pkt->memory.data, (void*)s->capture_buffers[vbuf_cap.index].m.userptr, planes_cap[0].bytesused);
                pkt->pts = (zst_time_t)vbuf_cap.timestamp.tv_sec * 1000000000ULL + vbuf_cap.timestamp.tv_usec * 1000ULL;
                pkt->dts = pkt->pts;
                pkt->duration = in->duration;
                *out = pkt;
            }
            ioctl(s->fd, VIDIOC_QBUF, &vbuf_cap);
        }
    }

    return ZST_OK;
}

static zst_caps_t*
nv_video_encoder_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)filter;
    nv_video_encoder_t* s = el->priv;
    zst_caps_t* caps = zst_caps_create();
    if (!caps) return NULL;

    if (pad == s->sinkpad) {
        zst_caps_append(caps, zst_caps_struct_create_video("video/x-raw", 0, 0, 0.0, "YUV420M"));
    } else if (pad == s->srcpad) {
        if (strcmp(s->codec, "h265") == 0) {
            zst_caps_append(caps, zst_caps_struct_create_video("video/x-h265",
                                                               (int)s->width,
                                                               (int)s->height,
                                                               30.0,
                                                               s->profile[0] ? s->profile : "main"));
        } else {
            zst_caps_append(caps, zst_caps_struct_create_video("video/x-h264",
                                                               (int)s->width,
                                                               (int)s->height,
                                                               30.0,
                                                               s->profile[0] ? s->profile : "main"));
        }
    }
    return caps;
}

static zst_result_t
nv_video_encoder_set_property(zst_element_t* el, const char* name, const char* value)
{
    nv_video_encoder_t* s = el->priv;
    if (!name || !value) return ZST_ERROR;

    if (strcmp(name, "codec") == 0) {
        snprintf(s->codec, sizeof(s->codec), "%s", value);
        return ZST_OK;
    } else if (strcmp(name, "preset") == 0) {
        snprintf(s->preset, sizeof(s->preset), "%s", value);
        return ZST_OK;
    } else if (strcmp(name, "profile") == 0) {
        snprintf(s->profile, sizeof(s->profile), "%s", value);
        return ZST_OK;
    } else if (strcmp(name, "bitrate") == 0) {
        s->bitrate = atoll(value);
        return ZST_OK;
    } else if (strcmp(name, "gop-size") == 0) {
        s->gop_size = atoi(value);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t
nv_video_encoder_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    nv_video_encoder_t* s = el->priv;
    if (!name || !value_out || max_len == 0) return ZST_ERROR;

    if (strcmp(name, "codec") == 0) {
        snprintf(value_out, max_len, "%s", s->codec);
    } else if (strcmp(name, "preset") == 0) {
        snprintf(value_out, max_len, "%s", s->preset);
    } else if (strcmp(name, "profile") == 0) {
        snprintf(value_out, max_len, "%s", s->profile);
    } else if (strcmp(name, "bitrate") == 0) {
        snprintf(value_out, max_len, "%" PRId64, s->bitrate);
    } else if (strcmp(name, "gop-size") == 0) {
        snprintf(value_out, max_len, "%d", s->gop_size);
    } else {
        return ZST_ERROR;
    }
    return ZST_OK;
}

static zst_buffer_pool_t*
nv_video_encoder_get_pool(zst_element_t* el)
{
    nv_video_encoder_t* s = el->priv;
    return s->pool;
}

static zst_result_t
nv_video_encoder_sink_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    if (!pad || !pad->parent || !buf) return ZST_ERROR;
    zst_buffer_t* out = NULL;
    zst_result_t ret = nv_video_encoder_process(pad->parent, buf, &out);

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

static zst_element_ops_t g_nv_video_encoder_ops = {
    .name    = "nvenc",
    .open    = nv_video_encoder_open,
    .close   = nv_video_encoder_close,
    .start   = nv_video_encoder_start,
    .stop    = nv_video_encoder_stop,
    .process = nv_video_encoder_process,
    .get_caps = nv_video_encoder_get_caps,
    .set_property = nv_video_encoder_set_property,
    .get_property = nv_video_encoder_get_property,
    .get_pool = nv_video_encoder_get_pool
};

zst_element_t*
zst_nv_video_encoder_create(void)
{
    nv_video_encoder_t* priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;

    snprintf(priv->codec, sizeof(priv->codec), "h264");
    snprintf(priv->preset, sizeof(priv->preset), "fast");
    snprintf(priv->profile, sizeof(priv->profile), "main");
    priv->bitrate = 4000000;
    priv->gop_size = 30;

    zst_element_t* el = zst_element_create(&g_nv_video_encoder_ops, priv);
    if (!el) {
        free(priv);
        return NULL;
    }

    priv->sinkpad = zst_pad_create("sink", ZST_PAD_SINK);
    priv->srcpad  = zst_pad_create("src",  ZST_PAD_SRC);
    if (!priv->sinkpad || !priv->srcpad) {
        zst_element_destroy(el);
        return NULL;
    }
    priv->sinkpad->push = nv_video_encoder_sink_push;

    zst_element_add_pad(el, priv->sinkpad);
    zst_element_add_pad(el, priv->srcpad);

    return el;
}

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "nvenc") == 0) {
        return zst_nv_video_encoder_create();
    }
    return NULL;
}

static const zst_property_spec_t g_nvenc_properties[] = {
    { "codec", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "h264", "h264 or h265" },
    { "preset", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "fast", "ultrafast, fast, medium, slow" },
    { "bitrate", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "4000000", "Target bitrate in bits/sec" },
    { "gop-size", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "30", "GOP interval" },
    { "profile", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "main", "main, baseline, high" }
};

static const zst_pad_template_t g_nvenc_pads[] = {
    { "sink", ZST_PAD_SINK, "video/x-raw" },
    { "src", ZST_PAD_SRC, "video/x-h264" },
    { "src", ZST_PAD_SRC, "video/x-h265" }
};

static const zst_element_desc_t g_nvenc_elements[] = {
    {
        .name = "nvenc",
        .long_name = "NVIDIA V4L2 Video Encoder",
        .category = "Codec/Encoder",
        .description = "Hardware H.264/H.265 video encoder using NV V4L2 extensions",
        .author = "zstreamer",
        .properties = g_nvenc_properties,
        .nb_properties = sizeof(g_nvenc_properties) / sizeof(g_nvenc_properties[0]),
        .pads = g_nvenc_pads,
        .nb_pads = sizeof(g_nvenc_pads) / sizeof(g_nvenc_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "nvenc_plugin",
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
        *nb_elements_out = sizeof(g_nvenc_elements) / sizeof(g_nvenc_elements[0]);
    }
    return g_nvenc_elements;
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
