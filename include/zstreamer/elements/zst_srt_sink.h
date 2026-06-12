/*=============================================================================
    zst_srt_sink.h — SRT Sink convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_SRT_SINK_FACTORY "srtsink"

zst_element_t* zst_srt_sink_create(void);

#ifdef __cplusplus
}
#endif
