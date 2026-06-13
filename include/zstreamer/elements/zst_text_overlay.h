/*=============================================================================
    zst_text_overlay.h — Text Overlay convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_TEXT_OVERLAY_FACTORY "textoverlay"
#define ZST_TEXT_OVERLAY_PROP_TEXT          "text"
#define ZST_TEXT_OVERLAY_PROP_TIMECODE       "timecode"
#define ZST_TEXT_OVERLAY_PROP_FONT_SIZE      "font-size"
#define ZST_TEXT_OVERLAY_PROP_FONT_PATH      "font-path"
#define ZST_TEXT_OVERLAY_PROP_X              "x"
#define ZST_TEXT_OVERLAY_PROP_Y              "y"

typedef struct {
    size_t struct_size;
    const char* text;
    bool timecode;
    int32_t font_size;
    const char* font_path;
    int32_t x;
    int32_t y;
} zst_text_overlay_config_t;

zst_element_t* zst_text_overlay_create(const char* text);
zst_element_t* zst_text_overlay_create_with_config(const zst_text_overlay_config_t* config);

#ifdef __cplusplus
}
#endif
