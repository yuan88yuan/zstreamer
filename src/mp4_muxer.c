#include <stdlib.h>

#include "mm_element.h"

typedef struct {

    void* ctx;

} mp4_muxer_t;

static mm_result_t
mux_process(
    mm_element_t* el,
    mm_buffer_t* in,
    mm_buffer_t** out)
{
    /*
        write packet into muxer
    */

    *out = in;

    return MM_OK;
}

static mm_element_ops_t g_ops = {

    .name = "mp4mux",

    .process = mux_process,
};

mm_element_t*
mm_mp4_muxer_create(void)
{
    mm_element_t* el;

    mp4_muxer_t* priv;

    mm_pad_t* video;
    mm_pad_t* audio;
    mm_pad_t* src;

    priv = calloc(1, sizeof(*priv));

    el = mm_element_create(&g_ops, priv);

    video = mm_pad_create(
        "video",
        MM_PAD_SINK);

    audio = mm_pad_create(
        "audio",
        MM_PAD_SINK);

    src = mm_pad_create(
        "src",
        MM_PAD_SRC);

    mm_element_add_pad(el, video);

    mm_element_add_pad(el, audio);

    mm_element_add_pad(el, src);

    return el;
}