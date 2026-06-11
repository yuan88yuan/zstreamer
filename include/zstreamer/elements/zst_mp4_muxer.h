/*=============================================================================
    zst_mp4_muxer.h — Mp4 Muxer convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_MP4_MUXER_FACTORY "mp4mux"

zst_element_t* zst_mp4_muxer_create(void);

#ifdef __cplusplus
}
#endif
