/*=============================================================================
    v4l2_sink.c — V4L2 video output sink
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#include "zst_element.h"
#include "zst_log.h"
#include "zst_buffer.h"
#include "zst_allocator.h"

typedef struct {
    int fd;
    int is_mock;
    uint32_t width;
    uint32_t height;

    char device[128];
    char pixel_format[32];
    char memory_type[32]; // "mmap", "userptr", "dmabuf"

    struct {
        void* start;
        size_t length;
    } *buffers;
    uint32_t nb_buffers;

    uint32_t queued_count;
    int started;
} v4l2_sink_t;

static zst_result_t
v4l2_sink_open(zst_element_t* el)
{
    v4l2_sink_t* s = el->priv;

    if (s->width == 0)  s->width = 640;
    if (s->height == 0) s->height = 480;
    if (s->memory_type[0] == '\0') strcpy(s->memory_type, "mmap");

    const char* dev_path = s->device[0] ? s->device : "/dev/video1";
    s->fd = open(dev_path, O_RDWR | O_NONBLOCK);
    if (s->fd < 0) {
        ZST_LOG_WARN("v4l2sink", "Failed to open %s. Falling back to synthetic sink.", dev_path);
        s->is_mock = 1;
        return ZST_OK;
    }

    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt.fmt.pix.width = s->width;
    fmt.fmt.pix.height = s->height;
    // Default to YUYV or whatever pixel_format specifies. For now YUYV is standard dummy
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;

    if (strcmp(s->pixel_format, "YUV420P") == 0 || strcmp(s->pixel_format, "I420") == 0) {
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
    }

    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (ioctl(s->fd, VIDIOC_S_FMT, &fmt) < 0) {
        ZST_LOG_WARN("v4l2sink", "VIDIOC_S_FMT failed. Falling back to synthetic sink.");
        close(s->fd);
        s->fd = -1;
        s->is_mock = 1;
        return ZST_OK;
    }

    s->is_mock = 0;
    return ZST_OK;
}

static zst_result_t
v4l2_sink_start(zst_element_t* el)
{
    v4l2_sink_t* s = el->priv;

    if (s->is_mock || s->fd < 0) {
        s->started = 1;
        return ZST_OK;
    }

    struct v4l2_requestbuffers req = {0};
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

    if (strcmp(s->memory_type, "userptr") == 0) {
        req.memory = V4L2_MEMORY_USERPTR;
    } else if (strcmp(s->memory_type, "dmabuf") == 0) {
        req.memory = V4L2_MEMORY_DMABUF;
    } else {
        req.memory = V4L2_MEMORY_MMAP;
    }

    if (ioctl(s->fd, VIDIOC_REQBUFS, &req) < 0) {
        ZST_LOG_ERROR("v4l2sink", "VIDIOC_REQBUFS failed.");
        return ZST_ERROR;
    }

    s->buffers = calloc(req.count, sizeof(*s->buffers));
    s->nb_buffers = req.count;
    s->queued_count = 0;

    if (req.memory == V4L2_MEMORY_MMAP) {
        for (uint32_t i = 0; i < req.count; i++) {
            struct v4l2_buffer buf = {0};
            buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            if (ioctl(s->fd, VIDIOC_QUERYBUF, &buf) < 0) {
                ZST_LOG_ERROR("v4l2sink", "VIDIOC_QUERYBUF failed.");
                return ZST_ERROR;
            }
            s->buffers[i].length = buf.length;
            s->buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd, buf.m.offset);
            if (s->buffers[i].start == MAP_FAILED) {
                ZST_LOG_ERROR("v4l2sink", "mmap failed.");
                return ZST_ERROR;
            }
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (ioctl(s->fd, VIDIOC_STREAMON, &type) < 0) {
        ZST_LOG_ERROR("v4l2sink", "VIDIOC_STREAMON failed.");
        return ZST_ERROR;
    }

    s->started = 1;
    return ZST_OK;
}

static zst_result_t
v4l2_sink_stop(zst_element_t* el)
{
    v4l2_sink_t* s = el->priv;
    if (!s->is_mock && s->fd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        ioctl(s->fd, VIDIOC_STREAMOFF, &type);
    }
    s->started = 0;
    return ZST_OK;
}

static zst_result_t
v4l2_sink_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    (void)out;
    v4l2_sink_t* s = el->priv;

    if (!in || !s->started) {
        return ZST_ERROR;
    }

    if (s->is_mock || s->fd < 0) {
        /* Mock mode: just drop the buffer to simulate a fast sink */
        return ZST_OK;
    }

    struct v4l2_buffer vbuf = {0};
    vbuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

    if (s->queued_count >= s->nb_buffers) {
        /* Need to dequeue a buffer before we can queue a new one */
        if (strcmp(s->memory_type, "userptr") == 0) {
            vbuf.memory = V4L2_MEMORY_USERPTR;
        } else if (strcmp(s->memory_type, "dmabuf") == 0) {
            vbuf.memory = V4L2_MEMORY_DMABUF;
        } else {
            vbuf.memory = V4L2_MEMORY_MMAP;
        }

        if (ioctl(s->fd, VIDIOC_DQBUF, &vbuf) < 0) {
            if (errno == EAGAIN) {
                struct pollfd pfd = { .fd = s->fd, .events = POLLOUT };
                int res = poll(&pfd, 1, 200);
                if (res > 0 && (pfd.revents & POLLOUT)) {
                    if (ioctl(s->fd, VIDIOC_DQBUF, &vbuf) < 0) {
                        return ZST_ERROR;
                    }
                } else {
                    return ZST_ERROR; /* timeout or err */
                }
            } else {
                return ZST_ERROR;
            }
        }
        s->queued_count--;
    } else {
        /* We still have available buffers that have never been queued.
           Use the next available index. */
        vbuf.index = s->queued_count;
        if (strcmp(s->memory_type, "userptr") == 0) {
            vbuf.memory = V4L2_MEMORY_USERPTR;
        } else if (strcmp(s->memory_type, "dmabuf") == 0) {
            vbuf.memory = V4L2_MEMORY_DMABUF;
        } else {
            vbuf.memory = V4L2_MEMORY_MMAP;
        }
    }

    if (strcmp(s->memory_type, "userptr") == 0) {
        vbuf.m.userptr = (unsigned long)in->memory.data;
        vbuf.length = in->memory.size;
        vbuf.bytesused = in->memory.size;
    } else if (strcmp(s->memory_type, "dmabuf") == 0) {
        if (in->memory.type == ZST_MEMORY_DMABUF && in->memory.priv) {
            /* If the buffer was allocated by our dmabuf allocator, priv points to the dmabuf fd internally.
               We could extract it using zst_allocator_dmabuf_get_fd if we had access to the allocator.
               Assuming `in->memory.priv` stores the fd for the dmabuf allocator. */
            vbuf.m.fd = *(int*)in->memory.priv;
            vbuf.length = in->memory.size;
            vbuf.bytesused = in->memory.size;
        } else {
            return ZST_ERROR;
        }
    } else {
        /* MMAP */
        size_t to_copy = in->memory.size < s->buffers[vbuf.index].length ? in->memory.size : s->buffers[vbuf.index].length;
        memcpy(s->buffers[vbuf.index].start, in->memory.data, to_copy);
        vbuf.bytesused = to_copy;
    }

    if (ioctl(s->fd, VIDIOC_QBUF, &vbuf) < 0) {
        ZST_LOG_ERROR("v4l2sink", "VIDIOC_QBUF failed.");
        return ZST_ERROR;
    }

    s->queued_count++;

    return ZST_OK;
}

static zst_result_t
v4l2_sink_close(zst_element_t* el)
{
    v4l2_sink_t* s = el->priv;
    if (!s->is_mock && s->fd >= 0) {
        if (s->buffers) {
            if (strcmp(s->memory_type, "mmap") == 0) {
                for (uint32_t i = 0; i < s->nb_buffers; i++) {
                    if (s->buffers[i].start && s->buffers[i].start != MAP_FAILED) {
                        munmap(s->buffers[i].start, s->buffers[i].length);
                    }
                }
            }
            free(s->buffers);
            s->buffers = NULL;
        }
        close(s->fd);
        s->fd = -1;
    }
    return ZST_OK;
}

static zst_result_t
v4l2_sink_set_property(zst_element_t* el, const char* name, const char* value)
{
    v4l2_sink_t* s = el->priv;
    if (!name || !value) return ZST_ERROR;

    if (strcmp(name, "device") == 0) {
        snprintf(s->device, sizeof(s->device), "%s", value);
        return ZST_OK;
    } else if (strcmp(name, "width") == 0) {
        s->width = atoi(value);
        return ZST_OK;
    } else if (strcmp(name, "height") == 0) {
        s->height = atoi(value);
        return ZST_OK;
    } else if (strcmp(name, "pixel-format") == 0) {
        snprintf(s->pixel_format, sizeof(s->pixel_format), "%s", value);
        return ZST_OK;
    } else if (strcmp(name, "memory-type") == 0) {
        snprintf(s->memory_type, sizeof(s->memory_type), "%s", value);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t
v4l2_sink_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    v4l2_sink_t* s = el->priv;
    if (!name || !value_out || max_len == 0) return ZST_ERROR;

    if (strcmp(name, "device") == 0) {
        snprintf(value_out, max_len, "%s", s->device);
    } else if (strcmp(name, "width") == 0) {
        snprintf(value_out, max_len, "%u", s->width);
    } else if (strcmp(name, "height") == 0) {
        snprintf(value_out, max_len, "%u", s->height);
    } else if (strcmp(name, "pixel-format") == 0) {
        snprintf(value_out, max_len, "%s", s->pixel_format);
    } else if (strcmp(name, "memory-type") == 0) {
        snprintf(value_out, max_len, "%s", s->memory_type);
    } else {
        return ZST_ERROR;
    }
    return ZST_OK;
}

static zst_element_ops_t g_ops = {
    .name    = "v4l2sink",
    .open    = v4l2_sink_open,
    .close   = v4l2_sink_close,
    .start   = v4l2_sink_start,
    .stop    = v4l2_sink_stop,
    .process = v4l2_sink_process,
    .set_property = v4l2_sink_set_property,
    .get_property = v4l2_sink_get_property
};

zst_element_t*
zst_v4l2_sink_create(void)
{
    zst_element_t* el;
    v4l2_sink_t* priv;
    zst_pad_t* sink;

    priv = calloc(1, sizeof(*priv));
    priv->fd = -1;

    el = zst_element_create(&g_ops, priv);
    sink = zst_pad_create("sink", ZST_PAD_SINK);
    zst_element_add_pad(el, sink);

    return el;
}

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "v4l2sink") == 0) {
        return zst_v4l2_sink_create();
    }
    return NULL;
}

static const zst_pad_template_t g_v4l2sink_pads[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-raw" }
};

static const zst_element_desc_t g_v4l2sink_elements[] = {
    {
        .name = "v4l2sink",
        .long_name = "V4L2 Sink",
        .category = "Sink/Video",
        .description = "Outputs video to a V4L2 device",
        .author = "zstreamer",
        .properties = NULL,
        .nb_properties = 0,
        .pads = g_v4l2sink_pads,
        .nb_pads = sizeof(g_v4l2sink_pads) / sizeof(g_v4l2sink_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "v4l2sink_plugin",
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
        *nb_elements_out = sizeof(g_v4l2sink_elements) / sizeof(g_v4l2sink_elements[0]);
    }
    return g_v4l2sink_elements;
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