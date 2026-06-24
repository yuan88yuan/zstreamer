/*=============================================================================
    zst_audio_mixer.h — Audio mixer element convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_AUDIO_MIXER_FACTORY                 "audiomixer"

#define ZST_AUDIO_MIXER_PROP_LATENCY            "latency"
#define ZST_AUDIO_MIXER_PROP_MAX_LATENESS       "max-lateness"
#define ZST_AUDIO_MIXER_PROP_DROPPED_LATE       "dropped-late"

#define ZST_AUDIO_MIXER_PAD_PROP_VOLUME         "volume"
#define ZST_AUDIO_MIXER_PAD_PROP_MUTE           "mute"
#define ZST_AUDIO_MIXER_PAD_PROP_PAN            "pan"

typedef struct {
    size_t struct_size;
    uint32_t latency;
} zst_audio_mixer_config_t;

zst_element_t* zst_audio_mixer_create(void);
zst_element_t* zst_audio_mixer_create_with_config(const zst_audio_mixer_config_t* config);

/* Request a sink pad.  If name is NULL, the next sink_%u pad is created. */
zst_pad_t* zst_audio_mixer_request_pad(zst_element_t* el, const char* name);

/* Release a previously requested sink pad. Safe in READY/PLAYING. */
zst_result_t zst_audio_mixer_release_pad(zst_element_t* el, zst_pad_t* pad);

#ifdef __cplusplus
}
#endif
