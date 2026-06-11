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

typedef struct {
    size_t struct_size;
    const char* path;
} zst_file_sink_config_t;

zst_element_t* zst_file_sink_create(const char* path);
zst_element_t* zst_file_sink_create_with_config(const zst_file_sink_config_t* config);

#ifdef __cplusplus
}
#endif
