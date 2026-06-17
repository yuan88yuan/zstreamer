/*=============================================================================
    zst_mp4_demuxer.h — MP4 Demuxer convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_MP4_DEMUXER_FACTORY "mp4demux"

#define ZST_MP4_DEMUXER_PROP_LOCATION     "location"
#define ZST_MP4_DEMUXER_PROP_REAL_TIME_PACING "real-time-pacing"

typedef struct {
    size_t struct_size;
    const char* location;
    bool real_time_pacing;
} zst_mp4_demuxer_config_t;

zst_element_t* zst_mp4_demuxer_create(void);
zst_element_t* zst_mp4_demuxer_create_with_config(const zst_mp4_demuxer_config_t* config);

/**
 * Retrieve the raw avcC (H.264) or hvcC (H.265) extradata from the demuxer.
 * Only valid after the element has been opened (ZST_STATE_READY or higher).
 * The returned pointer is owned by the demuxer — do NOT free it.
 * Returns NULL and sets *size_out=0 if no video stream is found.
 */
const uint8_t* zst_mp4_demuxer_get_video_extradata(zst_element_t* el, int* size_out);

#ifdef __cplusplus
}
#endif
