/*=============================================================================
    zst_builtins.c — Built-in element registration for the Element Public API

    Provides the strong implementation of zst_register_builtin_elements() that
    lives in libzstreamer-elements.  Each element constructor is referenced
    via a direct (strong) extern declaration — no weak symbols — so the
    linker naturally pulls in the element .o files from the static archive,
    and --as-needed keeps libzstreamer-elements.so in the NEETED list.

    Because this function is ONLY defined here (not in the core library),
    any app that calls zst_register_builtin_elements() creates a strong
    reference chain:

        app → zst_register_builtin_elements (this file)
            → zst_video_test_src_create (direct call, strong ref)

    This ensures both static and shared linking work without
    --whole-archive or --no-as-needed flags.
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include "zst_plugin.h"
#include "zst_element_factory.h"
#include <string.h>
#include <stdlib.h>

/*──────────────────────────────────────────────────────────────────────────
  Strong (non-weak) forward declarations for every element constructor in
  libzstreamer-elements.  These force the linker to include the element
  object files when zst_register_builtin_elements() is reachable.
──────────────────────────────────────────────────────────────────────────*/
zst_element_t* zst_queue_element_create(const char* name);

zst_element_t* zst_file_source_create(const char* path);
zst_element_t* zst_file_sink_create(const char* path);
zst_element_t* zst_fake_sink_create(void);
zst_element_t* zst_v4l2_source_create(void);
zst_element_t* zst_alsa_source_create(void);
zst_element_t* zst_h264_encoder_create(void);
zst_element_t* zst_h264_decoder_create(void);
zst_element_t* zst_h265_encoder_create(void);
zst_element_t* zst_h265_decoder_create(void);
zst_element_t* zst_aac_encoder_create(void);
zst_element_t* zst_aac_decoder_create(void);
zst_element_t* zst_mp4_muxer_create(void);
zst_element_t* zst_video_scaler_create(int target_width, int target_height, const char* target_pixel_format);
zst_element_t* zst_audio_resampler_create(int target_sample_rate, int target_channels, const char* target_format);
zst_element_t* zst_video_test_src_create(void);
zst_element_t* zst_audio_test_src_create(void);
zst_element_t* zst_text_overlay_create(const char* text);
zst_element_t* zst_text_source_create(void);
zst_element_t* zst_srt_parser_create(const char* path);
zst_element_t* zst_net_source_create(void);
zst_element_t* zst_net_sink_create(void);
zst_element_t* zst_rtsp_source_create(const char* url);
zst_element_t* zst_rtsp_sink_create(void);
zst_element_t* zst_rtsp_server_create(void);
zst_element_t* zst_rtmp_source_create(void);
zst_element_t* zst_rtmp_sink_create(void);

/*──────────────────────────────────────────────────────────────────────────
  Pad template tables (used by descriptor tables below).
──────────────────────────────────────────────────────────────────────────*/
static const zst_pad_template_t g_pad_src[]          = { { "src", ZST_PAD_SRC, "ANY" } };
static const zst_pad_template_t g_pad_sink[]         = { { "sink", ZST_PAD_SINK, "ANY" } };
static const zst_pad_template_t g_pad_filter[]       = {
    { "sink", ZST_PAD_SINK, "ANY" }, { "src", ZST_PAD_SRC, "ANY" }
};
static const zst_pad_template_t g_pad_mp4mux[]       = {
    { "video", ZST_PAD_SINK, "ANY" }, { "audio", ZST_PAD_SINK, "ANY" }, { "src", ZST_PAD_SRC, "ANY" }
};
static const zst_pad_template_t g_pad_textoverlay[]  = {
    { "sink", ZST_PAD_SINK, "ANY" }, { "text", ZST_PAD_SINK, "ANY" }, { "src", ZST_PAD_SRC, "ANY" }
};
static const zst_pad_template_t g_pad_rtsp_src[]     = {
    { "video", ZST_PAD_SRC, "ANY" }, { "audio", ZST_PAD_SRC, "ANY" }
};
static const zst_pad_template_t g_pad_rtsp_sink[]    = {
    { "video", ZST_PAD_SINK, "ANY" }, { "audio", ZST_PAD_SINK, "ANY" }
};
static const zst_pad_template_t g_pad_rtsp_server[]  = {
    { "video_%u", ZST_PAD_SINK, "ANY" }, { "audio_%u", ZST_PAD_SINK, "ANY" }
};

static const zst_pad_template_t g_pad_rtmp_src[]     = {
    { "video", ZST_PAD_SRC, "ANY" },
    { "audio", ZST_PAD_SRC, "ANY" }
};

static const zst_pad_template_t g_pad_rtmp_sink[]    = {
    { "video", ZST_PAD_SINK, "ANY" },
    { "audio", ZST_PAD_SINK, "ANY" }
};

/*──────────────────────────────────────────────────────────────────────────
  Property spec tables (for elements that expose typed properties).
──────────────────────────────────────────────────────────────────────────*/
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

/*──────────────────────────────────────────────────────────────────────────
  create_element callback — constructs an element by name using direct
  constructor calls (no weak symbols).
──────────────────────────────────────────────────────────────────────────*/
static zst_element_t*
create_builtin_element(const char* name)
{
    if (!name) return NULL;
    if (strcmp(name, "queue") == 0)        return zst_queue_element_create(NULL);
    if (strcmp(name, "filesrc") == 0)      return zst_file_source_create("");
    if (strcmp(name, "filesink") == 0)     return zst_file_sink_create("");
    if (strcmp(name, "fakesink") == 0)     return zst_fake_sink_create();
    if (strcmp(name, "v4l2src") == 0)      return zst_v4l2_source_create();
    if (strcmp(name, "alsasrc") == 0)      return zst_alsa_source_create();
    if (strcmp(name, "h264enc") == 0)      return zst_h264_encoder_create();
    if (strcmp(name, "h264dec") == 0)      return zst_h264_decoder_create();
    if (strcmp(name, "h265enc") == 0)      return zst_h265_encoder_create();
    if (strcmp(name, "h265dec") == 0)      return zst_h265_decoder_create();
    if (strcmp(name, "aacenc") == 0)       return zst_aac_encoder_create();
    if (strcmp(name, "aacdec") == 0)       return zst_aac_decoder_create();
    if (strcmp(name, "mp4mux") == 0)       return zst_mp4_muxer_create();
    if (strcmp(name, "videoscaler") == 0)  return zst_video_scaler_create(0, 0, NULL);
    if (strcmp(name, "audioresampler") == 0) return zst_audio_resampler_create(0, 0, NULL);
    if (strcmp(name, "videotestsrc") == 0) return zst_video_test_src_create();
    if (strcmp(name, "audiotestsrc") == 0) return zst_audio_test_src_create();
    if (strcmp(name, "textoverlay") == 0)  return zst_text_overlay_create(NULL);
    if (strcmp(name, "textsource") == 0)   return zst_text_source_create();
    if (strcmp(name, "srt_parser") == 0)   return zst_srt_parser_create(NULL);
    if (strcmp(name, "netsrc") == 0)       return zst_net_source_create();
    if (strcmp(name, "netsink") == 0)      return zst_net_sink_create();
    if (strcmp(name, "rtspsrc") == 0)      return zst_rtsp_source_create(NULL);
    if (strcmp(name, "rtspsink") == 0)     return zst_rtsp_sink_create();
    if (strcmp(name, "rtsp_server") == 0)  return zst_rtsp_server_create();
    if (strcmp(name, "rtmpsrc") == 0)      return zst_rtmp_source_create();
    if (strcmp(name, "rtmpsink") == 0)     return zst_rtmp_sink_create();
    return NULL;
}

/*──────────────────────────────────────────────────────────────────────────
  Descriptor tables (one per element).
──────────────────────────────────────────────────────────────────────────*/
#define DESC(name, longname, category, desc, props, nprops, pads) \
    { name, longname, category, desc, "zstreamer", props, nprops, pads, sizeof(pads) / sizeof((pads)[0]), NULL }

static const zst_element_desc_t g_builtin_descs[] = {
    DESC("queue",   "Queue",            "Generic",      "Thread-safe buffering element",                                                                                        NULL,                           0, g_pad_filter),
    DESC("filesrc", "File Source",      "Source/File",  "Reads buffers from a local file",                                                                                      g_builtin_filesrc_props,        sizeof(g_builtin_filesrc_props) / sizeof(g_builtin_filesrc_props[0]), g_pad_src),
    DESC("filesink", "File Sink",       "Sink/File",    "Writes incoming buffers to a local file",                                                                              g_builtin_filesink_props,       sizeof(g_builtin_filesink_props) / sizeof(g_builtin_filesink_props[0]), g_pad_sink),
    DESC("fakesink", "Fake Sink",       "Sink/Test",    "Consumes buffers and records simple statistics",                                                                       g_builtin_fakesink_props,       sizeof(g_builtin_fakesink_props) / sizeof(g_builtin_fakesink_props[0]), g_pad_sink),
    DESC("v4l2src", "V4L2 Source",      "Source/Video", "Captures video from a V4L2 device",                                                                                    NULL,                           0, g_pad_src),
    DESC("alsasrc", "ALSA Source",      "Source/Audio", "Captures audio from ALSA",                                                                                             NULL,                           0, g_pad_src),
    DESC("h264enc", "H.264 Encoder",    "Codec/Encoder","Encodes raw video to H.264",                                                                                           NULL,                           0, g_pad_filter),
    DESC("h264dec", "H.264 Decoder",    "Codec/Decoder","Decodes H.264 video frames",                                                                                           NULL,                           0, g_pad_filter),
    DESC("h265enc", "H.265 Encoder",    "Codec/Encoder","Encodes raw video to H.265",                                                                                           NULL,                           0, g_pad_filter),
    DESC("h265dec", "H.265 Decoder",    "Codec/Decoder","Decodes H.265 video frames",                                                                                           NULL,                           0, g_pad_filter),
    DESC("aacenc",  "AAC Encoder",      "Codec/Encoder","Encodes raw audio to AAC",                                                                                             NULL,                           0, g_pad_filter),
    DESC("aacdec",  "AAC Decoder",      "Codec/Decoder","Decodes AAC audio frames",                                                                                             NULL,                           0, g_pad_filter),
    DESC("mp4mux",  "MP4 Muxer",        "Muxer/File",   "Muxes encoded audio/video into MP4",                                                                                  NULL,                           0, g_pad_mp4mux),
    DESC("videoscaler", "Video Scaler", "Filter/Video", "Converts video resolution or pixel format",                                                                            NULL,                           0, g_pad_filter),
    DESC("audioresampler", "Audio Resampler", "Filter/Audio", "Converts audio sample rate, channels, or format",                                                                NULL,                           0, g_pad_filter),
    DESC("videotestsrc", "Video Test Source", "Source/Test", "Generates synthetic video test patterns",                                                                         NULL,                           0, g_pad_src),
    DESC("audiotestsrc", "Audio Test Source", "Source/Test", "Generates synthetic audio test signals",                                                                          NULL,                           0, g_pad_src),
    DESC("textoverlay", "Text Overlay", "Filter/Video", "Overlays text on video frames",                                                                                        NULL,                           0, g_pad_textoverlay),
    DESC("textsource", "Text Source",   "Source/Video", "Generates video frames containing text",                                                                                NULL,                           0, g_pad_src),
    DESC("srt_parser", "SRT Parser",    "Parser/Text",  "Parses SubRip subtitle data",                                                                                          NULL,                           0, g_pad_src),
    DESC("netsrc",  "Network Source",   "Source/Network","Receives buffers from TCP/UDP or Unix sockets",                                                                       NULL,                           0, g_pad_src),
    DESC("netsink", "Network Sink",     "Sink/Network", "Sends buffers to TCP/UDP or Unix sockets",                                                                             NULL,                           0, g_pad_sink),
    DESC("rtspsrc", "RTSP Source",      "Source/Network","Receives audio/video from an RTSP endpoint",                                                                          NULL,                           0, g_pad_rtsp_src),
    DESC("rtspsink", "RTSP Sink",       "Sink/Network", "Publishes audio/video to an RTSP endpoint",                                                                             NULL,                           0, g_pad_rtsp_sink),
    DESC("rtsp_server", "RTSP Server",  "Sink/Network", "Serves RTP streams over RTSP",                                                                                         NULL,                           0, g_pad_rtsp_server),
    DESC("rtmpsrc",  "RTMP Source",      "Source/Network","Receives audio/video from an RTMP endpoint",                                                                          NULL,                           0, g_pad_rtmp_src),
    DESC("rtmpsink", "RTMP Sink",        "Sink/Network", "Publishes audio/video to an RTMP endpoint",                                                                             NULL,                           0, g_pad_rtmp_sink)
};

/*──────────────────────────────────────────────────────────────────────────
  zst_register_builtin_elements — register every built-in element with the
  factory.  Only exists in libzstreamer-elements (not in core), so any
  caller creates a strong link dependency on the elements library.
──────────────────────────────────────────────────────────────────────────*/
zst_result_t
zst_register_builtin_elements(void)
{
    zst_result_t ret;

    ret = zst_plugin_registry_init();
    if (ret != ZST_OK) return ret;

    ret = zst_plugin_registry_add_entry(
        g_builtin_descs,
        sizeof(g_builtin_descs) / sizeof(g_builtin_descs[0]),
        create_builtin_element);

    return ret;
}
