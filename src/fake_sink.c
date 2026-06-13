#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zst_element.h"
#include "zstreamer/elements/zst_fake_sink.h"
#include "zst_element_factory.h"
#include "zst_buffer.h"

typedef struct {
    double drop_probability;
    uint64_t total_buffers;
    uint64_t total_bytes;
} fake_sink_t;

static zst_result_t
fake_open(zst_element_t* el)
{
    fake_sink_t* s = el->priv;
    s->total_buffers = 0;
    s->total_bytes = 0;
    return ZST_OK;
}

static zst_result_t
fake_process(
    zst_element_t* el,
    zst_buffer_t* in,
    zst_buffer_t** out)
{
    fake_sink_t* s = el->priv;
    (void)out;

    if (!in)
        return ZST_ERROR;

    /* Drop probability check */
    if (s->drop_probability > 0.0) {
        double r = rand() / (double)RAND_MAX;
        if (r <= s->drop_probability) {
            /* Drop buffer, do not count */
            return ZST_OK;
        }
    }

    /* Accumulate stats */
    s->total_buffers++;
    if (in->memory.data && in->memory.size > 0) {
        s->total_bytes += in->memory.size;
    }

    return ZST_OK;
}

static zst_result_t
fake_set_property(
    zst_element_t* el,
    const char* name,
    const char* value)
{
    fake_sink_t* s = el->priv;
    if (strcmp(name, "drop-probability") == 0) {
        s->drop_probability = atof(value);
        if (s->drop_probability < 0.0) s->drop_probability = 0.0;
        if (s->drop_probability > 1.0) s->drop_probability = 1.0;
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t
fake_get_property(
    zst_element_t* el,
    const char* name,
    char* value_out,
    size_t max_len)
{
    fake_sink_t* s = el->priv;
    if (strcmp(name, "drop-probability") == 0) {
        snprintf(value_out, max_len, "%f", s->drop_probability);
        return ZST_OK;
    } else if (strcmp(name, "total-buffers") == 0) {
        snprintf(value_out, max_len, "%llu", (unsigned long long)s->total_buffers);
        return ZST_OK;
    } else if (strcmp(name, "total-bytes") == 0) {
        snprintf(value_out, max_len, "%llu", (unsigned long long)s->total_bytes);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_element_ops_t g_ops = {
    .name = "fakesink",
    .open = fake_open,
    .process = fake_process,
    .set_property = fake_set_property,
    .get_property = fake_get_property,
};

zst_element_t*
zst_fake_sink_create(void)
{
    zst_element_t* el;
    fake_sink_t* priv;
    zst_pad_t* sink;

    priv = calloc(1, sizeof(*priv));
    priv->drop_probability = 0.0;
    priv->total_buffers = 0;
    priv->total_bytes = 0;

    el = zst_element_create(&g_ops, priv);

    sink = zst_pad_create("sink", ZST_PAD_SINK);
    zst_element_add_pad(el, sink);

    return el;
}

zst_element_t*
zst_fake_sink_create_with_config(const zst_fake_sink_config_t* config)
{
    if (!config || config->struct_size < sizeof(zst_fake_sink_config_t)) return NULL;
    zst_element_t* el = zst_element_factory_make("fakesink");
    if (!el) return NULL;

    zst_element_set_property_double(el, "drop-probability", config->drop_probability);
    return el;
}

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "fakesink") == 0) {
        return zst_fake_sink_create();
    }
    return NULL;
}

static const zst_property_spec_t g_fakesink_properties[] = {
    { "drop-probability", ZST_PROPERTY_DOUBLE, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE,
      "0.0", "Probability in [0.0, 1.0] of dropping a buffer without counting it" },
    { "total-buffers", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE,
      "0", "Number of buffers received since open" },
    { "total-bytes", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE,
      "0", "Number of bytes received since open" }
};

static const zst_pad_template_t g_fakesink_pads[] = {
    { "sink", ZST_PAD_SINK, "ANY" }
};

static const zst_element_desc_t g_fakesink_elements[] = {
    {
        .name = "fakesink",
        .long_name = "Fake Sink",
        .category = "Sink/Test",
        .description = "Consumes buffers and records simple statistics",
        .author = "zstreamer",
        .properties = g_fakesink_properties,
        .nb_properties = sizeof(g_fakesink_properties) / sizeof(g_fakesink_properties[0]),
        .pads = g_fakesink_pads,
        .nb_pads = sizeof(g_fakesink_pads) / sizeof(g_fakesink_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "fakesink_plugin",
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
        *nb_elements_out = sizeof(g_fakesink_elements) / sizeof(g_fakesink_elements[0]);
    }
    return g_fakesink_elements;
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
