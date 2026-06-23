/*=============================================================================
    zst_sdp_demuxer.h — SDP/RTP Demuxer public API
=============================================================================*/
#pragma once

#include "zst_types.h"
#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create a new SDP/RTP demuxer element.
 *
 * The SDP demuxer parses SDP descriptions to discover media tracks,
 * receives raw RTP packets on its "sink" pad, reorders them by RTP
 * sequence number, depacketizes (H.264, H.265, AAC, etc.), and outputs
 * ordered, perfectly-timed media buffers on dynamically-created source pads
 * ("video_0", "audio_0", ...).
 *
 * Timing is driven by a DPLL (digital phase-locked loop) that compensates
 * for clock drift between the sender and receiver oscillators.
 *
 * Properties:
 *   - "sdp" (string)         : SDP description text (inline)
 *   - "sdp-file" (string)    : Path to an SDP file
 *   - "jitter-buffer-ms" (int) : Target jitter buffer depth in ms (200)
 *   - "reorder-capacity" (int) : Per-track RTP reorder slots (256)
 *   - "max-lateness-ms" (int)  : Max packet lateness before drop (500)
 *   - "clock-rate-video" (int) : Default video RTP clock rate (90000)
 *   - "clock-rate-audio" (int) : Default audio RTP clock rate (48000)
 *
 * Source pads (auto-created from SDP):
 *   - "video_0", "video_1", ... for video (encoded video packets)
 *   - "audio_0", "audio_1", ... for audio (encoded audio packets)
 *
 * @return A new element, or NULL on failure.
 */
zst_element_t* zst_sdp_demuxer_create(void);

#ifdef __cplusplus
}
#endif
