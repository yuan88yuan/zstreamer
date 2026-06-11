/*=============================================================================
    zst_audio_test_src.h — Audio Test Src convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_AUDIO_TEST_SRC_FACTORY "audiotestsrc"

zst_element_t* zst_audio_test_src_create(void);

#ifdef __cplusplus
}
#endif
