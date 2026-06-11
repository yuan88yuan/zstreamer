/*=============================================================================
    zst_video_test_src.h — Video Test Src convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_VIDEO_TEST_SRC_FACTORY "videotestsrc"

zst_element_t* zst_video_test_src_create(void);

#ifdef __cplusplus
}
#endif
