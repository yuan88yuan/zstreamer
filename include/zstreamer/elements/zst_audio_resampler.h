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
#define ZST_AUDIO_RESAMPLER_PROP_ASRC_MODE   "asrc-mode"
#define ZST_AUDIO_RESAMPLER_PROP_MAX_DRIFT_PPM "max-drift-ppm"
#define ZST_AUDIO_RESAMPLER_PROP_DRIFT_CHECK_INTERVAL "drift-check-interval"
#define ZST_AUDIO_RESAMPLER_PROP_RATE_NUMER  "rate-numer"
#define ZST_AUDIO_RESAMPLER_PROP_RATE_DENOM  "rate-denom"

/* ASRC mode values */
#define ZST_ASRC_MODE_NONE  "none"   /* Fixed-ratio SRC (default) */
#define ZST_ASRC_MODE_PTS   "pts"    /* PTS-based drift compensation */

zst_element_t* zst_audio_resampler_create(int target_sample_rate, int target_channels, const char* target_format);
zst_element_t* zst_audio_resampler_create_with_config(int target_sample_rate, int target_channels, const char* target_format, const char* asrc_mode, double max_drift_ppm);

#ifdef __cplusplus
}
#endif
