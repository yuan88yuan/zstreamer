/*=============================================================================
    v4l2_source.c — V4L2 camera capture with mock synthetic fallback
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#include "zst_element.h"
#include "zst_log.h"
#include "zst_buffer.h"
#include "zst_buffer_pool.h"
#include "zst_clock.h"

static void v4l2_buf_free(zst_buffer_t* buf);

typedef struct v4l2_source v4l2_source_t;

typedef struct {
    /* Keep fd as the first member so existing downstream code that treats
     * memory.priv as int* (for ZST_MEMORY_DMABUF) remains compatible. */
    int fd;
    v4l2_source_t* source;
    uint32_t index;
} v4l2_export_buffer_ctx_t;

typedef struct v4l2_source {
    int fd;
    int is_mock;
    uint32_t width;
    uint32_t height;
    uint64_t frame_count;
    
    struct {
        void* start;
        size_t length;
    } *buffers;
    uint32_t nb_buffers;

    zst_buffer_pool_t* pool;

    char            device[128];
    char            pixel_format[32];
    char            memory_type[32]; // "mmap", "userptr", "dmabuf", "mmap-export"
    zst_buffer_t**  pool_buffers;
    int*            exported_fds;
    int             streaming;
} v4l2_source_t;

static int
v4l2_memory_type_is_valid(const char* memory_type)
{
    return strcmp(memory_type, "mmap") == 0 ||
           strcmp(memory_type, "userptr") == 0 ||
           strcmp(memory_type, "dmabuf") == 0 ||
           strcmp(memory_type, "mmap-export") == 0;
}

static int
v4l2_source_is_mmap_export(const v4l2_source_t* s)
{
    return s && strcmp(s->memory_type, "mmap-export") == 0;
}

static void
v4l2_source_close_exported_fds(v4l2_source_t* s)
{
    if (!s || !s->exported_fds) return;

    for (uint32_t i = 0; i < s->nb_buffers; i++) {
        if (s->exported_fds[i] >= 0) {
            close(s->exported_fds[i]);
            s->exported_fds[i] = -1;
        }
    }
    free(s->exported_fds);
    s->exported_fds = NULL;
}

static zst_result_t
v4l2_source_requeue_mmap_buffer(v4l2_source_t* s, uint32_t index)
{
    if (!s || s->fd < 0) return ZST_ERROR;

    struct v4l2_buffer qbuf = {0};
    qbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    qbuf.memory = V4L2_MEMORY_MMAP;
    qbuf.index = index;

    if (ioctl(s->fd, VIDIOC_QBUF, &qbuf) < 0) {
        ZST_LOG_WARN("v4l2src", "VIDIOC_QBUF failed while recycling exported buffer %u.", index);
        return ZST_ERROR;
    }
    return ZST_OK;
}

static void
v4l2src_dmabuf_release_callback(void* priv)
{
    v4l2_export_buffer_ctx_t* ctx = priv;
    if (!ctx) return;

    v4l2_source_t* s = ctx->source;
    if (s && s->streaming && s->fd >= 0) {
        (void)v4l2_source_requeue_mmap_buffer(s, ctx->index);
    }

    if (ctx->fd >= 0) {
        close(ctx->fd);
        ctx->fd = -1;
    }
    free(ctx);
}

static void
yuyv_to_yuv420p(uint32_t width, uint32_t height, const uint8_t* yuyv, uint8_t* yuv420p)
{
    uint8_t* y = yuv420p;
    uint8_t* u = yuv420p + width * height;
    uint8_t* v = yuv420p + width * height + (width * height) / 4;

    for (uint32_t i = 0; i < height; i++) {
        for (uint32_t j = 0; j < width; j += 2) {
            uint32_t yuyv_idx = (i * width + j) * 2;
            y[i * width + j] = yuyv[yuyv_idx];
            y[i * width + j + 1] = yuyv[yuyv_idx + 2];
            
            if (i % 2 == 0) {
                u[(i / 2) * (width / 2) + j / 2] = yuyv[yuyv_idx + 1];
                v[(i / 2) * (width / 2) + j / 2] = yuyv[yuyv_idx + 3];
            }
        }
    }
}

#include "zst_allocator.h"

static zst_result_t
v4l2_open(zst_element_t* el)
{
    v4l2_source_t* s = el->priv;

    if (s->width == 0)  s->width = 640;
    if (s->height == 0) s->height = 480;
    if (s->memory_type[0] == '\0') strcpy(s->memory_type, "mmap");
    s->frame_count = 0;

    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = s->width;
    fmt.fmt.pix.height = s->height;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (strcmp(s->pixel_format, "YUV420P") == 0 || strcmp(s->pixel_format, "I420") == 0) {
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
    } else {
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    }

    size_t frame_size = (fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_YUV420) ?
                        (s->width * s->height * 3 / 2) :
                        (s->width * s->height * 2);

    zst_buffer_pool_config_t pool_cfg = {
        .min_buffers = 4,
        .max_buffers = 8,
        .buffer_size = frame_size,
        .buffer_type = ZST_BUFFER_VIDEO_FRAME
    };

    zst_allocator_t* alloc = NULL;
    if (strcmp(s->memory_type, "dmabuf") == 0) {
        alloc = zst_allocator_dmabuf_create();
    }
    s->pool = zst_buffer_pool_create(alloc, &pool_cfg);

    const char* dev_path = s->device[0] ? s->device : "/dev/video0";
    s->fd = open(dev_path, O_RDWR | O_NONBLOCK);
    if (s->fd < 0) {
        ZST_LOG_WARN("v4l2src", "Failed to open %s. Falling back to synthetic source.", dev_path);
        s->is_mock = 1;
        return ZST_OK;
    }

    if (ioctl(s->fd, VIDIOC_S_FMT, &fmt) < 0) {
        ZST_LOG_WARN("v4l2src", "VIDIOC_S_FMT failed. Falling back to synthetic source.");
        close(s->fd);
        s->fd = -1;
        s->is_mock = 1;
        return ZST_OK;
    }

    /* Request buffers */
    struct v4l2_requestbuffers req = {0};
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (strcmp(s->memory_type, "userptr") == 0) {
        req.memory = V4L2_MEMORY_USERPTR;
    } else if (strcmp(s->memory_type, "dmabuf") == 0) {
        req.memory = V4L2_MEMORY_DMABUF;
    } else {
        /* Both "mmap" and "mmap-export" use driver-allocated MMAP buffers;
         * mmap-export additionally exports each slot with VIDIOC_EXPBUF. */
        req.memory = V4L2_MEMORY_MMAP;
    }

    if (ioctl(s->fd, VIDIOC_REQBUFS, &req) < 0) {
        ZST_LOG_WARN("v4l2src", "VIDIOC_REQBUFS failed. Falling back to synthetic source.");
        close(s->fd);
        s->fd = -1;
        s->is_mock = 1;
        return ZST_OK;
    }

    s->nb_buffers = req.count;

    if (req.memory == V4L2_MEMORY_MMAP) {
        s->buffers = calloc(req.count, sizeof(*s->buffers));
        if (!s->buffers) goto error;

        if (v4l2_source_is_mmap_export(s)) {
            s->exported_fds = calloc(req.count, sizeof(*s->exported_fds));
            if (!s->exported_fds) goto error;
            for (uint32_t i = 0; i < req.count; i++) {
                s->exported_fds[i] = -1;
            }
        }

        for (uint32_t i = 0; i < req.count; i++) {
            struct v4l2_buffer buf = {0};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            if (ioctl(s->fd, VIDIOC_QUERYBUF, &buf) < 0) {
                ZST_LOG_ERROR("v4l2src", "VIDIOC_QUERYBUF failed.");
                goto error;
            }
            s->buffers[i].length = buf.length;
            s->buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd, buf.m.offset);
            if (s->buffers[i].start == MAP_FAILED) {
                ZST_LOG_ERROR("v4l2src", "mmap failed.");
                goto error;
            }

            if (v4l2_source_is_mmap_export(s)) {
                struct v4l2_exportbuffer expbuf = {0};
                expbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                expbuf.index = i;
                expbuf.flags = O_RDWR | O_CLOEXEC;

                if (ioctl(s->fd, VIDIOC_EXPBUF, &expbuf) < 0) {
                    ZST_LOG_ERROR("v4l2src", "VIDIOC_EXPBUF failed for buffer %u.", i);
                    goto error;
                }
                s->exported_fds[i] = expbuf.fd;
            }
        }

        /* Queue all buffers */
        for (uint32_t i = 0; i < s->nb_buffers; i++) {
            struct v4l2_buffer buf = {0};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            if (ioctl(s->fd, VIDIOC_QBUF, &buf) < 0) {
                ZST_LOG_ERROR("v4l2src", "VIDIOC_QBUF failed.");
                goto error;
            }
        }
    } else {
        s->pool_buffers = calloc(req.count, sizeof(*s->pool_buffers));
    }

    s->is_mock = 0;
    return ZST_OK;

error:
    v4l2_source_close_exported_fds(s);
    if (s->buffers) {
        for (uint32_t i = 0; i < s->nb_buffers; i++) {
            if (s->buffers[i].start && s->buffers[i].start != MAP_FAILED) {
                munmap(s->buffers[i].start, s->buffers[i].length);
            }
        }
        free(s->buffers);
        s->buffers = NULL;
    }
    if (s->pool_buffers) {
        free(s->pool_buffers);
        s->pool_buffers = NULL;
    }
    close(s->fd);
    s->fd = -1;
    s->is_mock = 1;
    return ZST_OK;
}

static zst_result_t
v4l2_close(zst_element_t* el)
{
    v4l2_source_t* s = el->priv;
    if (!s->is_mock && s->fd >= 0) {
        s->streaming = 0;
        v4l2_source_close_exported_fds(s);
        if (s->buffers) {
            for (uint32_t i = 0; i < s->nb_buffers; i++) {
                munmap(s->buffers[i].start, s->buffers[i].length);
            }
            free(s->buffers);
            s->buffers = NULL;
        }
        if (s->pool_buffers) {
            for (uint32_t i = 0; i < s->nb_buffers; i++) {
                if (s->pool_buffers[i]) {
                    zst_buffer_unref(s->pool_buffers[i]);
                }
            }
            free(s->pool_buffers);
            s->pool_buffers = NULL;
        }
        close(s->fd);
        s->fd = -1;
    } else {
        v4l2_source_close_exported_fds(s);
    }

    if (s->pool) {
        zst_buffer_pool_destroy(s->pool);
        s->pool = NULL;
    }

    return ZST_OK;
}

static zst_result_t
v4l2_start(zst_element_t* el)
{
    v4l2_source_t* s = el->priv;
    if (!s->is_mock && s->fd >= 0) {
        if (strcmp(s->memory_type, "userptr") == 0 || strcmp(s->memory_type, "dmabuf") == 0) {
            for (uint32_t i = 0; i < s->nb_buffers; i++) {
                zst_buffer_t* buf = zst_buffer_create_with_pool(s->pool);
                if (!buf) {
                    ZST_LOG_ERROR("v4l2src", "Failed to get pool buffer for queuing.");
                    return ZST_ERROR;
                }
                s->pool_buffers[i] = buf;

                struct v4l2_buffer vbuf = {0};
                vbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                vbuf.index = i;

                if (strcmp(s->memory_type, "userptr") == 0) {
                    vbuf.memory = V4L2_MEMORY_USERPTR;
                    vbuf.m.userptr = (unsigned long)buf->memory.data;
                    vbuf.length = buf->memory.size;
                } else {
                    vbuf.memory = V4L2_MEMORY_DMABUF;
                    if (buf->memory.type == ZST_MEMORY_DMABUF && buf->memory.priv) {
                        vbuf.m.fd = *(int*)buf->memory.priv;
                        vbuf.length = buf->memory.size;
                    } else {
                        ZST_LOG_ERROR("v4l2src", "Expected dmabuf buffer from pool.");
                        return ZST_ERROR;
                    }
                }

                if (ioctl(s->fd, VIDIOC_QBUF, &vbuf) < 0) {
                    ZST_LOG_ERROR("v4l2src", "VIDIOC_QBUF failed.");
                    return ZST_ERROR;
                }
            }
        }

        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(s->fd, VIDIOC_STREAMON, &type) < 0) {
            ZST_LOG_ERROR("v4l2src", "VIDIOC_STREAMON failed.");
            return ZST_ERROR;
        }
        s->streaming = 1;
    }
    return ZST_OK;
}

static zst_result_t
v4l2_stop(zst_element_t* el)
{
    v4l2_source_t* s = el->priv;
    if (!s->is_mock && s->fd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(s->fd, VIDIOC_STREAMOFF, &type);
        s->streaming = 0;
    }
    return ZST_OK;
}

static zst_result_t
v4l2_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    (void)in;
    v4l2_source_t* s = el->priv;
    zst_buffer_t* buf = NULL;
    int mmap_export = v4l2_source_is_mmap_export(s);

    if (s->is_mock) {
        buf = zst_buffer_create_with_pool(s->pool);
        if (!buf) return ZST_ERROR;

        uint8_t* raw_data = buf->memory.data;
        zst_video_frame_t* frame = buf->payload;
        if (!frame) {
            frame = calloc(1, sizeof(*frame));
            if (!frame) { zst_buffer_unref(buf); return ZST_ERROR; }
            buf->payload = frame;
            buf->destroy = v4l2_buf_free;
        }

        frame->width = s->width;
        frame->height = s->height;
        frame->format = 0; // YUV420P
        frame->plane[0] = raw_data;
        frame->plane[1] = raw_data + s->width * s->height;
        frame->plane[2] = raw_data + s->width * s->height + (s->width * s->height) / 4;
        frame->stride[0] = s->width;
        frame->stride[1] = s->width / 2;
        frame->stride[2] = s->width / 2;

        uint8_t* y = raw_data;
        uint8_t* u = raw_data + s->width * s->height;
        uint8_t* v = raw_data + s->width * s->height + (s->width * s->height) / 4;

        memset(y, 128, s->width * s->height);
        memset(u, 128, (s->width * s->height) / 4);
        memset(v, 128, (s->width * s->height) / 4);

        int bar_pos = (s->frame_count * 8) % s->width;
        for (uint32_t r = 0; r < s->height; r++) {
            for (uint32_t c = bar_pos; c < bar_pos + 20 && c < s->width; c++) {
                y[r * s->width + c] = 235;
            }
        }

        struct timespec ts = { .tv_sec = 0, .tv_nsec = 33333333 };
        nanosleep(&ts, NULL);
    } else {
        struct pollfd pfd = { .fd = s->fd, .events = POLLIN };
        int res = poll(&pfd, 1, 200); // 200ms timeout

        struct v4l2_buffer vbuf = {0};
        vbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        if (strcmp(s->memory_type, "userptr") == 0) {
            vbuf.memory = V4L2_MEMORY_USERPTR;
        } else if (strcmp(s->memory_type, "dmabuf") == 0) {
            vbuf.memory = V4L2_MEMORY_DMABUF;
        } else {
            vbuf.memory = V4L2_MEMORY_MMAP;
        }

        int is_fallback = 0;
        if (res <= 0 || !(pfd.revents & POLLIN) || ioctl(s->fd, VIDIOC_DQBUF, &vbuf) < 0) {
            /* Timeout or error, generate a fallback frame.
             * But we need a buffer first. */
            buf = zst_buffer_create_with_pool(s->pool);
            if (!buf) return ZST_ERROR;
            // Best effort black frame fallback
            memset(buf->memory.data, 16, buf->memory.size);
            is_fallback = 1;
        } else {
            if (vbuf.memory == V4L2_MEMORY_MMAP && mmap_export) {
                buf = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
                if (!buf) {
                    (void)v4l2_source_requeue_mmap_buffer(s, vbuf.index);
                    return ZST_ERROR;
                }

                v4l2_export_buffer_ctx_t* ctx = calloc(1, sizeof(*ctx));
                if (!ctx) {
                    zst_buffer_unref(buf);
                    (void)v4l2_source_requeue_mmap_buffer(s, vbuf.index);
                    return ZST_ERROR;
                }

                int exported_fd = (s->exported_fds && vbuf.index < s->nb_buffers) ? s->exported_fds[vbuf.index] : -1;
                ctx->fd = exported_fd >= 0 ? dup(exported_fd) : -1;
                if (ctx->fd < 0) {
                    free(ctx);
                    zst_buffer_unref(buf);
                    (void)v4l2_source_requeue_mmap_buffer(s, vbuf.index);
                    return ZST_ERROR;
                }
                ctx->source = s;
                ctx->index = vbuf.index;

                buf->memory.type = ZST_MEMORY_DMABUF;
                buf->memory.data = s->buffers[vbuf.index].start;
                buf->memory.size = vbuf.bytesused ? vbuf.bytesused : s->buffers[vbuf.index].length;
                buf->memory.priv = ctx;
                buf->memory.release = v4l2src_dmabuf_release_callback;
            } else if (vbuf.memory == V4L2_MEMORY_MMAP) {
                buf = zst_buffer_create_with_pool(s->pool);
                if (!buf) return ZST_ERROR;

                uint8_t* raw_data = buf->memory.data;
                if (strcmp(s->pixel_format, "YUV420P") == 0 || strcmp(s->pixel_format, "I420") == 0) {
                    /* If we somehow configured the camera to YUV420P directly */
                    memcpy(raw_data, s->buffers[vbuf.index].start, buf->memory.size);
                } else {
                    /* Convert YUYV to YUV420P */
                    yuyv_to_yuv420p(s->width, s->height, s->buffers[vbuf.index].start, raw_data);
                }
                ioctl(s->fd, VIDIOC_QBUF, &vbuf);
            } else {
                /* USERPTR or DMABUF */
                buf = s->pool_buffers[vbuf.index];

                /* Now we need to queue a new buffer to keep V4L2 going */
                zst_buffer_t* new_buf = zst_buffer_create_with_pool(s->pool);
                if (new_buf) {
                    struct v4l2_buffer qbuf = {0};
                    qbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                    qbuf.memory = vbuf.memory;
                    qbuf.index = vbuf.index;

                    if (qbuf.memory == V4L2_MEMORY_USERPTR) {
                        qbuf.m.userptr = (unsigned long)new_buf->memory.data;
                        qbuf.length = new_buf->memory.size;
                    } else {
                        qbuf.m.fd = *(int*)new_buf->memory.priv;
                        qbuf.length = new_buf->memory.size;
                    }

                    if (ioctl(s->fd, VIDIOC_QBUF, &qbuf) >= 0) {
                        s->pool_buffers[vbuf.index] = new_buf;
                    } else {
                        /* If queuing fails, we might just unref it and fail later */
                        zst_buffer_unref(new_buf);
                        s->pool_buffers[vbuf.index] = NULL;
                    }
                } else {
                    s->pool_buffers[vbuf.index] = NULL;
                }
            }
        }

        if (buf) {
            zst_video_frame_t* frame = buf->payload;
            if (!frame) {
                frame = calloc(1, sizeof(*frame));
                if (!frame) { zst_buffer_unref(buf); return ZST_ERROR; }
                buf->payload = frame;
                buf->destroy = v4l2_buf_free;
            }

            frame->width = s->width;
            frame->height = s->height;
            uint8_t* raw_data = buf->memory.data;

            int is_yuv420 = 0;
            if (strcmp(s->pixel_format, "YUV420P") == 0 || strcmp(s->pixel_format, "I420") == 0) {
                is_yuv420 = 1;
            } else if ((vbuf.memory == V4L2_MEMORY_MMAP && !mmap_export) || is_fallback) {
                /* plain mmap and mock fallback paths convert to/generate YUV420P regardless of s->pixel_format */
                is_yuv420 = 1;
            }

            if (is_yuv420) {
                frame->format = 0; // YUV420P
                frame->plane[0] = raw_data;
                frame->plane[1] = raw_data + s->width * s->height;
                frame->plane[2] = raw_data + s->width * s->height + (s->width * s->height) / 4;
                frame->stride[0] = s->width;
                frame->stride[1] = s->width / 2;
                frame->stride[2] = s->width / 2;
            } else {
                frame->format = 1; // Assuming 1 is YUYV
                frame->plane[0] = raw_data;
                frame->plane[1] = NULL;
                frame->plane[2] = NULL;
                frame->stride[0] = s->width * 2;
                frame->stride[1] = 0;
                frame->stride[2] = 0;
            }
        }
    }

    if (buf) {
        if (el->clock) {
            buf->pts = zst_clock_get_time(el->clock);
        } else {
            buf->pts = s->frame_count * 33333333ULL; // 30 fps in nanoseconds
        }
        buf->duration = 33333333ULL;
        s->frame_count++;
        *out = buf;
        return ZST_OK;
    }

    return ZST_ERROR;
}

static void
v4l2_buf_free(zst_buffer_t* buf)
{
    if (buf) {
        // We only free payload since memory is managed by the allocator.
        if (buf->payload) {
            free(buf->payload);
            buf->payload = NULL;
        }
    }
}


static zst_buffer_pool_t*
element_get_pool(zst_element_t* el)
{
    v4l2_source_t* s = el->priv;
    return s->pool;
}

static zst_result_t
v4l2_set_property(zst_element_t* el, const char* name, const char* value)
{
    v4l2_source_t* s = el->priv;
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
        if (!v4l2_memory_type_is_valid(value)) {
            return ZST_ERROR;
        }
        snprintf(s->memory_type, sizeof(s->memory_type), "%s", value);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t
v4l2_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    v4l2_source_t* s = el->priv;
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
    .name    = "v4l2src",
    .open    = v4l2_open,
    .close   = v4l2_close,
    .start   = v4l2_start,
    .stop    = v4l2_stop,
    .process = v4l2_process,
    .set_property = v4l2_set_property,
    .get_property = v4l2_get_property,
    .get_pool = element_get_pool
};

zst_element_t*
zst_v4l2_source_create(void)
{
    zst_element_t* el;
    v4l2_source_t* priv;
    zst_pad_t* src;

    priv = calloc(1, sizeof(*priv));
    priv->fd = -1;

    el = zst_element_create(&g_ops, priv);
    src = zst_pad_create("src", ZST_PAD_SRC);
    zst_element_add_pad(el, src);

    return el;
}

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"
#include <string.h>

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "v4l2src") == 0) {
        return zst_v4l2_source_create();
    }
    return NULL;
}

static const zst_pad_template_t g_v4l2src_pads[] = {
    { "src", ZST_PAD_SRC, "video/x-raw" }
};

static const zst_property_spec_t g_v4l2src_props[] = {
    { "device", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "/dev/video0", "V4L2 capture device path" },
    { "width", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "640", "Capture width" },
    { "height", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "480", "Capture height" },
    { "pixel-format", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "YUYV", "Capture pixel format (YUYV, YUV420P/I420)" },
    { "memory-type", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "mmap", "V4L2 memory mode: mmap, userptr, dmabuf, mmap-export" }
};

static const zst_element_desc_t g_v4l2src_elements[] = {
    {
        .name = "v4l2src",
        .long_name = "V4L2 Source",
        .category = "Source/Video",
        .description = "Captures video from a V4L2 device",
        .author = "zstreamer",
        .properties = g_v4l2src_props,
        .nb_properties = sizeof(g_v4l2src_props) / sizeof(g_v4l2src_props[0]),
        .pads = g_v4l2src_pads,
        .nb_pads = sizeof(g_v4l2src_pads) / sizeof(g_v4l2src_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "v4l2source_plugin",
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
        *nb_elements_out = sizeof(g_v4l2src_elements) / sizeof(g_v4l2src_elements[0]);
    }
    return g_v4l2src_elements;
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