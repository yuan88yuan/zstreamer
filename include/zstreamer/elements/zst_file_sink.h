/*=============================================================================
    zst_file_sink.h — File sink convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_FILE_SINK_FACTORY       "filesink"
#define ZST_FILE_SINK_PROP_PATH     "path"
#define ZST_FILE_SINK_PROP_LOCATION "location"

zst_element_t* zst_file_sink_create(const char* path);

#ifdef __cplusplus
}
#endif
