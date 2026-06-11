/*=============================================================================
    file_source.c — Reads raw/containerised media data from a local file
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "zst_element.h"
#include "zstreamer/elements/zst_file_source.h"
#include "zst_element_factory.h"
#include "zst_buffer.h"
#include "zst_buffer_pool.h"
#include "zst_log.h"
#include "zst_clock.h"

typedef struct {
    FILE* fp;
    char path[256];
    int chunk_size;
    bool loop;
    int64_t offset;
    int64_t length;
    uint64_t bytes_read;
    zst_buffer_pool_t* pool;
} file_source_t;

static bool
has_extension(const char* ext, const char* target)
{
    if (!ext || !target) return false;
    while (*ext && *target) {
        char c1 = *ext;
        char c2 = *target;
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return false;
        ext++;
        target++;
    }
    return *ext == '\0' && *target == '\0';
}

static const char*
determine_media_type(const char* path)
{
    if (!path) return "application/octet-stream";
    const char* ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";

    if (has_extension(ext, ".h264") || has_extension(ext, ".264")) {
        return "video/x-h264";
    } else if (has_extension(ext, ".aac")) {
        return "audio/aac";
    } else if (has_extension(ext, ".txt")) {
        return "text/plain";
    } else if (has_extension(ext, ".mp4")) {
        return "video/mp4";
    } else if (has_extension(ext, ".yuv")) {
        return "video/x-raw";
    } else if (has_extension(ext, ".pcm")) {
        return "audio/x-raw";
    }
    return "application/octet-stream";
}

static uint32_t
determine_buffer_type(const char* media_type)
{
    if (strcmp(media_type, "video/x-raw") == 0) {
        return ZST_BUFFER_VIDEO_FRAME;
    } else if (strcmp(media_type, "audio/x-raw") == 0) {
        return ZST_BUFFER_AUDIO_FRAME;
    } else if (strcmp(media_type, "video/x-h264") == 0) {
        return ZST_BUFFER_VIDEO_PACKET;
    } else if (strcmp(media_type, "audio/aac") == 0) {
        return ZST_BUFFER_AUDIO_PACKET;
    }
    return ZST_BUFFER_USER;
}

static zst_result_t
file_source_open(zst_element_t* el)
{
    file_source_t* s = el->priv;
    s->bytes_read = 0;

    if (s->chunk_size <= 0) {
        s->chunk_size = 4096;
    }

    s->fp = fopen(s->path, "rb");
    if (!s->fp) {
        ZST_LOG_ERROR("filesrc", "failed to open file '%s'", s->path);
        return ZST_ERROR;
    }

    if (s->offset > 0) {
        if (fseek(s->fp, s->offset, SEEK_SET) != 0) {
            ZST_LOG_ERROR("filesrc", "failed to seek to offset %ld", (long)s->offset);
            fclose(s->fp);
            s->fp = NULL;
            return ZST_ERROR;
        }
    }

    const char* media_type = determine_media_type(s->path);
    uint32_t buffer_type = determine_buffer_type(media_type);

    zst_buffer_pool_config_t pool_cfg = {
        .min_buffers = 4,
        .max_buffers = 8,
        .buffer_size = (size_t)s->chunk_size,
        .buffer_type = buffer_type
    };
    s->pool = zst_buffer_pool_create(NULL, &pool_cfg);
    if (!s->pool) {
        ZST_LOG_ERROR("filesrc", "failed to create buffer pool");
        fclose(s->fp);
        s->fp = NULL;
        return ZST_ERROR;
    }

    return ZST_OK;
}

static zst_result_t
file_source_close(zst_element_t* el)
{
    file_source_t* s = el->priv;
    if (s->fp) {
        fclose(s->fp);
        s->fp = NULL;
    }
    if (s->pool) {
        zst_buffer_pool_destroy(s->pool);
        s->pool = NULL;
    }
    return ZST_OK;
}

static zst_result_t
file_source_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    (void)in;
    file_source_t* s = el->priv;

    if (!s->fp) {
        return ZST_ERROR;
    }

    while (1) {
        size_t to_read = (size_t)s->chunk_size;
        if (s->length >= 0) {
            if (s->bytes_read >= (uint64_t)s->length) {
                if (s->loop) {
                    if (fseek(s->fp, s->offset, SEEK_SET) != 0) {
                        return ZST_EOF;
                    }
                    s->bytes_read = 0;
                } else {
                    return ZST_EOF;
                }
            }
            if ((uint64_t)s->length - s->bytes_read < to_read) {
                to_read = (size_t)((uint64_t)s->length - s->bytes_read);
            }
        }

        if (to_read == 0) {
            if (s->loop) {
                if (fseek(s->fp, s->offset, SEEK_SET) != 0) {
                    return ZST_EOF;
                }
                s->bytes_read = 0;
                continue;
            } else {
                return ZST_EOF;
            }
        }

        zst_buffer_t* buf = NULL;
        if (zst_buffer_pool_acquire(s->pool, &buf, 0, 0) != ZST_OK) {
            return ZST_ERROR;
        }

        buf->memory.size = (size_t)s->chunk_size; // reset size back to pool buffer_size
        size_t n = fread(buf->memory.data, 1, to_read, s->fp);
        if (n == 0) {
            zst_buffer_unref(buf);
            if (ferror(s->fp)) {
                return ZST_ERROR;
            }
            if (s->loop) {
                long pos = ftell(s->fp);
                if (pos == s->offset) {
                    // Empty or unreadable file at offset, avoid infinite loop
                    return ZST_EOF;
                }
                if (fseek(s->fp, s->offset, SEEK_SET) != 0) {
                    return ZST_EOF;
                }
                s->bytes_read = 0;
                continue;
            } else {
                return ZST_EOF;
            }
        }

        buf->memory.size = n;
        s->bytes_read += n;

        if (el->clock) {
            buf->pts = zst_clock_get_time(el->clock);
        } else {
            buf->pts = 0;
        }
        buf->duration = 0;

        *out = buf;
        return ZST_OK;
    }
}

static zst_caps_t*
file_source_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)pad;
    (void)filter;
    file_source_t* s = el->priv;
    zst_caps_t* caps = zst_caps_create();
    if (!caps) return NULL;

    const char* media_type = determine_media_type(s->path);
    zst_caps_struct_t* caps_struct = NULL;
    if (strncmp(media_type, "video/", 6) == 0) {
        caps_struct = zst_caps_struct_create_video(media_type, 0, 0, 0.0, "");
    } else if (strncmp(media_type, "audio/", 6) == 0) {
        caps_struct = zst_caps_struct_create_audio(media_type, 0, 0, "");
    } else {
        caps_struct = calloc(1, sizeof(*caps_struct));
        if (caps_struct) {
            strncpy(caps_struct->media_type, media_type, sizeof(caps_struct->media_type) - 1);
            caps_struct->type = ZST_CAPS_ANY;
        }
    }

    if (caps_struct) {
        zst_caps_append(caps, caps_struct);
    }
    return caps;
}

static zst_result_t
file_source_set_property(zst_element_t* el, const char* name, const char* value)
{
    file_source_t* s = el->priv;
    if (strcmp(name, "path") == 0) {
        strncpy(s->path, value, sizeof(s->path) - 1);
        s->path[sizeof(s->path) - 1] = '\0';
        return ZST_OK;
    } else if (strcmp(name, "chunk-size") == 0 || strcmp(name, "chunk_size") == 0) {
        s->chunk_size = atoi(value);
        return ZST_OK;
    } else if (strcmp(name, "loop") == 0) {
        s->loop = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
        return ZST_OK;
    } else if (strcmp(name, "offset") == 0) {
        s->offset = atoll(value);
        return ZST_OK;
    } else if (strcmp(name, "length") == 0) {
        s->length = atoll(value);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t
file_source_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    file_source_t* s = el->priv;
    if (strcmp(name, "path") == 0) {
        strncpy(value_out, s->path, max_len - 1);
        value_out[max_len - 1] = '\0';
        return ZST_OK;
    } else if (strcmp(name, "chunk-size") == 0 || strcmp(name, "chunk_size") == 0) {
        snprintf(value_out, max_len, "%d", s->chunk_size);
        return ZST_OK;
    } else if (strcmp(name, "loop") == 0) {
        strncpy(value_out, s->loop ? "true" : "false", max_len - 1);
        value_out[max_len - 1] = '\0';
        return ZST_OK;
    } else if (strcmp(name, "offset") == 0) {
        snprintf(value_out, max_len, "%lld", (long long)s->offset);
        return ZST_OK;
    } else if (strcmp(name, "length") == 0) {
        snprintf(value_out, max_len, "%lld", (long long)s->length);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_element_ops_t g_ops = {
    .name = "filesrc",
    .open = file_source_open,
    .close = file_source_close,
    .process = file_source_process,
    .get_caps = file_source_get_caps,
    .set_property = file_source_set_property,
    .get_property = file_source_get_property
};

zst_element_t*
zst_file_source_create(const char* path)
{
    zst_element_t* el;
    file_source_t* priv;
    zst_pad_t* src;

    priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;

    if (path) {
        strncpy(priv->path, path, sizeof(priv->path) - 1);
        priv->path[sizeof(priv->path) - 1] = '\0';
    }
    priv->chunk_size = 4096;
    priv->loop = false;
    priv->offset = 0;
    priv->length = -1;
    priv->bytes_read = 0;

    el = zst_element_create(&g_ops, priv);
    if (!el) {
        free(priv);
        return NULL;
    }

    src = zst_pad_create("src", ZST_PAD_SRC);
    if (!src) {
        zst_element_destroy(el);
        return NULL;
    }

    zst_element_add_pad(el, src);
    return el;
}

zst_element_t*
zst_file_source_create_with_config(const zst_file_source_config_t* config)
{
    if (!config || config->struct_size < sizeof(zst_file_source_config_t)) return NULL;
    zst_element_t* el = zst_element_factory_make("filesrc");
    if (!el) return NULL;

    if (config->path) {
        zst_element_set_property_string(el, "path", config->path);
    }
    if (config->chunk_size > 0) {
        zst_element_set_property_uint(el, "chunk-size", config->chunk_size);
    }
    zst_element_set_property_bool(el, "loop", config->loop);
    zst_element_set_property_int(el, "offset", config->offset);
    zst_element_set_property_int(el, "length", config->length);

    return el;
}

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "filesrc") == 0 || strcmp(name, "file_source") == 0) {
        return zst_file_source_create("");
    }
    return NULL;
}

static const zst_property_spec_t g_filesrc_properties[] = {
    { "path", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE,
      "", "Input file path" },
    { "chunk-size", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE,
      "4096", "Maximum bytes to read per buffer" },
    { "loop", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE,
      "false", "Loop back to the start at EOF" },
    { "offset", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE,
      "0", "Initial byte offset" },
    { "length", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE,
      "-1", "Maximum number of bytes to read; -1 means unlimited" }
};

static const zst_pad_template_t g_filesrc_pads[] = {
    { "src", ZST_PAD_SRC, "ANY" }
};

static const zst_element_desc_t g_filesrc_elements[] = {
    {
        .name = "filesrc",
        .long_name = "File Source",
        .category = "Source/File",
        .description = "Reads buffers from a local file",
        .author = "zstreamer",
        .properties = g_filesrc_properties,
        .nb_properties = sizeof(g_filesrc_properties) / sizeof(g_filesrc_properties[0]),
        .pads = g_filesrc_pads,
        .nb_pads = sizeof(g_filesrc_pads) / sizeof(g_filesrc_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "filesrc_plugin",
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
        *nb_elements_out = sizeof(g_filesrc_elements) / sizeof(g_filesrc_elements[0]);
    }
    return g_filesrc_elements;
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
