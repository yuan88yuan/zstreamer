#include <stdio.h>
#include <stdlib.h>

#include "zst_element.h"
#include "zst_buffer.h"

typedef struct {

    FILE* fp;

    char path[256];

} file_sink_t;

static zst_result_t
file_open(zst_element_t* el)
{
    file_sink_t* s = el->priv;

    s->fp = fopen(s->path, "wb");

    if (!s->fp)
        return ZST_ERROR;

    return ZST_OK;
}

static zst_result_t
file_close(zst_element_t* el)
{
    file_sink_t* s = el->priv;

    if (s->fp) {
        fclose(s->fp);
        s->fp = NULL;
    }

    return ZST_OK;
}

static zst_result_t
file_process(
    zst_element_t* el,
    zst_buffer_t* in,
    zst_buffer_t** out)
{
    file_sink_t* s = el->priv;
    (void)out;

    if (!s->fp || !in)
        return ZST_ERROR;

    if (in->memory.data && in->memory.size > 0) {
        size_t written = fwrite(in->memory.data, 1, in->memory.size, s->fp);
        if (written != in->memory.size) {
            return ZST_ERROR;
        }
    }

    return ZST_OK;
}

static zst_element_ops_t g_ops = {

    .name = "filesink",

    .open = file_open,
    
    .close = file_close,

    .process = file_process,
};

zst_element_t*
zst_file_sink_create(
    const char* path)
{
    zst_element_t* el;

    file_sink_t* priv;

    zst_pad_t* sink;

    priv = calloc(1, sizeof(*priv));

    snprintf(
        priv->path,
        sizeof(priv->path),
        "%s",
        path);

    el = zst_element_create(&g_ops, priv);

    sink = zst_pad_create(
        "sink",
        ZST_PAD_SINK);

    zst_element_add_pad(el, sink);

    return el;
}

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"
#include <string.h>

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "filesink") == 0) {
        return zst_file_sink_create("");
    }
    return NULL;
}

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "filesink_plugin",
        .author = "Antigravity",
        .version = "1.0.0",
        .init = NULL,
        .deinit = NULL
    },
    .create_element = plugin_create_element
};

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