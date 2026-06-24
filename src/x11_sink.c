/*=============================================================================
    x11_sink.c — X11 video sink element

    Displays CPU-backed raw video frames in an X11 window using Xlib.  The
    element accepts YUV420P, RGB24, and BGR24.  In headless environments it
    falls back to a null-sink mode so applications and tests can still run.
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include "zstreamer/elements/zst_x11_sink.h"
#include "zst_buffer.h"
#include "zst_caps.h"
#include "zst_element_factory.h"
#include "zst_log.h"
#include "zst_pad.h"
#include "zst_plugin.h"

#define ZST_X11_FMT_YUV420P 0u
#define ZST_X11_FMT_RGB24   2u
#define ZST_X11_FMT_BGR24   3u

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

typedef struct {
    char display_name[256];
    char window_title[256];

    Display* display;
    int screen;
    Visual* visual;
    Window window;
    GC gc;
    Atom wm_delete_message;
    XImage* ximage;
    uint8_t* image_data;
    size_t image_data_size;

    uint32_t width;
    uint32_t height;
    char pixel_format[32];

    bool configured;
    bool null_mode;
    uint64_t frame_count;
} x11_sink_t;

static uint8_t
clamp_u8(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static int
mask_shift(unsigned long mask)
{
    int shift = 0;
    if (!mask) return 0;
    while ((mask & 1UL) == 0UL) {
        mask >>= 1;
        shift++;
    }
    return shift;
}

static int
mask_bits(unsigned long mask)
{
    int bits = 0;
    if (!mask) return 0;
    while ((mask & 1UL) == 0UL) mask >>= 1;
    while (mask & 1UL) {
        bits++;
        mask >>= 1;
    }
    return bits;
}

static unsigned long
component_to_mask(uint8_t value, unsigned long mask)
{
    int bits = mask_bits(mask);
    int shift = mask_shift(mask);
    if (bits <= 0) return 0;

    unsigned long max_value = (1UL << bits) - 1UL;
    unsigned long scaled = ((unsigned long)value * max_value + 127UL) / 255UL;
    return (scaled << shift) & mask;
}

static unsigned long
rgb_to_pixel(const x11_sink_t* s, uint8_t r, uint8_t g, uint8_t b)
{
    if (!s->visual) {
        return ((unsigned long)r << 16) | ((unsigned long)g << 8) | (unsigned long)b;
    }

    return component_to_mask(r, s->visual->red_mask) |
           component_to_mask(g, s->visual->green_mask) |
           component_to_mask(b, s->visual->blue_mask);
}

static void
x11_sink_destroy_ximage(x11_sink_t* s)
{
    if (s->ximage) {
        s->ximage->data = NULL; /* image_data is owned and freed below */
        XDestroyImage(s->ximage);
        s->ximage = NULL;
    }
    free(s->image_data);
    s->image_data = NULL;
    s->image_data_size = 0;
}

static void
x11_sink_destroy_window(x11_sink_t* s)
{
    x11_sink_destroy_ximage(s);

    if (s->display && s->gc) {
        XFreeGC(s->display, s->gc);
        s->gc = 0;
    }

    if (s->display && s->window) {
        XDestroyWindow(s->display, s->window);
        s->window = 0;
    }

    s->configured = false;
}

static void
x11_sink_process_events(x11_sink_t* s)
{
    if (!s->display) return;

    while (XPending(s->display) > 0) {
        XEvent ev;
        XNextEvent(s->display, &ev);
        if (ev.type == ClientMessage &&
            (Atom)ev.xclient.data.l[0] == s->wm_delete_message) {
            x11_sink_destroy_window(s);
            s->null_mode = true;
            break;
        }
    }
}

static bool
format_from_name(const char* name, uint32_t* format_out)
{
    if (!name || !format_out) return false;
    if (strcmp(name, "YUV420P") == 0 || strcmp(name, "I420") == 0) {
        *format_out = ZST_X11_FMT_YUV420P;
        return true;
    }
    if (strcmp(name, "RGB24") == 0) {
        *format_out = ZST_X11_FMT_RGB24;
        return true;
    }
    if (strcmp(name, "BGR24") == 0) {
        *format_out = ZST_X11_FMT_BGR24;
        return true;
    }
    return false;
}

static const char*
name_from_format(uint32_t format)
{
    switch (format) {
        case ZST_X11_FMT_YUV420P: return "YUV420P";
        case ZST_X11_FMT_RGB24:   return "RGB24";
        case ZST_X11_FMT_BGR24:   return "BGR24";
        default:                  return "";
    }
}

static bool
x11_sink_get_caps_info(zst_element_t* el, uint32_t* width, uint32_t* height, uint32_t* format)
{
    zst_pad_t* sink = zst_element_get_pad(el, "sink");
    if (!sink || !sink->caps || !sink->caps->structs) return false;

    zst_caps_struct_t* c = sink->caps->structs;
    if (c->type != ZST_CAPS_VIDEO) return false;

    if (c->video.width > 0) *width = (uint32_t)c->video.width;
    if (c->video.height > 0) *height = (uint32_t)c->video.height;
    if (c->video.pixel_format[0] != '\0') {
        (void)format_from_name(c->video.pixel_format, format);
    }
    return true;
}

static bool
x11_sink_get_buffer_info(zst_element_t* el, const zst_buffer_t* in,
                         uint32_t* width, uint32_t* height, uint32_t* format)
{
    if (!in || !width || !height || !format) return false;

    if (in->type == ZST_BUFFER_VIDEO_FRAME && in->payload) {
        const zst_video_frame_t* frame = (const zst_video_frame_t*)in->payload;
        if (frame->width > 0) *width = frame->width;
        if (frame->height > 0) *height = frame->height;
        if (frame->format == ZST_X11_FMT_YUV420P ||
            frame->format == ZST_X11_FMT_RGB24 ||
            frame->format == ZST_X11_FMT_BGR24) {
            *format = frame->format;
        }
    }

    (void)x11_sink_get_caps_info(el, width, height, format);
    return *width > 0 && *height > 0;
}

static zst_result_t
x11_sink_configure(x11_sink_t* s, uint32_t width, uint32_t height, uint32_t format)
{
    if (s->null_mode) {
        s->width = width;
        s->height = height;
        snprintf(s->pixel_format, sizeof(s->pixel_format), "%s", name_from_format(format));
        s->configured = true;
        return ZST_OK;
    }

    if (!s->display) return ZST_ERROR;

    if (s->configured && s->width == width && s->height == height) {
        snprintf(s->pixel_format, sizeof(s->pixel_format), "%s", name_from_format(format));
        return ZST_OK;
    }

    x11_sink_destroy_window(s);

    s->screen = DefaultScreen(s->display);
    s->visual = DefaultVisual(s->display, s->screen);
    int depth = DefaultDepth(s->display, s->screen);

    s->window = XCreateSimpleWindow(
        s->display,
        RootWindow(s->display, s->screen),
        0, 0,
        width, height,
        0,
        BlackPixel(s->display, s->screen),
        BlackPixel(s->display, s->screen));
    if (!s->window) return ZST_ERROR;

    XStoreName(s->display, s->window, s->window_title);
    XSelectInput(s->display, s->window, ExposureMask | StructureNotifyMask);
    s->wm_delete_message = XInternAtom(s->display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(s->display, s->window, &s->wm_delete_message, 1);
    XMapWindow(s->display, s->window);

    s->gc = XCreateGC(s->display, s->window, 0, NULL);
    if (!s->gc) {
        x11_sink_destroy_window(s);
        return ZST_ERROR;
    }

    s->ximage = XCreateImage(
        s->display,
        s->visual,
        (unsigned int)depth,
        ZPixmap,
        0,
        NULL,
        width,
        height,
        32,
        0);
    if (!s->ximage) {
        x11_sink_destroy_window(s);
        return ZST_ERROR;
    }

    s->image_data_size = (size_t)s->ximage->bytes_per_line * height;
    s->image_data = calloc(1, s->image_data_size);
    if (!s->image_data) {
        x11_sink_destroy_window(s);
        return ZST_ERROR;
    }
    s->ximage->data = (char*)s->image_data;

    s->width = width;
    s->height = height;
    snprintf(s->pixel_format, sizeof(s->pixel_format), "%s", name_from_format(format));
    s->configured = true;

    XFlush(s->display);
    return ZST_OK;
}

static bool
x11_sink_get_planes(const zst_buffer_t* in, uint32_t format,
                    const uint8_t** p0, const uint8_t** p1, const uint8_t** p2,
                    int32_t* s0, int32_t* s1, int32_t* s2)
{
    if (!in || !p0 || !p1 || !p2 || !s0 || !s1 || !s2) return false;

    if (in->type == ZST_BUFFER_VIDEO_FRAME && in->payload) {
        const zst_video_frame_t* frame = (const zst_video_frame_t*)in->payload;
        *p0 = frame->plane[0];
        *p1 = frame->plane[1];
        *p2 = frame->plane[2];
        *s0 = (int32_t)frame->stride[0];
        *s1 = (int32_t)frame->stride[1];
        *s2 = (int32_t)frame->stride[2];
    } else {
        *p0 = (const uint8_t*)in->memory.data;
        *p1 = NULL;
        *p2 = NULL;
        *s0 = 0;
        *s1 = 0;
        *s2 = 0;
    }

    (void)format;
    if (!*p0) return false;
    return true;
}

static zst_result_t
x11_sink_convert_frame(x11_sink_t* s, const zst_buffer_t* in, uint32_t format)
{
    const uint8_t* p0 = NULL;
    const uint8_t* p1 = NULL;
    const uint8_t* p2 = NULL;
    int32_t stride0 = 0;
    int32_t stride1 = 0;
    int32_t stride2 = 0;

    if (!x11_sink_get_planes(in, format, &p0, &p1, &p2, &stride0, &stride1, &stride2)) {
        return ZST_ERROR_INVALID_ARGUMENT;
    }

    if (format == ZST_X11_FMT_RGB24 || format == ZST_X11_FMT_BGR24) {
        if (stride0 <= 0) stride0 = (int32_t)s->width * 3;
    } else if (format == ZST_X11_FMT_YUV420P) {
        if (stride0 <= 0) stride0 = (int32_t)s->width;
        if (stride1 <= 0) stride1 = (int32_t)s->width / 2;
        if (stride2 <= 0) stride2 = (int32_t)s->width / 2;
        if (!p1) p1 = p0 + (size_t)stride0 * s->height;
        if (!p2) p2 = p1 + (size_t)stride1 * ((s->height + 1) / 2);
    } else {
        return ZST_ERROR_INVALID_ARGUMENT;
    }

    for (uint32_t y = 0; y < s->height; ++y) {
        for (uint32_t x = 0; x < s->width; ++x) {
            uint8_t r = 0, g = 0, b = 0;

            if (format == ZST_X11_FMT_RGB24) {
                const uint8_t* px = p0 + (size_t)y * stride0 + (size_t)x * 3;
                r = px[0];
                g = px[1];
                b = px[2];
            } else if (format == ZST_X11_FMT_BGR24) {
                const uint8_t* px = p0 + (size_t)y * stride0 + (size_t)x * 3;
                b = px[0];
                g = px[1];
                r = px[2];
            } else {
                int yy = p0[(size_t)y * stride0 + x];
                int u = p1[(size_t)(y / 2) * stride1 + (x / 2)] - 128;
                int v = p2[(size_t)(y / 2) * stride2 + (x / 2)] - 128;
                r = clamp_u8(yy + ((91881 * v) >> 16));
                g = clamp_u8(yy - ((22554 * u + 46802 * v) >> 16));
                b = clamp_u8(yy + ((116130 * u) >> 16));
            }

            XPutPixel(s->ximage, (int)x, (int)y, rgb_to_pixel(s, r, g, b));
        }
    }

    return ZST_OK;
}

static zst_result_t
x11_sink_open(zst_element_t* el)
{
    x11_sink_t* s = (x11_sink_t*)el->priv;
    s->frame_count = 0;
    s->null_mode = false;

    s->display = XOpenDisplay(s->display_name[0] ? s->display_name : NULL);
    if (!s->display) {
        s->null_mode = true;
        ZST_LOG_WARN("x11sink", "no X11 display available; using null-sink mode");
    }

    return ZST_OK;
}

static zst_result_t
x11_sink_close(zst_element_t* el)
{
    x11_sink_t* s = (x11_sink_t*)el->priv;

    x11_sink_destroy_window(s);

    if (s->display) {
        XCloseDisplay(s->display);
        s->display = NULL;
    }
    s->visual = NULL;
    s->screen = 0;
    s->null_mode = false;
    return ZST_OK;
}

static zst_result_t
x11_sink_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    x11_sink_t* s = (x11_sink_t*)el->priv;
    (void)out;

    if (!in) return ZST_ERROR_INVALID_ARGUMENT;

    uint32_t width = s->width;
    uint32_t height = s->height;
    uint32_t format = ZST_X11_FMT_YUV420P;
    if (s->pixel_format[0] != '\0') {
        (void)format_from_name(s->pixel_format, &format);
    }

    if (!x11_sink_get_buffer_info(el, in, &width, &height, &format)) {
        return ZST_ERROR_INVALID_ARGUMENT;
    }

    if (x11_sink_configure(s, width, height, format) != ZST_OK) {
        return ZST_ERROR;
    }

    if (s->null_mode) {
        s->frame_count++;
        return ZST_OK;
    }

    x11_sink_process_events(s);
    if (!s->configured || s->null_mode) {
        s->frame_count++;
        return ZST_OK;
    }

    zst_result_t ret = x11_sink_convert_frame(s, in, format);
    if (ret != ZST_OK) return ret;

    XPutImage(s->display, s->window, s->gc, s->ximage, 0, 0, 0, 0, s->width, s->height);
    XFlush(s->display);

    s->frame_count++;
    return ZST_OK;
}

static zst_result_t
x11_sink_set_property(zst_element_t* el, const char* name, const char* value)
{
    x11_sink_t* s = (x11_sink_t*)el->priv;
    if (!name || !value) return ZST_ERROR_INVALID_ARGUMENT;

    if (strcmp(name, "display") == 0) {
        snprintf(s->display_name, sizeof(s->display_name), "%s", value);
        return ZST_OK;
    }
    if (strcmp(name, "window-title") == 0 || strcmp(name, "title") == 0) {
        snprintf(s->window_title, sizeof(s->window_title), "%s", value);
        if (s->display && s->window) {
            XStoreName(s->display, s->window, s->window_title);
        }
        return ZST_OK;
    }

    return ZST_ERROR;
}

static zst_result_t
x11_sink_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    x11_sink_t* s = (x11_sink_t*)el->priv;
    if (!name || !value_out || max_len == 0) return ZST_ERROR_INVALID_ARGUMENT;

    if (strcmp(name, "display") == 0) {
        snprintf(value_out, max_len, "%s", s->display_name);
        return ZST_OK;
    }
    if (strcmp(name, "window-title") == 0 || strcmp(name, "title") == 0) {
        snprintf(value_out, max_len, "%s", s->window_title);
        return ZST_OK;
    }
    if (strcmp(name, "frame-count") == 0) {
        snprintf(value_out, max_len, "%llu", (unsigned long long)s->frame_count);
        return ZST_OK;
    }
    if (strcmp(name, "mode") == 0) {
        snprintf(value_out, max_len, "%s", s->null_mode ? "null" : "x11");
        return ZST_OK;
    }

    return ZST_ERROR;
}

static zst_caps_t*
x11_sink_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)el;
    (void)pad;

    zst_caps_t* caps = zst_caps_create();
    if (!caps) return NULL;
    zst_caps_append(caps, zst_caps_struct_create_video("video/x-raw", 0, 0, 0.0, "YUV420P"));
    zst_caps_append(caps, zst_caps_struct_create_video("video/x-raw", 0, 0, 0.0, "RGB24"));
    zst_caps_append(caps, zst_caps_struct_create_video("video/x-raw", 0, 0, 0.0, "BGR24"));

    if (!filter) return caps;

    zst_caps_t* intersected = zst_caps_intersect(caps, filter);
    zst_caps_destroy(caps);
    return intersected;
}

static zst_element_ops_t g_x11_sink_ops = {
    .name = "x11sink",
    .open = x11_sink_open,
    .close = x11_sink_close,
    .process = x11_sink_process,
    .get_caps = x11_sink_get_caps,
    .set_property = x11_sink_set_property,
    .get_property = x11_sink_get_property,
};

zst_element_t*
zst_x11_sink_create(const char* display)
{
    x11_sink_t* priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;

    snprintf(priv->window_title, sizeof(priv->window_title), "zstreamer X11 Sink");
    snprintf(priv->pixel_format, sizeof(priv->pixel_format), "YUV420P");
    if (display) {
        snprintf(priv->display_name, sizeof(priv->display_name), "%s", display);
    }

    zst_element_t* el = zst_element_create(&g_x11_sink_ops, priv);
    if (!el) {
        free(priv);
        return NULL;
    }

    zst_pad_t* sink = zst_pad_create("sink", ZST_PAD_SINK);
    if (sink) {
        zst_element_add_pad(el, sink);
    }

    return el;
}

zst_element_t*
zst_x11_sink_create_with_config(const zst_x11_sink_config_t* config)
{
    if (!config || config->struct_size < sizeof(zst_x11_sink_config_t)) return NULL;

    zst_element_t* el = zst_element_factory_make("x11sink");
    if (!el) {
        el = zst_x11_sink_create(config->display);
    }
    if (!el) return NULL;

    if (config->display) {
        zst_element_set_property_string(el, "display", config->display);
    }
    if (config->window_title) {
        zst_element_set_property_string(el, "window-title", config->window_title);
    }
    return el;
}

#ifdef BUILDING_PLUGIN
static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "x11sink") == 0) {
        return zst_x11_sink_create(NULL);
    }
    return NULL;
}

static const zst_property_spec_t g_x11sink_properties[] = {
    { "display",      ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE,
      "", "X11 display name; empty uses DISPLAY" },
    { "window-title", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE,
      "zstreamer X11 Sink", "Window title" },
    { "frame-count",  ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE,
      "0", "Number of rendered or discarded frames" },
};

static const zst_pad_template_t g_x11sink_pads[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-raw" }
};

static const zst_element_desc_t g_x11sink_elements[] = {
    {
        .name = "x11sink",
        .long_name = "X11 Video Sink",
        .category = "Sink/Video",
        .description = "Displays raw video frames in an X11 window",
        .author = "zstreamer",
        .properties = g_x11sink_properties,
        .nb_properties = sizeof(g_x11sink_properties) / sizeof(g_x11sink_properties[0]),
        .pads = g_x11sink_pads,
        .nb_pads = sizeof(g_x11sink_pads) / sizeof(g_x11sink_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "x11sink_plugin",
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
        *nb_elements_out = sizeof(g_x11sink_elements) / sizeof(g_x11sink_elements[0]);
    }
    return g_x11sink_elements;
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
#endif /* BUILDING_PLUGIN */
