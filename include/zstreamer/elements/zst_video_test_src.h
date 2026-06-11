/*=============================================================================
    zst_video_test_src.h — Video Test Src convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_VIDEO_TEST_SRC_FACTORY "videotestsrc"
#define ZST_VIDEO_TEST_SRC_PROP_WIDTH         "width"
#define ZST_VIDEO_TEST_SRC_PROP_HEIGHT        "height"
#define ZST_VIDEO_TEST_SRC_PROP_FPS           "fps"
#define ZST_VIDEO_TEST_SRC_PROP_PATTERN       "pattern"
#define ZST_VIDEO_TEST_SRC_PROP_PIXEL_FORMAT   "pixel-format"
#define ZST_VIDEO_TEST_SRC_PROP_NUM_BUFFERS   "num-buffers"
#define ZST_VIDEO_TEST_SRC_PROP_LOOP          "loop"
#define ZST_VIDEO_TEST_SRC_PROP_USE_CLOCK     "use-clock"
#define ZST_VIDEO_TEST_SRC_PROP_DO_TIMESTAMP  "do-timestamp"

typedef struct {
    size_t struct_size;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    const char* pattern;
    const char* pixel_format;
    int32_t num_buffers;
    bool loop;
    bool use_clock;
} zst_video_test_src_config_t;

zst_element_t* zst_video_test_src_create(void);
zst_element_t* zst_video_test_src_create_with_config(const zst_video_test_src_config_t* config);

#ifdef __cplusplus
}
#endif
