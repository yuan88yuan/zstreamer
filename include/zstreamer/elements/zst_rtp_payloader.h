/*=============================================================================
    zst_rtp_payloader.h — Generic RTP payloader public API
=============================================================================*/
#pragma once

#include "zst_types.h"
#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create a generic RTP payloader element.
 *
 * The element has one generic "sink" pad for encoded/raw media access units
 * and one "src" pad that emits complete application/x-rtp packet buffers.
 * Transport is intentionally not handled here; connect the src pad to a UDP
 * sink, RTSP interleaver, file sink, test probe, etc.
 *
 * Supported codec property values: "h264", "h265", "aac", "pcm".
 *
 * @return A new element, or NULL on failure.
 */
zst_element_t* zst_rtp_payloader_create(void);

#ifdef __cplusplus
}
#endif
