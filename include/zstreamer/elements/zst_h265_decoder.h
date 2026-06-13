/*=============================================================================
    zst_h265_decoder.h — H265 Decoder convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_H265_DECODER_FACTORY "h265dec"

#define ZST_H265_DECODER_PROP_THREADS  "threads"

zst_element_t* zst_h265_decoder_create(void);

#ifdef __cplusplus
}
#endif
