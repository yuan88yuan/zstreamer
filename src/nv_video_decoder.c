/*=============================================================================
    nv_video_decoder.c — NVIDIA V4L2 video decoder implementation
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

#ifndef V4L2_CID_MPEG_BASE
#define V4L2_CID_MPEG_BASE 0x00990900
#endif

#ifndef V4L2_PIX_FMT_H265
#define V4L2_PIX_FMT_H265 v4l2_fourcc('H', '2', '6', '5')
#endif

#ifndef V4L2_PIX_FMT_NV12M
#define V4L2_PIX_FMT_NV12M v4l2_fourcc('N', 'M', '1', '2')
#endif

/* NVIDIA-Specific controls from docs */
#define V4L2_CID_MPEG_VIDEO_DISABLE_COMPLETE_FRAME_INPUT (V4L2_CID_MPEG_BASE+515)
#define V4L2_CID_MPEG_VIDEO_DISABLE_DPB                  (V4L2_CID_MPEG_BASE+516)
#define V4L2_CID_MPEG_VIDEO_ERROR_REPORTING              (V4L2_CID_MPEG_BASE+517)
#define V4L2_CID_MPEG_VIDEO_SKIP_FRAMES                  (V4L2_CID_MPEG_BASE+518)

#ifndef V4L2_EVENT_RESOLUTION_CHANGE
#define V4L2_EVENT_RESOLUTION_CHANGE 5
#endif

typedef struct {
    int             fd;
    int             initialized;
    zst_buffer_pool_t* pool;
    uint32_t        width;
    uint32_t        height;
    zst_pad_t*      sinkpad;
    zst_pad_t*      srcpad;

    char            codec[32];      /* "h264" or "h265" */
    int             skip_frames;
    int             error_reporting;

    struct v4l2_buffer* output_buffers;
    struct v4l2_buffer* capture_buffers;
    uint32_t        nb_output_buffers;
    uint32_t        nb_capture_buffers;

    int             resolution_changed;
} nv_video_decoder_t;

static zst_result_t
nv_video_decoder_open(zst_element_t* el)
{
    nv_video_decoder_t* s = el->priv;
    s->fd = open("/dev/nvhost-nvdec", O_RDWR | O_NONBLOCK);
    if (s->fd < 0) {
        ZST_LOG_ERROR("nvdec", "Failed to open /dev/nvhost-nvdec (ensure you are on Jetson)");
        return ZST_ERROR;
    }

    struct v4l2_event_subscription sub = {0};
    sub.type = V4L2_EVENT_RESOLUTION_CHANGE;
    if (ioctl(s->fd, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0) {
        ZST_LOG_ERROR("nvdec", "Failed to subscribe to resolution change event");
    }

    s->initialized = 0;
    s->resolution_changed = 0;
    s->pool = NULL;
    return ZST_OK;
}

static void
nv_video_decoder_cleanup_buffers(nv_video_decoder_t* s)
{
    if (s->output_buffers) {
        for (uint32_t i = 0; i < s->nb_output_buffers; i++) {
            if (s->output_buffers[i].m.planes) {
                if (s->output_buffers[i].m.planes[0].m.userptr && s->output_buffers[i].m.planes[0].m.userptr != (unsigned long)MAP_FAILED) {
                    munmap((void*)s->output_buffers[i].m.planes[0].m.userptr, s->output_buffers[i].m.planes[0].length);
                }
                free(s->output_buffers[i].m.planes);
            }
        }
        free(s->output_buffers);
        s->output_buffers = NULL;
    }
    if (s->capture_buffers) {
        for (uint32_t i = 0; i < s->nb_capture_buffers; i++) {
            if (s->capture_buffers[i].m.planes) {
                for(int j = 0; j < s->capture_buffers[i].length; j++) {
                    if (s->capture_buffers[i].m.planes[j].m.userptr && s->capture_buffers[i].m.planes[j].m.userptr != (unsigned long)MAP_FAILED) {
                        munmap((void*)s->capture_buffers[i].m.planes[j].m.userptr, s->capture_buffers[i].m.planes[j].length);
                    }
                }
                free(s->capture_buffers[i].m.planes);
            }
        }
        free(s->capture_buffers);
        s->capture_buffers = NULL;
    }
}

static zst_result_t
nv_video_decoder_close(zst_element_t* el)
{
    nv_video_decoder_t* s = el->priv;
    if (s->fd >= 0) {
        nv_video_decoder_cleanup_buffers(s);
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
nv_video_decoder_start(zst_element_t* el)
{
    nv_video_decoder_t* s = el->priv;
    if (s->fd < 0) return ZST_ERROR;
    return ZST_OK;
}

static zst_result_t
nv_video_decoder_stop(zst_element_t* el)
{
    nv_video_decoder_t* s = el->priv;
    if (s->fd >= 0 && s->initialized) {
        enum v4l2_buf_type type;
        type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        ioctl(s->fd, VIDIOC_STREAMOFF, &type);
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        ioctl(s->fd, VIDIOC_STREAMOFF, &type);
    }
    return ZST_OK;
}

static zst_result_t
nv_video_decoder_init_v4l2(nv_video_decoder_t* s, uint32_t width, uint32_t height)
{
    s->width = width;
    s->height = height;

    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    fmt.fmt.pix_mp.width = width;
    fmt.fmt.pix_mp.height = height;
    fmt.fmt.pix_mp.pixelformat = (strcmp(s->codec, "h265") == 0) ? V4L2_PIX_FMT_H265 : V4L2_PIX_FMT_H264;
    fmt.fmt.pix_mp.num_planes = 1;
    fmt.fmt.pix_mp.plane_fmt[0].sizeimage = 2 * 1024 * 1024; /* generous bitstream buffer */

    if (ioctl(s->fd, VIDIOC_S_FMT, &fmt) < 0) {
        return ZST_ERROR;
    }

    struct v4l2_control ctl = {0};
    if (s->error_reporting) {
        ctl.id = V4L2_CID_MPEG_VIDEO_ERROR_REPORTING;
        ctl.value = 1;
        ioctl(s->fd, VIDIOC_S_CTRL, &ctl);
    }

    if (s->skip_frames) {
        ctl.id = V4L2_CID_MPEG_VIDEO_SKIP_FRAMES;
        ctl.value = s->skip_frames;
        ioctl(s->fd, VIDIOC_S_CTRL, &ctl);
    }

    struct v4l2_requestbuffers req = {0};
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(s->fd, VIDIOC_REQBUFS, &req) < 0) {
        return ZST_ERROR;
    }
    s->nb_output_buffers = req.count;
    s->output_buffers = calloc(req.count, sizeof(struct v4l2_buffer));

    for (uint32_t i = 0; i < req.count; i++) {
        struct v4l2_plane* planes = calloc(1, sizeof(struct v4l2_plane));
        s->output_buffers[i].type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        s->output_buffers[i].memory = V4L2_MEMORY_MMAP;
        s->output_buffers[i].index = i;
        s->output_buffers[i].length = 1;
        s->output_buffers[i].m.planes = planes;

        if (ioctl(s->fd, VIDIOC_QUERYBUF, &s->output_buffers[i]) < 0) {
            return ZST_ERROR;
        }

        s->output_buffers[i].m.planes[0].m.userptr = (unsigned long)mmap(NULL, planes[0].length, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd, planes[0].m.mem_offset);
        s->output_buffers[i].m.planes[0].length = planes[0].length;
    }

    /* Stream on output */
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (ioctl(s->fd, VIDIOC_STREAMON, &type) < 0) return ZST_ERROR;

    s->initialized = 1;
    return ZST_OK;
}

static zst_result_t
nv_video_decoder_handle_resolution_change(nv_video_decoder_t* s)
{
    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(s->fd, VIDIOC_G_FMT, &fmt) < 0) {
        return ZST_ERROR;
    }

    s->width = fmt.fmt.pix_mp.width;
    s->height = fmt.fmt.pix_mp.height;

    struct v4l2_requestbuffers req = {0};
    req.count = 8;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(s->fd, VIDIOC_REQBUFS, &req) < 0) {
        return ZST_ERROR;
    }
    s->nb_capture_buffers = req.count;
    s->capture_buffers = calloc(req.count, sizeof(struct v4l2_buffer));

    for (uint32_t i = 0; i < req.count; i++) {
        struct v4l2_plane* planes = calloc(3, sizeof(struct v4l2_plane));
        s->capture_buffers[i].type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        s->capture_buffers[i].memory = V4L2_MEMORY_MMAP;
        s->capture_buffers[i].index = i;
        s->capture_buffers[i].length = fmt.fmt.pix_mp.num_planes;
        s->capture_buffers[i].m.planes = planes;

        if (ioctl(s->fd, VIDIOC_QUERYBUF, &s->capture_buffers[i]) < 0) {
            return ZST_ERROR;
        }

        for (uint32_t j = 0; j < s->capture_buffers[i].length; j++) {
            s->capture_buffers[i].m.planes[j].m.userptr = (unsigned long)mmap(NULL, planes[j].length, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd, planes[j].m.mem_offset);
            s->capture_buffers[i].m.planes[j].length = planes[j].length;
        }

        /* Queue the capture buffers immediately */
        ioctl(s->fd, VIDIOC_QBUF, &s->capture_buffers[i]);
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(s->fd, VIDIOC_STREAMON, &type) < 0) return ZST_ERROR;

    if (!s->pool) {
        zst_buffer_pool_config_t pool_cfg = {
            .min_buffers = 2,
            .max_buffers = 16,
            .buffer_size = s->width * s->height * 3 / 2, /* NV12 size */
            .buffer_type = ZST_BUFFER_VIDEO_FRAME
        };
        s->pool = zst_buffer_pool_create(NULL, &pool_cfg);
    }

    return ZST_OK;
}

static zst_result_t
nv_video_decoder_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    nv_video_decoder_t* s = el->priv;
    if (out) *out = NULL;
    if (!in || s->fd < 0) return ZST_ERROR;

    if (!s->initialized) {
        if (nv_video_decoder_init_v4l2(s, 1920, 1080) != ZST_OK) { // Default resolution until dynamic resize
            return ZST_ERROR;
        }
    }

    /* Handle pending V4L2 events (like resolution change) */
    struct pollfd pfd_event = { .fd = s->fd, .events = POLLPRI };
    if (poll(&pfd_event, 1, 0) > 0) {
        struct v4l2_event ev = {0};
        while (ioctl(s->fd, VIDIOC_DQEVENT, &ev) == 0) {
            if (ev.type == V4L2_EVENT_RESOLUTION_CHANGE) {
                nv_video_decoder_handle_resolution_change(s);
            }
        }
    }

    /* Queue input packet buffer */
    struct v4l2_buffer vbuf_out = {0};
    struct v4l2_plane planes_out[1] = {0};
    vbuf_out.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    vbuf_out.memory = V4L2_MEMORY_MMAP;
    vbuf_out.length = 1;
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
            static int out_idx = 0;
            vbuf_out.index = out_idx;
            out_idx = (out_idx + 1) % s->nb_output_buffers;
        }

        size_t bytes = in->memory.size;
        if (bytes > s->output_buffers[vbuf_out.index].m.planes[0].length) {
            bytes = s->output_buffers[vbuf_out.index].m.planes[0].length;
        }
        memcpy((void*)s->output_buffers[vbuf_out.index].m.planes[0].m.userptr, in->memory.data, bytes);
        planes_out[0].bytesused = bytes;
        if (in->flags & ZST_BUFFER_FLAG_EOS) {
            planes_out[0].bytesused = 0; // EOS trigger for decoder
        }

        ioctl(s->fd, VIDIOC_QBUF, &vbuf_out);
    }

    /* Capture output frame buffer */
    if (s->capture_buffers) {
        struct v4l2_buffer vbuf_cap = {0};
        struct v4l2_plane planes_cap[3] = {0}; // decoder capture could be multipaned
        vbuf_cap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        vbuf_cap.memory = V4L2_MEMORY_MMAP;
        vbuf_cap.length = s->capture_buffers[0].length; // usually 1 or 2 for NV12M
        vbuf_cap.m.planes = planes_cap;

        struct pollfd pfd_cap = { .fd = s->fd, .events = POLLIN };
        if (poll(&pfd_cap, 1, 100) > 0) {
            if (ioctl(s->fd, VIDIOC_DQBUF, &vbuf_cap) == 0) {
                zst_buffer_t* frame_buf = NULL;
                if (s->pool && zst_buffer_pool_acquire(s->pool, &frame_buf, 0, 0) == ZST_OK) {

                    zst_video_frame_t* v_frame = frame_buf->payload;
                    if (!v_frame) {
                        v_frame = calloc(1, sizeof(zst_video_frame_t));
                        frame_buf->payload = v_frame;
                    }

                    v_frame->width = s->width;
                    v_frame->height = s->height;
                    v_frame->format = 1; // Assuming NV12 format

                    size_t copied = 0;
                    for(uint32_t j=0; j < vbuf_cap.length; j++) {
                        size_t bytes = planes_cap[j].bytesused;
                        if (bytes == 0) continue;
                        memcpy(frame_buf->memory.data + copied, (void*)s->capture_buffers[vbuf_cap.index].m.planes[j].m.userptr, bytes);
                        v_frame->plane[j] = frame_buf->memory.data + copied;

                        // Crude stride setup
                        if (j == 0) v_frame->stride[j] = s->width;
                        else if (j == 1) v_frame->stride[j] = s->width;

                        copied += bytes;
                    }
                    frame_buf->memory.size = copied;

                    frame_buf->pts = (zst_time_t)vbuf_cap.timestamp.tv_sec * 1000000000ULL + vbuf_cap.timestamp.tv_usec * 1000ULL;
                    frame_buf->dts = frame_buf->pts;
                    frame_buf->duration = in->duration;

                    if (planes_cap[0].bytesused == 0) {
                        frame_buf->flags |= ZST_BUFFER_FLAG_EOS;
                    }

                    *out = frame_buf;
                }
                ioctl(s->fd, VIDIOC_QBUF, &vbuf_cap);
            }
        }
    }

    return ZST_OK;
}

static zst_caps_t*
nv_video_decoder_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)filter;
    nv_video_decoder_t* s = el->priv;
    zst_caps_t* caps = zst_caps_create();
    if (!caps) return NULL;

    if (pad == s->sinkpad) {
        if (strcmp(s->codec, "h265") == 0) {
            zst_caps_append(caps, zst_caps_struct_create_video("video/x-h265", 0, 0, 0.0, ""));
        } else {
            zst_caps_append(caps, zst_caps_struct_create_video("video/x-h264", 0, 0, 0.0, ""));
        }
    } else if (pad == s->srcpad) {
        zst_caps_append(caps, zst_caps_struct_create_video("video/x-raw", (int)s->width, (int)s->height, 0.0, "NV12M"));
    }
    return caps;
}

static zst_result_t
nv_video_decoder_set_property(zst_element_t* el, const char* name, const char* value)
{
    nv_video_decoder_t* s = el->priv;
    if (!name || !value) return ZST_ERROR;

    if (strcmp(name, "codec") == 0) {
        snprintf(s->codec, sizeof(s->codec), "%s", value);
        return ZST_OK;
    } else if (strcmp(name, "skip-frames") == 0) {
        s->skip_frames = atoi(value);
        return ZST_OK;
    } else if (strcmp(name, "error-reporting") == 0) {
        s->error_reporting = atoi(value);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t
nv_video_decoder_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    nv_video_decoder_t* s = el->priv;
    if (!name || !value_out || max_len == 0) return ZST_ERROR;

    if (strcmp(name, "codec") == 0) {
        snprintf(value_out, max_len, "%s", s->codec);
    } else if (strcmp(name, "skip-frames") == 0) {
        snprintf(value_out, max_len, "%d", s->skip_frames);
    } else if (strcmp(name, "error-reporting") == 0) {
        snprintf(value_out, max_len, "%d", s->error_reporting);
    } else {
        return ZST_ERROR;
    }
    return ZST_OK;
}

static zst_buffer_pool_t*
nv_video_decoder_get_pool(zst_element_t* el)
{
    nv_video_decoder_t* s = el->priv;
    return s->pool;
}

static zst_result_t
nv_video_decoder_sink_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    if (!pad || !pad->parent || !buf) return ZST_ERROR;
    zst_buffer_t* out = NULL;
    zst_result_t ret = nv_video_decoder_process(pad->parent, buf, &out);

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

static zst_element_ops_t g_nv_video_decoder_ops = {
    .name    = "nvdec",
    .open    = nv_video_decoder_open,
    .close   = nv_video_decoder_close,
    .start   = nv_video_decoder_start,
    .stop    = nv_video_decoder_stop,
    .process = nv_video_decoder_process,
    .get_caps = nv_video_decoder_get_caps,
    .set_property = nv_video_decoder_set_property,
    .get_property = nv_video_decoder_get_property,
    .get_pool = nv_video_decoder_get_pool
};

zst_element_t*
zst_nv_video_decoder_create(void)
{
    nv_video_decoder_t* priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;

    snprintf(priv->codec, sizeof(priv->codec), "h264");
    priv->skip_frames = 0;
    priv->error_reporting = 0;

    zst_element_t* el = zst_element_create(&g_nv_video_decoder_ops, priv);
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
    priv->sinkpad->push = nv_video_decoder_sink_push;

    zst_element_add_pad(el, priv->sinkpad);
    zst_element_add_pad(el, priv->srcpad);

    return el;
}

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "nvdec") == 0) {
        return zst_nv_video_decoder_create();
    }
    return NULL;
}

static const zst_property_spec_t g_nvdec_properties[] = {
    { "codec", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "h264", "h264 or h265" },
    { "skip-frames", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "0", "Skip frames (0: none, 1: nonref, 2: decode IDR only)" },
    { "error-reporting", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "0", "Enable error reporting" }
};

static const zst_pad_template_t g_nvdec_pads[] = {
    { "sink", ZST_PAD_SINK, "video/x-h264" },
    { "sink", ZST_PAD_SINK, "video/x-h265" },
    { "src", ZST_PAD_SRC, "video/x-raw" }
};

static const zst_element_desc_t g_nvdec_elements[] = {
    {
        .name = "nvdec",
        .long_name = "NVIDIA V4L2 Video Decoder",
        .category = "Codec/Decoder",
        .description = "Hardware H.264/H.265 video decoder using NV V4L2 extensions",
        .author = "zstreamer",
        .properties = g_nvdec_properties,
        .nb_properties = sizeof(g_nvdec_properties) / sizeof(g_nvdec_properties[0]),
        .pads = g_nvdec_pads,
        .nb_pads = sizeof(g_nvdec_pads) / sizeof(g_nvdec_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "nvdec_plugin",
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
        *nb_elements_out = sizeof(g_nvdec_elements) / sizeof(g_nvdec_elements[0]);
    }
    return g_nvdec_elements;
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
