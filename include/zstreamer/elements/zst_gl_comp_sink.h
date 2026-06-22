/*=============================================================================
    zst_gl_comp_sink.h — OpenGL compositor display sink convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_GL_COMP_SINK_FACTORY                 "glcompsink"
#define ZST_GL_COMP_SINK_PROP_WINDOW_TITLE       "window-title"
#define ZST_GL_COMP_SINK_PROP_CANVAS_WIDTH       "canvas-width"
#define ZST_GL_COMP_SINK_PROP_CANVAS_HEIGHT      "canvas-height"
#define ZST_GL_COMP_SINK_PROP_BACKGROUND_COLOR   "background-color"
#define ZST_GL_COMP_SINK_PROP_FULLSCREEN         "fullscreen"
#define ZST_GL_COMP_SINK_PROP_VSYNC              "vsync"
#define ZST_GL_COMP_SINK_PROP_INPUT_COUNT        "input-count"
#define ZST_GL_COMP_SINK_PROP_REQUEST_PAD        "request-pad"

#define ZST_GL_COMP_SINK_PAD_PROP_X              "x"
#define ZST_GL_COMP_SINK_PAD_PROP_Y              "y"
#define ZST_GL_COMP_SINK_PAD_PROP_WIDTH          "width"
#define ZST_GL_COMP_SINK_PAD_PROP_HEIGHT         "height"
#define ZST_GL_COMP_SINK_PAD_PROP_Z_ORDER        "z-order"
#define ZST_GL_COMP_SINK_PAD_PROP_ALPHA          "alpha"
#define ZST_GL_COMP_SINK_PAD_PROP_VISIBLE        "visible"
#define ZST_GL_COMP_SINK_PAD_PROP_SCALING        "scaling"

typedef struct {
    size_t struct_size;
    const char* window_title;
    uint32_t canvas_width;
    uint32_t canvas_height;
    const char* background_color;
    int fullscreen;
    int vsync;
    uint32_t input_count;
} zst_gl_comp_sink_config_t;

zst_element_t* zst_gl_comp_sink_create(void);
zst_element_t* zst_gl_comp_sink_create_with_config(const zst_gl_comp_sink_config_t* config);

/* Request a sink pad.  If name is NULL, the next sink_%u pad is created. */
zst_pad_t* zst_gl_comp_sink_request_pad(zst_element_t* el, const char* name);

#ifdef __cplusplus
}
#endif
