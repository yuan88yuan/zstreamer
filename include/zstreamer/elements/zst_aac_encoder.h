/*=============================================================================
    zst_aac_encoder.h — Aac Encoder convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_AAC_ENCODER_FACTORY "aacenc"

zst_element_t* zst_aac_encoder_create(void);

#ifdef __cplusplus
}
#endif
