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

typedef struct {
    size_t struct_size;
    const char* location;
} zst_mp4_demuxer_config_t;

zst_element_t* zst_mp4_demuxer_create(void);
zst_element_t* zst_mp4_demuxer_create_with_config(const zst_mp4_demuxer_config_t* config);

#ifdef __cplusplus
}
#endif
