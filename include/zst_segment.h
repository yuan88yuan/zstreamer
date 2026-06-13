/*=============================================================================
    zst_segment.h — Timestamp segment clipping and seeking
=============================================================================*/
#pragma once

#include "zst_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_SEGMENT_STOP_NONE UINT64_MAX

struct zst_segment {
    zst_time_t start;
    zst_time_t stop;
    double rate;
    zst_time_t base;
    zst_time_t position;
};

static inline zst_segment_t
zst_segment_default(void)
{
    zst_segment_t segment;
    segment.start = 0;
    segment.stop = ZST_SEGMENT_STOP_NONE;
    segment.rate = 1.0;
    segment.base = 0;
    segment.position = 0;
    return segment;
}

#ifdef __cplusplus
}
#endif
