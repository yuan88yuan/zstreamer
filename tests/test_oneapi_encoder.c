/*=============================================================================
    test_oneapi_encoder.c — Intel oneAPI/oneVPL encoder smoke test
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "zst_buffer.h"
#include "zst_element.h"
#include "zst_element_factory.h"
#include "zstreamer/elements/zst_oneapi_encoder.h"

static void release_memory(void* priv)
{
    free(priv);
}

static void destroy_frame(zst_buffer_t* buf)
{
    free(buf->payload);
    buf->payload = NULL;
}

static zst_buffer_t* make_frame(uint32_t width, uint32_t height, uint64_t index)
{
    size_t y_size = (size_t)width * height;
    size_t uv_size = y_size / 4;
    uint8_t* data = malloc(y_size + uv_size * 2);
    zst_video_frame_t* frame = calloc(1, sizeof(*frame));
    zst_buffer_t* buf = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
    assert(data && frame && buf);

    memset(data, (int)(16 + (index * 7) % 200), y_size);
    memset(data + y_size, 128, uv_size * 2);

    frame->width = width;
    frame->height = height;
    frame->format = 0;
    frame->plane[0] = data;
    frame->plane[1] = data + y_size;
    frame->plane[2] = data + y_size + uv_size;
    frame->stride[0] = width;
    frame->stride[1] = width / 2;
    frame->stride[2] = width / 2;

    buf->payload = frame;
    buf->destroy = destroy_frame;
    buf->memory.type = ZST_MEMORY_CPU;
    buf->memory.data = data;
    buf->memory.size = y_size + uv_size * 2;
    buf->memory.priv = data;
    buf->memory.release = release_memory;
    buf->pts = index * 33333333ULL;
    buf->duration = 33333333ULL;
    return buf;
}

int main(void)
{
    if (access("/dev/dri/renderD128", R_OK | W_OK) != 0) {
        printf("SKIP: /dev/dri/renderD128 is not available; run with --device /dev/dri for hardware encode\n");
        return 0;
    }

    assert(zst_register_builtin_elements() == ZST_OK);
    assert(zst_element_factory_get_desc("oneapienc") != NULL);

    zst_element_t* enc = zst_element_factory_make("oneapienc");
    assert(enc != NULL);
    assert(zst_element_set_property(enc, "codec", "h264") == ZST_OK);
    assert(zst_element_set_property_int(enc, "bitrate", 2000000) == ZST_OK);
    assert(zst_element_set_property_int(enc, "gop-size", 15) == ZST_OK);
    assert(zst_element_set_property(enc, "fps", "30/1") == ZST_OK);
    assert(zst_element_set_property(enc, "profile", "high") == ZST_OK);
    assert(zst_element_set_property(enc, "level", "4.1") == ZST_OK);
    assert(zst_element_set_state(enc, ZST_STATE_PLAYING) == ZST_OK);

    size_t total_encoded = 0;
    int packets = 0;
    for (uint64_t i = 0; i < 30; i++) {
        zst_buffer_t* frame = make_frame(320, 240, i);
        zst_buffer_t* pkt = NULL;
        zst_result_t r = enc->ops->process(enc, frame, &pkt);
        zst_buffer_unref(frame);
        assert(r == ZST_OK);
        if (pkt) {
            assert(pkt->type == ZST_BUFFER_VIDEO_PACKET);
            assert(pkt->memory.data != NULL);
            assert(pkt->memory.size > 0);
            total_encoded += pkt->memory.size;
            packets++;
            zst_buffer_unref(pkt);
        }
    }

    int saw_eos = 0;
    for (int i = 0; i < 16 && !saw_eos; i++) {
        zst_buffer_t* eos = zst_buffer_create(ZST_BUFFER_VIDEO_PACKET);
        zst_buffer_t* drained = NULL;
        assert(eos != NULL);
        eos->flags |= ZST_BUFFER_FLAG_EOS;
        assert(enc->ops->process(enc, eos, &drained) == ZST_OK);
        zst_buffer_unref(eos);
        if (drained) {
            if (drained->flags & ZST_BUFFER_FLAG_EOS) {
                saw_eos = 1;
            } else {
                total_encoded += drained->memory.size;
                packets++;
            }
            zst_buffer_unref(drained);
        }
    }

    printf("oneapienc produced %d packets, %zu bytes\n", packets, total_encoded);
    assert(packets > 0);
    assert(total_encoded > 0);
    assert(saw_eos);

    zst_buffer_t* oneapi_frame = make_frame(320, 240, 31);
    zst_buffer_t* rejected = NULL;
    oneapi_frame->memory.type = ZST_MEMORY_ONEAPI;
    assert(enc->ops->process(enc, oneapi_frame, &rejected) == ZST_ERROR_NOT_IMPLEMENTED);
    assert(rejected == NULL);
    zst_buffer_unref(oneapi_frame);

    assert(zst_element_set_state(enc, ZST_STATE_NULL) == ZST_OK);
    zst_element_destroy(enc);

    zst_element_t* hevc = zst_oneapi_encoder_create();
    assert(hevc != NULL);
    assert(zst_element_set_property(hevc, "codec", "h265") == ZST_OK);
    assert(zst_element_set_property(hevc, "profile", "main") == ZST_OK);
    zst_element_destroy(hevc);
    return 0;
}
