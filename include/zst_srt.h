/*=============================================================================
    zst_srt.h — Simple SRT subtitle parser element
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

zst_element_t* zst_srt_parser_create(const char* path);

#ifdef __cplusplus
}
#endif
