/*=============================================================================
    zst_text_source.h — Text Source convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_TEXT_SOURCE_FACTORY "textsource"

zst_element_t* zst_text_source_create(void);

#ifdef __cplusplus
}
#endif
