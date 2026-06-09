#include <stdlib.h>
#include <string.h>

#include "mm_element.h"
#include "mm_buffer.h"

typedef struct {

    int fd;

} v4l2_source_t;

static mm_result_t
v4l2_open(mm_element_t* el)
{
    v4l2_source_t* s = el->priv;

    /* open camera */

    return MM_OK;
}

static mm_result_t
v4l2_start(mm_element_t* el)
{
    return MM_OK;
}

static mm_result_t
v4l2_process(
    mm_element_t* el,
    mm_buffer_t* in,
    mm_buffer_t** out)
{
    mm_buffer_t* buf;

    buf = mm_buffer_create(
        MM_BUFFER_VIDEO_FRAME);

    /*
        capture frame from v4l2
    */

    *out = buf;

    return MM_OK;
}

static mm_element_ops_t g_ops = {

    .name = "v4l2src",

    .open = v4l2_open,

    .start = v4l2_start,

    .process = v4l2_process,
};

mm_element_t*
mm_v4l2_source_create(void)
{
    mm_element_t* el;

    v4l2_source_t* priv;

    mm_pad_t* src;

    priv = calloc(1, sizeof(*priv));

    el = mm_element_create(&g_ops, priv);

    src = mm_pad_create("src", MM_PAD_SRC);

    mm_element_add_pad(el, src);

    return el;
}