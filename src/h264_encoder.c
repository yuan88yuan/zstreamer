#include <stdlib.h>

#include "mm_element.h"
#include "mm_buffer.h"

typedef struct {

    void* ffmpeg;

} h264_encoder_t;

static mm_result_t
h264_process(
    mm_element_t* el,
    mm_buffer_t* in,
    mm_buffer_t** out)
{
    mm_buffer_t* pkt;

    /*
        send raw frame to encoder
    */

    pkt = mm_buffer_create(
        MM_BUFFER_VIDEO_PACKET);

    /*
        encoded packet here
    */

    *out = pkt;

    return MM_OK;
}

static mm_element_ops_t g_ops = {

    .name = "h264enc",

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

    src = mm_pad_create("src", MM_PAD_SRC);

    mm_element_add_pad(el, sink);

    mm_element_add_pad(el, src);

    return el;
}