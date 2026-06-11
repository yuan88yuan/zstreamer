import re
import os

elements = {
    'v4l2_source': 'v4l2_source.c',
    'alsa_source': 'alsa_source.c',
    'h264_encoder': 'h264_encoder.c',
    'h264_decoder': 'h264_decoder.c',
    'h265_encoder': 'h265_encoder.c',
    'h265_decoder': 'h265_decoder.c',
    'aac_encoder': 'aac_encoder.c',
    'aac_decoder': 'aac_decoder.c',
    'mp4_muxer': 'mp4_muxer.c',
    'video_scaler': 'video_scaler.c',
    'audio_resampler': 'audio_resampler.c',
    'video_test_src': 'video_test_src.c',
    'audio_test_src': 'audio_test_src.c',
    'text_overlay': 'text_overlay.c',
    'text_source': 'text_source.c',
    'srt_parser': 'srt_parser.c',
    'net_source': 'net_source.c',
    'net_sink': 'net_sink.c',
    'rtsp_source': 'rtsp_source.c',
    'rtsp_sink': 'rtsp_sink.c',
    'rtsp_server': 'rtsp_server.c',
}

properties = {}
for name, c_file in elements.items():
    with open(f"src/{c_file}", "r") as f:
        content = f.read()

    props = re.findall(r'{\s*\"(.*?)\"', content)
    # Remove duplicates and things that are not properties (like pad names)
    props = [p for p in props if p not in ('src', 'sink', 'video', 'audio', 'text', 'video_%u', 'audio_%u')]
    # also remove the component names themselves
    props = [p for p in props if p not in (name,)]

    # Let's inspect the actual struct definitions
    struct_match = re.search(r'static const zst_property_spec_t g_\w+_properties\[\] = {(.*?)};', content, re.DOTALL)
    if struct_match:
        struct_content = struct_match.group(1)
        actual_props = re.findall(r'\{\s*\"(.*?)\"', struct_content)
        properties[name] = actual_props

print(properties)
