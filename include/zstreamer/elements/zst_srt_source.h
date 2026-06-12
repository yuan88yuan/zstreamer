/*=============================================================================
    zst_srt_source.h — SRT Source convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_SRT_SOURCE_FACTORY "srtsrc"

zst_element_t* zst_srt_source_create(void);

#ifdef __cplusplus
}
#endif
