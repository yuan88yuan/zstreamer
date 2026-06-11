/*=============================================================================
    zst_h265_encoder.h — H265 Encoder convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_H265_ENCODER_FACTORY "h265enc"

zst_element_t* zst_h265_encoder_create(void);

#ifdef __cplusplus
}
#endif
