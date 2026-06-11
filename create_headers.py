import re

elements = [
    ('zst_v4l2_source_create', 'v4l2_source', 'void', 'v4l2src'),
    ('zst_alsa_source_create', 'alsa_source', 'void', 'alsasrc'),
    ('zst_h264_encoder_create', 'h264_encoder', 'void', 'h264enc'),
    ('zst_h264_decoder_create', 'h264_decoder', 'void', 'h264dec'),
    ('zst_h265_encoder_create', 'h265_encoder', 'void', 'h265enc'),
    ('zst_h265_decoder_create', 'h265_decoder', 'void', 'h265dec'),
    ('zst_aac_encoder_create', 'aac_encoder', 'void', 'aacenc'),
    ('zst_aac_decoder_create', 'aac_decoder', 'void', 'aacdec'),
    ('zst_mp4_muxer_create', 'mp4_muxer', 'void', 'mp4mux'),
    ('zst_video_scaler_create', 'video_scaler', 'int target_width, int target_height, const char* target_pixel_format', 'videoscaler'),
    ('zst_audio_resampler_create', 'audio_resampler', 'int target_sample_rate, int target_channels, const char* target_format', 'audioresampler'),
    ('zst_video_test_src_create', 'video_test_src', 'void', 'videotestsrc'),
    ('zst_audio_test_src_create', 'audio_test_src', 'void', 'audiotestsrc'),
    ('zst_text_overlay_create', 'text_overlay', 'const char* text', 'textoverlay'),
    ('zst_text_source_create', 'text_source', 'void', 'textsource'),
    ('zst_srt_parser_create', 'srt_parser', 'const char* path', 'srt_parser'),
    ('zst_net_source_create', 'net_source', 'void', 'netsrc'),
    ('zst_net_sink_create', 'net_sink', 'void', 'netsink'),
    ('zst_rtsp_source_create', 'rtsp_source', 'const char* url', 'rtspsrc'),
    ('zst_rtsp_sink_create', 'rtsp_sink', 'void', 'rtspsink'),
    ('zst_rtsp_server_create', 'rtsp_server', 'void', 'rtsp_server')
]

for func, name, args, factory in elements:
    filename = f"include/zstreamer/elements/zst_{name}.h"
    title = f"{name.replace('_', ' ').title()}"
    macro_prefix = f"ZST_{name.upper()}"

    content = f"""/*=============================================================================
    zst_{name}.h — {title} convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {{
#endif

#define {macro_prefix}_FACTORY "{factory}"

zst_element_t* {func}({args});

#ifdef __cplusplus
}}
#endif
"""
    with open(filename, 'w') as f:
        f.write(content)
