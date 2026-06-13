/*=============================================================================
    zst_aac_decoder.h — Aac Decoder convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_AAC_DECODER_FACTORY "aacdec"

#define ZST_AAC_DECODER_PROP_THREADS  "threads"

zst_element_t* zst_aac_decoder_create(void);

#ifdef __cplusplus
}
#endif
