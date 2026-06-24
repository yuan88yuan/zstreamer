#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "zst_element.h"
#include "zstreamer/elements/zst_fake_sink.h"
#include "zst_element_factory.h"
#include "zst_buffer.h"
#include "zst_log.h"

typedef struct {
    double drop_probability;
    uint64_t total_buffers;
    uint64_t total_bytes;
    bool bits_per_second;
    bool push_per_second;
    uint64_t log_period_sec;
    uint64_t stats_window_buffers;
    uint64_t stats_window_bytes;
    uint64_t stats_window_start_ns;
} fake_sink_t;

static uint64_t
fake_monotonic_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void
fake_reset_log_window(fake_sink_t* s)
{
    if (!s) return;
    s->stats_window_buffers = 0;
    s->stats_window_bytes = 0;
    s->stats_window_start_ns = 0;
}

static int
fake_parse_bool(const char* value, bool* out)
{
    if (!value || !out) return 0;
    if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 ||
        strcmp(value, "TRUE") == 0 || strcmp(value, "on") == 0 ||
        strcmp(value, "ON") == 0 || strcmp(value, "yes") == 0 ||
        strcmp(value, "YES") == 0) {
        *out = true;
        return 1;
    }
    if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0 ||
        strcmp(value, "FALSE") == 0 || strcmp(value, "off") == 0 ||
        strcmp(value, "OFF") == 0 || strcmp(value, "no") == 0 ||
        strcmp(value, "NO") == 0) {
        *out = false;
        return 1;
    }
    return 0;
}

static int
fake_parse_log_period(const char* value, uint64_t* out)
{
    if (!value || !out) return 0;
    errno = 0;
    char* end = NULL;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || (end && *end != '\0') || parsed == 0) {
        return 0;
    }
    *out = (uint64_t)parsed;
    return 1;
}

static void
fake_maybe_log_stats(fake_sink_t* s, const zst_buffer_t* in)
{
    if (!s || !in || (!s->bits_per_second && !s->push_per_second)) return;

    uint64_t now = fake_monotonic_now_ns();
    if (s->stats_window_start_ns == 0) {
        s->stats_window_start_ns = now;
    }

    s->stats_window_buffers++;
    s->stats_window_bytes += in->memory.size;

    uint64_t period_ns = s->log_period_sec * 1000000000ULL;
    uint64_t elapsed_ns = now - s->stats_window_start_ns;
    if (elapsed_ns < period_ns) return;

    double elapsed_sec = (double)elapsed_ns / 1000000000.0;
    if (elapsed_sec <= 0.0) return;

    double bps = ((double)s->stats_window_bytes * 8.0) / elapsed_sec;
    double pps = (double)s->stats_window_buffers / elapsed_sec;

    if (s->bits_per_second && s->push_per_second) {
        ZST_LOG_INFO("fakesink",
                     "stats: bits-per-second=%.2f push-per-second=%.2f period=%.3fs bytes=%" PRIu64 " pushes=%" PRIu64,
                     bps, pps, elapsed_sec,
                     s->stats_window_bytes, s->stats_window_buffers);
    } else if (s->bits_per_second) {
        ZST_LOG_INFO("fakesink",
                     "stats: bits-per-second=%.2f period=%.3fs bytes=%" PRIu64,
                     bps, elapsed_sec, s->stats_window_bytes);
    } else {
        ZST_LOG_INFO("fakesink",
                     "stats: push-per-second=%.2f period=%.3fs pushes=%" PRIu64,
                     pps, elapsed_sec, s->stats_window_buffers);
    }

    fake_reset_log_window(s);
    s->stats_window_start_ns = now;
}

static zst_result_t
fake_open(zst_element_t* el)
{
    fake_sink_t* s = el->priv;
    s->total_buffers = 0;
    s->total_bytes = 0;
    fake_reset_log_window(s);
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
    fake_maybe_log_stats(s, in);

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
    } else if (strcmp(name, "bits-per-second") == 0) {
        bool enabled = false;
        if (!fake_parse_bool(value, &enabled)) return ZST_ERROR;
        s->bits_per_second = enabled;
        fake_reset_log_window(s);
        return ZST_OK;
    } else if (strcmp(name, "push-per-second") == 0) {
        bool enabled = false;
        if (!fake_parse_bool(value, &enabled)) return ZST_ERROR;
        s->push_per_second = enabled;
        fake_reset_log_window(s);
        return ZST_OK;
    } else if (strcmp(name, "log-period") == 0) {
        uint64_t period = 0;
        if (!fake_parse_log_period(value, &period)) return ZST_ERROR;
        s->log_period_sec = period;
        fake_reset_log_window(s);
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
    } else if (strcmp(name, "bits-per-second") == 0) {
        snprintf(value_out, max_len, "%s", s->bits_per_second ? "true" : "false");
        return ZST_OK;
    } else if (strcmp(name, "push-per-second") == 0) {
        snprintf(value_out, max_len, "%s", s->push_per_second ? "true" : "false");
        return ZST_OK;
    } else if (strcmp(name, "log-period") == 0) {
        snprintf(value_out, max_len, "%llu", (unsigned long long)s->log_period_sec);
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
    priv->bits_per_second = false;
    priv->push_per_second = false;
    priv->log_period_sec = 1;
    fake_reset_log_window(priv);

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
      "0", "Number of bytes received since open" },
    { "bits-per-second", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE | ZST_PROPERTY_RUNTIME,
      "false", "Log received bitrate statistics every log-period seconds" },
    { "log-period", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE | ZST_PROPERTY_RUNTIME,
      "1", "Statistics log period in seconds" },
    { "push-per-second", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE | ZST_PROPERTY_RUNTIME,
      "false", "Log received buffer push-rate statistics every log-period seconds" }
};

static const zst_pad_template_t g_fakesink_pads[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "ANY" }
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
