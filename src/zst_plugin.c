/*=============================================================================
    zst_plugin.c — dlopen-based dynamic plugin loader and registry
=============================================================================*/

#define _POSIX_C_SOURCE 200809L  /* strdup */

#include "zst_plugin.h"
#include "zst_queue.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <dirent.h>
#include <stdio.h>

#ifdef _WIN32
#  include <windows.h>
#  define DLOPEN(f)   (void*)LoadLibraryA(f)
#  define DLSYM(h, n) (void*)GetProcAddress((HMODULE)h, n)
#  define DLCLOSE(h)   FreeLibrary((HMODULE)h)
#else
#  include <dlfcn.h>
#  define DLOPEN(f)   dlopen(f, RTLD_LAZY | RTLD_LOCAL)
#  define DLSYM(h, n) dlsym(h, n)
#  define DLCLOSE(h)   dlclose(h)
#endif

typedef struct zst_registry_entry {
    zst_plugin_t*              plugin;
    char*                      path;
    const zst_element_desc_t*  elements;
    uint32_t                   nb_elements;
    zst_create_element_fn      create_element;
    int                        is_builtin;
    struct zst_registry_entry* next;
} zst_registry_entry_t;

static struct {
    zst_registry_entry_t* head;
    pthread_mutex_t      lock;
    int                  initialized;
    int                  builtins_registered;
} g_registry = {
    .head = NULL,
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .initialized = 0,
    .builtins_registered = 0
};

#if defined(__GNUC__)
#define ZST_WEAK __attribute__((weak))
#else
#define ZST_WEAK
#endif

/* Optional official element constructors.  When an application links any of
 * these element objects directly, zst_register_builtin_elements() exposes them
 * through the same factory/introspection registry used by dynamic plugins. */
extern zst_element_t* zst_file_source_create(const char* path) ZST_WEAK;
extern zst_element_t* zst_file_sink_create(const char* path) ZST_WEAK;
extern zst_element_t* zst_fake_sink_create(void) ZST_WEAK;
extern zst_element_t* zst_v4l2_source_create(void) ZST_WEAK;
extern zst_element_t* zst_alsa_source_create(void) ZST_WEAK;
extern zst_element_t* zst_h264_encoder_create(void) ZST_WEAK;
extern zst_element_t* zst_h264_decoder_create(void) ZST_WEAK;
extern zst_element_t* zst_h265_encoder_create(void) ZST_WEAK;
extern zst_element_t* zst_h265_decoder_create(void) ZST_WEAK;
extern zst_element_t* zst_aac_encoder_create(void) ZST_WEAK;
extern zst_element_t* zst_aac_decoder_create(void) ZST_WEAK;
extern zst_element_t* zst_mp4_muxer_create(void) ZST_WEAK;
extern zst_element_t* zst_video_scaler_create(int target_width, int target_height, const char* target_pixel_format) ZST_WEAK;
extern zst_element_t* zst_audio_resampler_create(int target_sample_rate, int target_channels, const char* target_format) ZST_WEAK;
extern zst_element_t* zst_video_test_src_create(void) ZST_WEAK;
extern zst_element_t* zst_audio_test_src_create(void) ZST_WEAK;
extern zst_element_t* zst_text_overlay_create(const char* text) ZST_WEAK;
extern zst_element_t* zst_text_source_create(void) ZST_WEAK;
extern zst_element_t* zst_srt_parser_create(const char* path) ZST_WEAK;
extern zst_element_t* zst_net_source_create(void) ZST_WEAK;
extern zst_element_t* zst_net_sink_create(void) ZST_WEAK;
extern zst_element_t* zst_rtsp_source_create(const char* url) ZST_WEAK;
extern zst_element_t* zst_rtsp_sink_create(void) ZST_WEAK;
extern zst_element_t* zst_rtsp_server_create(void) ZST_WEAK;

static const zst_property_spec_t g_builtin_filesrc_props[] = {
    { "path", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Input file path" },
    { "chunk-size", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "4096", "Maximum bytes to read per buffer" },
    { "loop", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "false", "Loop back to the start at EOF" },
    { "offset", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "0", "Initial byte offset" },
    { "length", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "-1", "Maximum number of bytes to read; -1 means unlimited" }
};

static const zst_property_spec_t g_builtin_filesink_props[] = {
    { "path", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Output file path" },
    { "location", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Alias for path" }
};

static const zst_property_spec_t g_builtin_fakesink_props[] = {
    { "drop-probability", ZST_PROPERTY_DOUBLE, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "0.0", "Probability in [0.0, 1.0] of dropping a buffer without counting it" },
    { "total-buffers", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Number of buffers received since open" },
    { "total-bytes", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Number of bytes received since open" }
};

static const zst_pad_template_t g_pad_src[] = { { "src", ZST_PAD_SRC, "ANY" } };
static const zst_pad_template_t g_pad_sink[] = { { "sink", ZST_PAD_SINK, "ANY" } };
static const zst_pad_template_t g_pad_filter[] = {
    { "sink", ZST_PAD_SINK, "ANY" }, { "src", ZST_PAD_SRC, "ANY" }
};
static const zst_pad_template_t g_pad_mp4mux[] = {
    { "video", ZST_PAD_SINK, "ANY" }, { "audio", ZST_PAD_SINK, "ANY" }, { "src", ZST_PAD_SRC, "ANY" }
};
static const zst_pad_template_t g_pad_textoverlay[] = {
    { "sink", ZST_PAD_SINK, "ANY" }, { "text", ZST_PAD_SINK, "ANY" }, { "src", ZST_PAD_SRC, "ANY" }
};
static const zst_pad_template_t g_pad_rtsp_src[] = {
    { "video", ZST_PAD_SRC, "ANY" }, { "audio", ZST_PAD_SRC, "ANY" }
};
static const zst_pad_template_t g_pad_rtsp_sink[] = {
    { "video", ZST_PAD_SINK, "ANY" }, { "audio", ZST_PAD_SINK, "ANY" }
};
static const zst_pad_template_t g_pad_rtsp_server[] = {
    { "video_%u", ZST_PAD_SINK, "ANY" }, { "audio_%u", ZST_PAD_SINK, "ANY" }
};

#define ZST_BUILTIN_DESC(NAME, LONG_NAME, CATEGORY, DESCRIPTION, PROPS, NPROPS, PADS) \
    { NAME, LONG_NAME, CATEGORY, DESCRIPTION, "zstreamer", PROPS, NPROPS, PADS, sizeof(PADS) / sizeof((PADS)[0]), NULL }

static const zst_element_desc_t g_builtin_descs[] = {
    ZST_BUILTIN_DESC("queue", "Queue", "Generic", "Thread-safe buffering element", NULL, 0, g_pad_filter),
    ZST_BUILTIN_DESC("filesrc", "File Source", "Source/File", "Reads buffers from a local file", g_builtin_filesrc_props, sizeof(g_builtin_filesrc_props) / sizeof(g_builtin_filesrc_props[0]), g_pad_src),
    ZST_BUILTIN_DESC("filesink", "File Sink", "Sink/File", "Writes incoming buffers to a local file", g_builtin_filesink_props, sizeof(g_builtin_filesink_props) / sizeof(g_builtin_filesink_props[0]), g_pad_sink),
    ZST_BUILTIN_DESC("fakesink", "Fake Sink", "Sink/Test", "Consumes buffers and records simple statistics", g_builtin_fakesink_props, sizeof(g_builtin_fakesink_props) / sizeof(g_builtin_fakesink_props[0]), g_pad_sink),
    ZST_BUILTIN_DESC("v4l2src", "V4L2 Source", "Source/Video", "Captures video from a V4L2 device", NULL, 0, g_pad_src),
    ZST_BUILTIN_DESC("alsasrc", "ALSA Source", "Source/Audio", "Captures audio from ALSA", NULL, 0, g_pad_src),
    ZST_BUILTIN_DESC("h264enc", "H.264 Encoder", "Codec/Encoder", "Encodes raw video to H.264", NULL, 0, g_pad_filter),
    ZST_BUILTIN_DESC("h264dec", "H.264 Decoder", "Codec/Decoder", "Decodes H.264 video frames", NULL, 0, g_pad_filter),
    ZST_BUILTIN_DESC("h265enc", "H.265 Encoder", "Codec/Encoder", "Encodes raw video to H.265", NULL, 0, g_pad_filter),
    ZST_BUILTIN_DESC("h265dec", "H.265 Decoder", "Codec/Decoder", "Decodes H.265 video frames", NULL, 0, g_pad_filter),
    ZST_BUILTIN_DESC("aacenc", "AAC Encoder", "Codec/Encoder", "Encodes raw audio to AAC", NULL, 0, g_pad_filter),
    ZST_BUILTIN_DESC("aacdec", "AAC Decoder", "Codec/Decoder", "Decodes AAC audio frames", NULL, 0, g_pad_filter),
    ZST_BUILTIN_DESC("mp4mux", "MP4 Muxer", "Muxer/File", "Muxes encoded audio/video into MP4", NULL, 0, g_pad_mp4mux),
    ZST_BUILTIN_DESC("videoscaler", "Video Scaler", "Filter/Video", "Converts video resolution or pixel format", NULL, 0, g_pad_filter),
    ZST_BUILTIN_DESC("audioresampler", "Audio Resampler", "Filter/Audio", "Converts audio sample rate, channels, or format", NULL, 0, g_pad_filter),
    ZST_BUILTIN_DESC("videotestsrc", "Video Test Source", "Source/Test", "Generates synthetic video test patterns", NULL, 0, g_pad_src),
    ZST_BUILTIN_DESC("audiotestsrc", "Audio Test Source", "Source/Test", "Generates synthetic audio test signals", NULL, 0, g_pad_src),
    ZST_BUILTIN_DESC("textoverlay", "Text Overlay", "Filter/Video", "Overlays text on video frames", NULL, 0, g_pad_textoverlay),
    ZST_BUILTIN_DESC("textsource", "Text Source", "Source/Video", "Generates video frames containing text", NULL, 0, g_pad_src),
    ZST_BUILTIN_DESC("srt_parser", "SRT Parser", "Parser/Text", "Parses SubRip subtitle data", NULL, 0, g_pad_src),
    ZST_BUILTIN_DESC("netsrc", "Network Source", "Source/Network", "Receives buffers from TCP/UDP or Unix sockets", NULL, 0, g_pad_src),
    ZST_BUILTIN_DESC("netsink", "Network Sink", "Sink/Network", "Sends buffers to TCP/UDP or Unix sockets", NULL, 0, g_pad_sink),
    ZST_BUILTIN_DESC("rtspsrc", "RTSP Source", "Source/Network", "Receives audio/video from an RTSP endpoint", NULL, 0, g_pad_rtsp_src),
    ZST_BUILTIN_DESC("rtspsink", "RTSP Sink", "Sink/Network", "Publishes audio/video to an RTSP endpoint", NULL, 0, g_pad_rtsp_sink),
    ZST_BUILTIN_DESC("rtsp_server", "RTSP Server", "Sink/Network", "Serves RTP streams over RTSP", NULL, 0, g_pad_rtsp_server)
};

static int
builtin_constructor_available(const char* name)
{
    if (strcmp(name, "queue") == 0) return 1;
    if (strcmp(name, "filesrc") == 0) return zst_file_source_create != NULL;
    if (strcmp(name, "filesink") == 0) return zst_file_sink_create != NULL;
    if (strcmp(name, "fakesink") == 0) return zst_fake_sink_create != NULL;
    if (strcmp(name, "v4l2src") == 0) return zst_v4l2_source_create != NULL;
    if (strcmp(name, "alsasrc") == 0) return zst_alsa_source_create != NULL;
    if (strcmp(name, "h264enc") == 0) return zst_h264_encoder_create != NULL;
    if (strcmp(name, "h264dec") == 0) return zst_h264_decoder_create != NULL;
    if (strcmp(name, "h265enc") == 0) return zst_h265_encoder_create != NULL;
    if (strcmp(name, "h265dec") == 0) return zst_h265_decoder_create != NULL;
    if (strcmp(name, "aacenc") == 0) return zst_aac_encoder_create != NULL;
    if (strcmp(name, "aacdec") == 0) return zst_aac_decoder_create != NULL;
    if (strcmp(name, "mp4mux") == 0) return zst_mp4_muxer_create != NULL;
    if (strcmp(name, "videoscaler") == 0) return zst_video_scaler_create != NULL;
    if (strcmp(name, "audioresampler") == 0) return zst_audio_resampler_create != NULL;
    if (strcmp(name, "videotestsrc") == 0) return zst_video_test_src_create != NULL;
    if (strcmp(name, "audiotestsrc") == 0) return zst_audio_test_src_create != NULL;
    if (strcmp(name, "textoverlay") == 0) return zst_text_overlay_create != NULL;
    if (strcmp(name, "textsource") == 0) return zst_text_source_create != NULL;
    if (strcmp(name, "srt_parser") == 0) return zst_srt_parser_create != NULL;
    if (strcmp(name, "netsrc") == 0) return zst_net_source_create != NULL;
    if (strcmp(name, "netsink") == 0) return zst_net_sink_create != NULL;
    if (strcmp(name, "rtspsrc") == 0) return zst_rtsp_source_create != NULL;
    if (strcmp(name, "rtspsink") == 0) return zst_rtsp_sink_create != NULL;
    if (strcmp(name, "rtsp_server") == 0) return zst_rtsp_server_create != NULL;
    return 0;
}

static zst_element_t*
builtin_create_element(const char* name)
{
    if (!name) return NULL;
    if (strcmp(name, "queue") == 0) return zst_queue_element_create(NULL);
    if (strcmp(name, "filesrc") == 0 && zst_file_source_create) return zst_file_source_create("");
    if (strcmp(name, "filesink") == 0 && zst_file_sink_create) return zst_file_sink_create("");
    if (strcmp(name, "fakesink") == 0 && zst_fake_sink_create) return zst_fake_sink_create();
    if (strcmp(name, "v4l2src") == 0 && zst_v4l2_source_create) return zst_v4l2_source_create();
    if (strcmp(name, "alsasrc") == 0 && zst_alsa_source_create) return zst_alsa_source_create();
    if (strcmp(name, "h264enc") == 0 && zst_h264_encoder_create) return zst_h264_encoder_create();
    if (strcmp(name, "h264dec") == 0 && zst_h264_decoder_create) return zst_h264_decoder_create();
    if (strcmp(name, "h265enc") == 0 && zst_h265_encoder_create) return zst_h265_encoder_create();
    if (strcmp(name, "h265dec") == 0 && zst_h265_decoder_create) return zst_h265_decoder_create();
    if (strcmp(name, "aacenc") == 0 && zst_aac_encoder_create) return zst_aac_encoder_create();
    if (strcmp(name, "aacdec") == 0 && zst_aac_decoder_create) return zst_aac_decoder_create();
    if (strcmp(name, "mp4mux") == 0 && zst_mp4_muxer_create) return zst_mp4_muxer_create();
    if (strcmp(name, "videoscaler") == 0 && zst_video_scaler_create) return zst_video_scaler_create(0, 0, NULL);
    if (strcmp(name, "audioresampler") == 0 && zst_audio_resampler_create) return zst_audio_resampler_create(0, 0, NULL);
    if (strcmp(name, "videotestsrc") == 0 && zst_video_test_src_create) return zst_video_test_src_create();
    if (strcmp(name, "audiotestsrc") == 0 && zst_audio_test_src_create) return zst_audio_test_src_create();
    if (strcmp(name, "textoverlay") == 0 && zst_text_overlay_create) return zst_text_overlay_create(NULL);
    if (strcmp(name, "textsource") == 0 && zst_text_source_create) return zst_text_source_create();
    if (strcmp(name, "srt_parser") == 0 && zst_srt_parser_create) return zst_srt_parser_create(NULL);
    if (strcmp(name, "netsrc") == 0 && zst_net_source_create) return zst_net_source_create();
    if (strcmp(name, "netsink") == 0 && zst_net_sink_create) return zst_net_sink_create();
    if (strcmp(name, "rtspsrc") == 0 && zst_rtsp_source_create) return zst_rtsp_source_create(NULL);
    if (strcmp(name, "rtspsink") == 0 && zst_rtsp_sink_create) return zst_rtsp_sink_create();
    if (strcmp(name, "rtsp_server") == 0 && zst_rtsp_server_create) return zst_rtsp_server_create();
    return NULL;
}

/* suppress -Wpedantic warning for dlsym cast to function pointer */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"

zst_plugin_t*
zst_plugin_load(const char* path)
{
    if (!path) return NULL;

    void* handle = DLOPEN(path);
    if (!handle) return NULL;

    zst_get_plugin_fn get_plugin = (zst_get_plugin_fn)DLSYM(handle, "zst_get_plugin");
    if (!get_plugin) {
        DLCLOSE(handle);
        return NULL;
    }

    zst_plugin_t* plugin = get_plugin();
    if (!plugin) {
        DLCLOSE(handle);
        return NULL;
    }

    plugin->refcount = 1;

    /* Call plugin init if provided */
    if (plugin->desc.init)
        plugin->desc.init();

    plugin->priv = (void*)handle;
    return plugin;
}

#pragma GCC diagnostic pop

void
zst_plugin_unload(zst_plugin_t* plugin)
{
    if (!plugin) return;

    if (plugin->desc.deinit)
        plugin->desc.deinit();

    if (plugin->priv)
        DLCLOSE(plugin->priv);

    free(plugin);
}

zst_plugin_t*
zst_plugin_ref(zst_plugin_t* plugin)
{
    if (!plugin) return NULL;
    __sync_fetch_and_add(&plugin->refcount, 1);
    return plugin;
}

void
zst_plugin_unref(zst_plugin_t* plugin)
{
    if (!plugin) return;
    if (__sync_sub_and_fetch(&plugin->refcount, 1) <= 0) {
        zst_plugin_unload(plugin);
    }
}

zst_result_t
zst_plugin_registry_init(void)
{
    pthread_mutex_lock(&g_registry.lock);
    if (g_registry.initialized) {
        pthread_mutex_unlock(&g_registry.lock);
        return ZST_OK;
    }
    g_registry.head = NULL;
    g_registry.initialized = 1;
    pthread_mutex_unlock(&g_registry.lock);
    return ZST_OK;
}

void
zst_plugin_registry_deinit(void)
{
    pthread_mutex_lock(&g_registry.lock);
    if (!g_registry.initialized) {
        pthread_mutex_unlock(&g_registry.lock);
        return;
    }
    zst_registry_entry_t* curr = g_registry.head;
    while (curr) {
        zst_registry_entry_t* next = curr->next;
        if (curr->plugin)
            zst_plugin_unref(curr->plugin);
        free(curr->path);
        free(curr);
        curr = next;
    }
    g_registry.head = NULL;
    g_registry.initialized = 0;
    g_registry.builtins_registered = 0;
    pthread_mutex_unlock(&g_registry.lock);
}

static const zst_element_desc_t*
plugin_query_elements(zst_plugin_t* plugin, uint32_t* nb_elements_out)
{
    if (nb_elements_out) *nb_elements_out = 0;
    if (!plugin || !plugin->priv) return NULL;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
    zst_get_plugin_elements_fn get_elements =
        (zst_get_plugin_elements_fn)DLSYM(plugin->priv, "zst_get_plugin_elements");
#pragma GCC diagnostic pop
    if (!get_elements) return NULL;

    return get_elements(nb_elements_out);
}

zst_result_t
zst_plugin_registry_scan(const char* directory)
{
    if (!directory) return ZST_ERROR;

    DIR* dir = opendir(directory);
    if (!dir) return ZST_ERROR;

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len > 3 && strcmp(entry->d_name + len - 3, ".so") == 0) {
            char path[1024];
            snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);

            /* Check if already loaded in registry */
            pthread_mutex_lock(&g_registry.lock);
            zst_registry_entry_t* curr = g_registry.head;
            int already_loaded = 0;
            while (curr) {
                if (curr->path && strcmp(curr->path, path) == 0) {
                    already_loaded = 1;
                    break;
                }
                curr = curr->next;
            }
            pthread_mutex_unlock(&g_registry.lock);

            if (already_loaded) continue;

            zst_plugin_t* plugin = zst_plugin_load(path);
            if (plugin) {
                zst_registry_entry_t* node = malloc(sizeof(*node));
                if (node) {
                    node->plugin = plugin;
                    node->path = strdup(path);
                    node->elements = plugin_query_elements(plugin, &node->nb_elements);
                    node->create_element = plugin->create_element;
                    node->is_builtin = 0;
                    
                    pthread_mutex_lock(&g_registry.lock);
                    node->next = g_registry.head;
                    g_registry.head = node;
                    pthread_mutex_unlock(&g_registry.lock);
                } else {
                    zst_plugin_unref(plugin);
                }
            }
        }
    }

    closedir(dir);
    return ZST_OK;
}

zst_result_t
zst_plugin_registry_scan_env(void)
{
    const char* env = getenv("ZSTREAMER_PLUGIN_PATH");
    if (env) {
        char* env_copy = strdup(env);
        if (!env_copy) return ZST_ERROR;

        char* token = strtok(env_copy, ":");
        while (token) {
            zst_plugin_registry_scan(token);
            token = strtok(NULL, ":");
        }

        free(env_copy);
    }
#ifdef ZSTREAMER_DEFAULT_PLUGIN_DIR
    else {
        zst_plugin_registry_scan(ZSTREAMER_DEFAULT_PLUGIN_DIR);
    }
#endif
    return ZST_OK;
}

static const zst_element_desc_t*
entry_find_desc(zst_registry_entry_t* entry, const char* name)
{
    if (!entry || !name || !entry->elements) return NULL;
    for (uint32_t i = 0; i < entry->nb_elements; i++) {
        const zst_element_desc_t* desc = &entry->elements[i];
        if (desc->name && strcmp(desc->name, name) == 0) {
            return desc;
        }
    }
    return NULL;
}

zst_element_t*
zst_element_factory_make(const char* name)
{
    if (!name) return NULL;

    pthread_mutex_lock(&g_registry.lock);
    zst_registry_entry_t* curr = g_registry.head;
    while (curr) {
        const zst_element_desc_t* desc = entry_find_desc(curr, name);
        if (curr->create_element && (!curr->is_builtin || desc)) {
            zst_element_t* el = curr->create_element(name);
            if (el) {
                if (curr->plugin)
                    el->plugin = zst_plugin_ref(curr->plugin);
                el->desc = desc ? desc : entry_find_desc(curr, el->ops ? el->ops->name : name);
                pthread_mutex_unlock(&g_registry.lock);
                return el;
            }
        } else if (desc && desc->create) {
            zst_element_t* el = desc->create();
            if (el) {
                if (curr->plugin)
                    el->plugin = zst_plugin_ref(curr->plugin);
                el->desc = desc;
                pthread_mutex_unlock(&g_registry.lock);
                return el;
            }
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&g_registry.lock);
    return NULL;
}

uint32_t
zst_element_factory_list(const zst_element_desc_t*** elements_out)
{
    uint32_t count = 0;
    const zst_element_desc_t** out = NULL;

    if (!elements_out) return 0;
    *elements_out = NULL;

    pthread_mutex_lock(&g_registry.lock);
    for (zst_registry_entry_t* curr = g_registry.head; curr; curr = curr->next) {
        count += curr->nb_elements;
    }

    if (count > 0) {
        out = calloc(count, sizeof(*out));
        if (!out) {
            pthread_mutex_unlock(&g_registry.lock);
            return 0;
        }

        uint32_t idx = 0;
        for (zst_registry_entry_t* curr = g_registry.head; curr; curr = curr->next) {
            if (!curr->elements) continue;
            for (uint32_t i = 0; i < curr->nb_elements; i++) {
                out[idx++] = &curr->elements[i];
            }
        }
    }

    pthread_mutex_unlock(&g_registry.lock);
    *elements_out = out;
    return count;
}

void
zst_element_factory_list_free(const zst_element_desc_t** elements)
{
    free((void*)elements);
}

const zst_element_desc_t*
zst_element_factory_get_desc(const char* name)
{
    if (!name) return NULL;

    pthread_mutex_lock(&g_registry.lock);
    for (zst_registry_entry_t* curr = g_registry.head; curr; curr = curr->next) {
        const zst_element_desc_t* desc = entry_find_desc(curr, name);
        if (desc) {
            pthread_mutex_unlock(&g_registry.lock);
            return desc;
        }
    }
    pthread_mutex_unlock(&g_registry.lock);
    return NULL;
}

static zst_result_t
register_builtin_desc_locked(const zst_element_desc_t* desc)
{
    zst_registry_entry_t* node;

    if (!desc || !desc->name || !builtin_constructor_available(desc->name)) {
        return ZST_OK;
    }

    node = calloc(1, sizeof(*node));
    if (!node) return ZST_ERROR;

    node->plugin = NULL;
    node->path = NULL;
    node->elements = desc;
    node->nb_elements = 1;
    node->create_element = builtin_create_element;
    node->is_builtin = 1;
    node->next = g_registry.head;
    g_registry.head = node;
    return ZST_OK;
}

zst_result_t
zst_register_builtin_elements(void)
{
    zst_result_t ret = ZST_OK;

    zst_plugin_registry_init();

    pthread_mutex_lock(&g_registry.lock);
    if (g_registry.builtins_registered) {
        pthread_mutex_unlock(&g_registry.lock);
        return ZST_OK;
    }

    for (uint32_t i = 0; i < sizeof(g_builtin_descs) / sizeof(g_builtin_descs[0]); i++) {
        ret = register_builtin_desc_locked(&g_builtin_descs[i]);
        if (ret != ZST_OK) break;
    }

    if (ret == ZST_OK)
        g_registry.builtins_registered = 1;

    pthread_mutex_unlock(&g_registry.lock);
    return ret;
}
