/*=============================================================================
    zst_text_overlay.h — Text Overlay convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_TEXT_OVERLAY_FACTORY "textoverlay"

zst_element_t* zst_text_overlay_create(const char* text);

#ifdef __cplusplus
}
#endif
