#include <stdio.h>
#include <stdlib.h>

#include "mm_element.h"
#include "mm_buffer.h"

typedef struct {

    FILE* fp;

    char path[256];

} file_sink_t;

static mm_result_t
file_open(mm_element_t* el)
{
    file_sink_t* s = el->priv;

    s->fp = fopen(s->path, "wb");

    if (!s->fp)
        return MM_ERROR;

    return MM_OK;
}

static mm_result_t
file_close(mm_element_t* el)
{
    file_sink_t* s = el->priv;

    if (s->fp) {
        fclose(s->fp);
        s->fp = NULL;
    }

    return MM_OK;
}

static mm_result_t
file_process(
    mm_element_t* el,
    mm_buffer_t* in,
    mm_buffer_t** out)
{
    file_sink_t* s = el->priv;
    (void)out;

    if (!s->fp || !in)
        return MM_ERROR;

    if (in->memory.data && in->memory.size > 0) {
        size_t written = fwrite(in->memory.data, 1, in->memory.size, s->fp);
        if (written != in->memory.size) {
            return MM_ERROR;
        }
    }

    return MM_OK;
}

static mm_element_ops_t g_ops = {

    .name = "filesink",

    .open = file_open,
    
    .close = file_close,

    .process = file_process,
};

mm_element_t*
mm_file_sink_create(
    const char* path)
{
    mm_element_t* el;

    file_sink_t* priv;

    mm_pad_t* sink;

    priv = calloc(1, sizeof(*priv));

    snprintf(
        priv->path,
        sizeof(priv->path),
        "%s",
        path);

    el = mm_element_create(&g_ops, priv);

    sink = mm_pad_create(
        "sink",
        MM_PAD_SINK);

    mm_element_add_pad(el, sink);

    return el;
}