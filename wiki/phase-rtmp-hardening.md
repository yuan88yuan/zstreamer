# RTMP Source/Sink Hardening (Post-P0)

This document tracks the tasks and status for hardening the RTMP Source and Sink elements in `zstreamer`.

## Status Roadmap

### P1 — High

- [x] **Resource leak on error in `rtmp_sink_write_header`** — If `avformat_new_stream()` fails, `fc` and the AVIO context are not freed.  Add a `fail:` cleanup label.  ([rtmp_sink.c](file:///home/zzlee/zstreamer/src/rtmp_sink.c#L40-L100))
- [x] **Correct buffer types in `rtmp_source`** — ~~Use `ZST_BUFFER_VIDEO_PACKET` / `ZST_BUFFER_AUDIO_PACKET`~~ ✅ Fixed with P0.
- [x] **NULL checks in create functions** — `calloc`, `zst_element_create`, `zst_pad_create` return values unchecked in both `zst_rtmp_source_create()` and `zst_rtmp_sink_create()`.  ([rtmp_source.c](file:///home/zzlee/zstreamer/src/rtmp_source.c#L215-L255), [rtmp_sink.c](file:///home/zzlee/zstreamer/src/rtmp_sink.c#L240-L280))

### P2 — Medium

- [x] **Create convenience headers** — Add `include/zstreamer/elements/zst_rtmp_source.h` and `zst_rtmp_sink.h` (all 24 other elements already have them).
- [x] **Update documentation status** — Mark RTMP as ✅ Done in `AGENTS.md`, `README.md`, `wiki/future.md`; check off completed sub-items in `wiki/phase-elements.md` (4q/4r).
- [x] **Add error logging on write failure** — `av_interleaved_write_frame` failure in `rtmp_sink_write` returns `ZST_ERROR` silently; add `ZST_LOG_ERROR`.  ([rtmp_sink.c](file:///home/zzlee/zstreamer/src/rtmp_sink.c#L115-L125))
- [x] **Write trailer on EOS** — `rtmp_sink_check_eos` posts the bus event but doesn't call `av_write_trailer()` first; stream may not be finalized until `stop`.  ([rtmp_sink.c](file:///home/zzlee/zstreamer/src/rtmp_sink.c#L125-L140))
- [x] **NULL param guards in property functions** — Neither `set_property` nor `get_property` validates `el`, `name`, or `value` pointers (both files).
- [x] **No mutex for thread-safe pad push** — `rtmp_source` worker thread calls `zst_pad_push()` without a mutex; race possible if `stop` is called mid-push.  ([rtmp_source.c](file:///home/zzlee/zstreamer/src/rtmp_source.c#L45-L105))

### P3 — Low / Enhancement

- [x] **Add `open`/`close` ops** — Move URL validation and FFmpeg context allocation to `open` (NULL→READY); keep `start` for thread launch.  Matches RTSP source pattern.
- [x] **Add `get_caps` op** — Return video/audio caps templates so pipeline introspection and auto-linking work.
- [x] **Add property specs to plugin descriptors** — Both elements accept a `url` property but declare `nb_properties = 0`.  Add `zst_property_spec_t` tables and wire them into the builtin descriptor table.
- [x] **Sink: negotiate codec from upstream caps** — Currently hardcodes `AV_CODEC_ID_H264` / `AV_CODEC_ID_AAC`.  Should read upstream caps to support H.265 video.  ([rtmp_sink.c](file:///home/zzlee/zstreamer/src/rtmp_sink.c#L50-L100))
- [x] **Sink: populate codec parameters** — Set `codecpar->width`, `height`, `sample_rate`, `channels` from upstream caps or metadata.
- [x] **Source: accept URL at construction** — Change `zst_rtmp_source_create(void)` to `zst_rtmp_source_create(const char* url)` (NULL allowed) for API parity with RTSP source.
- [x] **Reconnection with back-off** — Both source and sink lack auto-reconnect on stream loss (planned in phase-elements.md 4q/4r).
- [x] **Additional properties** — `live`, `buffer_time`, `swf_url` for source; `live` for sink (planned in phase-elements.md).
