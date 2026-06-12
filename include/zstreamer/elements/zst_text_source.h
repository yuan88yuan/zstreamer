/*=============================================================================
    zst_text_source.h — Text Source convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_TEXT_SOURCE_FACTORY "textsource"

#define ZST_TEXT_SOURCE_PROP_WIDTH               "width"
#define ZST_TEXT_SOURCE_PROP_HEIGHT              "height"
#define ZST_TEXT_SOURCE_PROP_FPS                 "fps"
#define ZST_TEXT_SOURCE_PROP_TEXT                "text"
#define ZST_TEXT_SOURCE_PROP_TEXT_CONTENT        "text-content"
#define ZST_TEXT_SOURCE_PROP_FONT_SIZE           "font-size"
#define ZST_TEXT_SOURCE_PROP_FONT_SIZE_ALIAS           "font_size"
#define ZST_TEXT_SOURCE_PROP_FONT_PATH           "font-path"
#define ZST_TEXT_SOURCE_PROP_FONT_PATH_ALIAS           "font_path"
#define ZST_TEXT_SOURCE_PROP_BG_COLOR            "bg-color"
#define ZST_TEXT_SOURCE_PROP_BACKGROUND_COLOR    "background-color"
#define ZST_TEXT_SOURCE_PROP_TEXT_COLOR          "text-color"
#define ZST_TEXT_SOURCE_PROP_COLOR               "color"
#define ZST_TEXT_SOURCE_PROP_TEXT_COLOR_ALIAS          "text_color"
#define ZST_TEXT_SOURCE_PROP_PIXEL_FORMAT        "pixel-format"
#define ZST_TEXT_SOURCE_PROP_PIXEL_FORMAT_ALIAS        "pixel_format"
#define ZST_TEXT_SOURCE_PROP_NUM_BUFFERS         "num-buffers"
#define ZST_TEXT_SOURCE_PROP_NUM_BUFFERS_ALIAS         "num_buffers"
#define ZST_TEXT_SOURCE_PROP_LOOP                "loop"
#define ZST_TEXT_SOURCE_PROP_USE_CLOCK           "use-clock"
#define ZST_TEXT_SOURCE_PROP_DO_TIMESTAMP        "do-timestamp"
#define ZST_TEXT_SOURCE_PROP_X                   "x"
#define ZST_TEXT_SOURCE_PROP_Y                   "y"

zst_element_t* zst_text_source_create(void);

#ifdef __cplusplus
}
#endif
