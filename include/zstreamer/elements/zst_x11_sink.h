/*=============================================================================
    zst_x11_sink.h — X11 video sink element convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_X11_SINK_FACTORY            "x11sink"
#define ZST_X11_SINK_PROP_DISPLAY       "display"
#define ZST_X11_SINK_PROP_WINDOW_TITLE  "window-title"
#define ZST_X11_SINK_PROP_FRAME_COUNT   "frame-count"

typedef struct {
    size_t struct_size;
    const char* display;
    const char* window_title;
} zst_x11_sink_config_t;

zst_element_t* zst_x11_sink_create(const char* display);
zst_element_t* zst_x11_sink_create_with_config(const zst_x11_sink_config_t* config);

#ifdef __cplusplus
}
#endif
