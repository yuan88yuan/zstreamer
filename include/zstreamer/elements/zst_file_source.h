/*=============================================================================
    zst_file_source.h — File source convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_FILE_SOURCE_FACTORY          "filesrc"
#define ZST_FILE_SOURCE_PROP_PATH       "path"
#define ZST_FILE_SOURCE_PROP_CHUNK_SIZE "chunk-size"
#define ZST_FILE_SOURCE_PROP_LOOP       "loop"
#define ZST_FILE_SOURCE_PROP_OFFSET     "offset"
#define ZST_FILE_SOURCE_PROP_LENGTH     "length"

zst_element_t* zst_file_source_create(const char* path);

#ifdef __cplusplus
}
#endif
