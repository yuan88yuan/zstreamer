#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zst_element.h"
#include "zstreamer/elements/zst_file_sink.h"
#include "zst_element_factory.h"
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

static zst_result_t
file_set_property(zst_element_t* el, const char* name, const char* value)
{
    file_sink_t* s = el->priv;

    if (strcmp(name, "path") == 0 || strcmp(name, "location") == 0) {
        snprintf(s->path, sizeof(s->path), "%s", value);
        return ZST_OK;
    }

    return ZST_ERROR;
}

static zst_result_t
file_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    file_sink_t* s = el->priv;

    if (strcmp(name, "path") == 0 || strcmp(name, "location") == 0) {
        snprintf(value_out, max_len, "%s", s->path);
        return ZST_OK;
    }

    return ZST_ERROR;
}

static zst_element_ops_t g_ops = {

    .name = "filesink",

    .open = file_open,
    
    .close = file_close,

    .process = file_process,
    .set_property = file_set_property,
    .get_property = file_get_property,
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

zst_element_t*
zst_file_sink_create_with_config(const zst_file_sink_config_t* config)
{
    if (!config || config->struct_size < sizeof(zst_file_sink_config_t)) return NULL;
    zst_element_t* el = zst_element_factory_make("filesink");
    if (!el) return NULL;

    if (config->path) {
        zst_element_set_property_string(el, "path", config->path);
    }
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

static const zst_property_spec_t g_filesink_properties[] = {
    { "path", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE,
      "", "Output file path" },
    { "location", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE,
      "", "Alias for path" }
};

static const zst_pad_template_t g_filesink_pads[] = {
    { "sink", ZST_PAD_SINK, "ANY" }
};

static const zst_element_desc_t g_filesink_elements[] = {
    {
        .name = "filesink",
        .long_name = "File Sink",
        .category = "Sink/File",
        .description = "Writes incoming buffers to a local file",
        .author = "zstreamer",
        .properties = g_filesink_properties,
        .nb_properties = sizeof(g_filesink_properties) / sizeof(g_filesink_properties[0]),
        .pads = g_filesink_pads,
        .nb_pads = sizeof(g_filesink_pads) / sizeof(g_filesink_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "filesink_plugin",
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
        *nb_elements_out = sizeof(g_filesink_elements) / sizeof(g_filesink_elements[0]);
    }
    return g_filesink_elements;
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