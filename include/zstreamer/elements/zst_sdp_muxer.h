/*=============================================================================
    zst_sdp_muxer.h — SDP generator/muxer public API
=============================================================================*/
#pragma once

#include "zst_types.h"
#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create a new SDP muxer/generator element.
 *
 * The SDP muxer accepts encoded media packets on optional "video" and
 * "audio" sink pads, derives codec parameter sets where possible, and emits
 * a single application/sdp buffer on its "src" pad.  The generated SDP is also
 * available through the read-only "sdp" property.
 *
 * Initial implementation supports SDP generation for H.264, H.265/HEVC, and
 * AAC RTP payload descriptions.
 *
 * @return A new element, or NULL on failure.
 */
zst_element_t* zst_sdp_muxer_create(void);

#ifdef __cplusplus
}
#endif
