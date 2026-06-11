/*=============================================================================
    zst_audio_test_src.h — Audio Test Src convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_AUDIO_TEST_SRC_FACTORY "audiotestsrc"
#define ZST_AUDIO_TEST_SRC_PROP_SAMPLE_RATE         "sample-rate"
#define ZST_AUDIO_TEST_SRC_PROP_CHANNELS            "channels"
#define ZST_AUDIO_TEST_SRC_PROP_SAMPLE_FORMAT       "sample-format"
#define ZST_AUDIO_TEST_SRC_PROP_WAVE                "wave"
#define ZST_AUDIO_TEST_SRC_PROP_FREQUENCY           "frequency"
#define ZST_AUDIO_TEST_SRC_PROP_VOLUME              "volume"
#define ZST_AUDIO_TEST_SRC_PROP_SAMPLES_PER_BUFFER  "samples-per-buffer"
#define ZST_AUDIO_TEST_SRC_PROP_NUM_SAMPLES         "num-samples"
#define ZST_AUDIO_TEST_SRC_PROP_NUM_BUFFERS         "num-buffers"
#define ZST_AUDIO_TEST_SRC_PROP_LOOP                "loop"
#define ZST_AUDIO_TEST_SRC_PROP_USE_CLOCK           "use-clock"

typedef struct {
    size_t struct_size;
    uint32_t sample_rate;
    uint32_t channels;
    const char* sample_format;
    const char* wave;
    double frequency;
    double volume;
    uint32_t samples_per_buffer;
    int64_t num_samples;
    int64_t num_buffers;
    bool loop;
    bool use_clock;
} zst_audio_test_src_config_t;

zst_element_t* zst_audio_test_src_create(void);
zst_element_t* zst_audio_test_src_create_with_config(const zst_audio_test_src_config_t* config);

#ifdef __cplusplus
}
#endif
