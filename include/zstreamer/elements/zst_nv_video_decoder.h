/*=============================================================================
    zst_nv_video_decoder.h — NV V4L2 Decoder convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_NVDEC_FACTORY "nvdec"

/* Properties */
#define ZST_NVDEC_PROP_CODEC      "codec"
#define ZST_NVDEC_PROP_SKIP_FRAMES "skip-frames"
#define ZST_NVDEC_PROP_ERROR_REPORTING "error-reporting"

zst_element_t* zst_nv_video_decoder_create(void);

#ifdef __cplusplus
}
#endif
