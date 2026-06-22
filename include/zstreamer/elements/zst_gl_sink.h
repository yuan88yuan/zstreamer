/*=============================================================================
    zst_gl_sink.h — OpenGL display sink convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_GL_SINK_FACTORY                "glsink"
#define ZST_GL_SINK_PROP_WINDOW_TITLE      "window-title"
#define ZST_GL_SINK_PROP_WIDTH             "width"
#define ZST_GL_SINK_PROP_HEIGHT            "height"
#define ZST_GL_SINK_PROP_FULLSCREEN        "fullscreen"
#define ZST_GL_SINK_PROP_VSYNC             "vsync"
#define ZST_GL_SINK_PROP_SCALING           "scaling"
#define ZST_GL_SINK_PROP_MAX_LATENESS      "max-lateness"
#define ZST_GL_SINK_PROP_COLOR_MATRIX      "color-matrix"
#define ZST_GL_SINK_PROP_BRIGHTNESS        "brightness"
#define ZST_GL_SINK_PROP_CONTRAST          "contrast"
#define ZST_GL_SINK_PROP_SATURATION        "saturation"

typedef struct {
    size_t struct_size;
    const char* window_title;
    uint32_t width;
    uint32_t height;
    int fullscreen;
    int vsync;
    const char* scaling;
    int64_t max_lateness;
    const char* color_matrix;
    double brightness;
    double contrast;
    double saturation;
} zst_gl_sink_config_t;

zst_element_t* zst_gl_sink_create(void);
zst_element_t* zst_gl_sink_create_with_config(const zst_gl_sink_config_t* config);

#ifdef __cplusplus
}
#endif
