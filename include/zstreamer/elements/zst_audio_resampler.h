/*=============================================================================
    zst_audio_resampler.h — Audio Resampler convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_AUDIO_RESAMPLER_FACTORY "audioresampler"

#define ZST_AUDIO_RESAMPLER_PROP_SAMPLE_RATE "sample-rate"
#define ZST_AUDIO_RESAMPLER_PROP_CHANNELS    "channels"
#define ZST_AUDIO_RESAMPLER_PROP_FORMAT      "format"

zst_element_t* zst_audio_resampler_create(int target_sample_rate, int target_channels, const char* target_format);

#ifdef __cplusplus
}
#endif
