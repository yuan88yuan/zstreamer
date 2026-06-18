import re

with open("src/v4l2_source.c", "r") as f:
    content = f.read()

old_process = """        if (buf) {
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
            } else if (vbuf.memory == V4L2_MEMORY_MMAP || res <= 0 || !(pfd.revents & POLLIN)) {
                /* mmap and mock fallback paths convert to/generate YUV420P regardless of s->pixel_format */
                is_yuv420 = 1;
            }"""

new_process = """        int is_fallback = 0;
        if (res <= 0 || !(pfd.revents & POLLIN) || ioctl(s->fd, VIDIOC_DQBUF, &vbuf) < 0) {
            /* Timeout or error, generate a fallback frame.
             * But we need a buffer first. */
            buf = zst_buffer_create_with_pool(s->pool);
            if (!buf) return ZST_ERROR;
            // Best effort black frame fallback
            memset(buf->memory.data, 16, buf->memory.size);
            is_fallback = 1;
        } else {
            if (vbuf.memory == V4L2_MEMORY_MMAP) {
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
            } else if (vbuf.memory == V4L2_MEMORY_MMAP || is_fallback) {
                /* mmap and mock fallback paths convert to/generate YUV420P regardless of s->pixel_format */
                is_yuv420 = 1;
            }"""

content = re.sub(
    r'        if \(res <= 0 \|\| !\(pfd\.revents & POLLIN\) \|\| ioctl\(s->fd, VIDIOC_DQBUF, &vbuf\) < 0\) \{.*?\/\* mmap and mock fallback paths convert to/generate YUV420P regardless of s->pixel_format \*\/\n                is_yuv420 = 1;\n            \}',
    new_process,
    content,
    flags=re.DOTALL
)

with open("src/v4l2_source.c", "w") as f:
    f.write(content)
