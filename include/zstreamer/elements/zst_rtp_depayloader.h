/*=============================================================================
    zst_rtp_depayloader.h — Generic RTP depayloader public API
=============================================================================*/
#pragma once

#include "zst_types.h"
#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create a generic RTP depayloader element.
 *
 * The element has one "sink" pad for complete application/x-rtp packet
 * buffers and one "src" pad that emits codec access units. Transport is not
 * handled here; connect a UDP/RTSP/file source upstream and a decoder, muxer,
 * or test sink downstream.
 *
 * Supported codec property values: "h264", "h265", "aac", "pcm".
 * H.264/H.265 are emitted as Annex-B access units.
 *
 * @return A new element, or NULL on failure.
 */
zst_element_t* zst_rtp_depayloader_create(void);

#ifdef __cplusplus
}
#endif
