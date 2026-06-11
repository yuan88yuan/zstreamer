# Implementation Plan

All phases are now documented in separate files for easier maintenance.

| Phase | Document | Lines | Status |
|-------|----------|-------|--------|
| 0–3   | [Core Framework](phase-core.md) | ~190 | ✅ Complete |
| 4     | [Element Implementations](phase-elements.md) | ~360 | ✅ Done (all 26 elements) |
| 5–7   | [Infrastructure](phase-infrastructure.md) | ~58 | ✅ Complete |
| 8     | [Advanced Features](phase-advanced.md) | ~417 | 🔄 In Progress |
| 9–10  | [Future Work](phase-future.md) | ~34 | ⬜ Not Started |

---

## Quick Status

| Area | Status | Notes |
|------|--------|-------|
| Scaffolding (0) | ✅ Done | CMake, Docker, git, AGENTS.md |
| Core Framework (1) | ✅ Done | All 8 core modules |
| Scheduler (2) | ✅ Done | Topological sort, push/pull, EOS |
| Queue Element (3) | ✅ Done | First-class queue with worker thread |
| Logging (3.5) | ✅ Done | Compile-time log levels, thread-safe |
| Elements (4) | ✅ Done | 26 elements implemented |
| Caps Negotiation (5) | ✅ Done | Intersection, auto-negotiation |
| Event Bus (6) | ✅ Done | Error/state/EOS notifications |
| Dynamic Plugins (7) | ✅ Done | dlopen-based loading |
| Allocator API (8a) | ✅ Mostly done | Pool + elements migration done; default sizing & tests pending |
| Clock (8b) | ✅ Done | System clock, pipeline integration |
| Testing & CI (9) | ⬜ Planned | CI pipeline, stress tests, static analysis |
| Documentation (10) | ⬜ Planned | Doxygen API ref, tutorials |
| Advanced Features (8c) | 📝 Planned | Element bin, pad probes, segment seeking |
| Element Public API (8d) | ✅ Done | Descriptor ABI, plugin introspection, typed properties, official element metadata, convenience headers, library & installation layout |

---

## RTMP Source/Sink Hardening  (Post-P0)

P0 bugs (memory leak, audio caps type) are fixed.  Items below remain open.

### P1 — High

- [ ] **Resource leak on error in `rtmp_sink_write_header`** — If `avformat_new_stream()` fails, `fc` and the AVIO context are not freed.  Add a `fail:` cleanup label.  (`src/rtmp_sink.c:55-65`)
- [ ] **Correct buffer types in `rtmp_source`** — ~~Use `ZST_BUFFER_VIDEO_PACKET` / `ZST_BUFFER_AUDIO_PACKET`~~ ✅ Fixed with P0.
- [ ] **NULL checks in create functions** — `calloc`, `zst_element_create`, `zst_pad_create` return values unchecked in both `zst_rtmp_source_create()` and `zst_rtmp_sink_create()`.  (`src/rtmp_source.c:226-237`, `src/rtmp_sink.c:251-265`)

### P2 — Medium

- [ ] **Create convenience headers** — Add `include/zstreamer/elements/zst_rtmp_source.h` and `zst_rtmp_sink.h` (all 24 other elements already have them).
- [ ] **Update documentation status** — Mark RTMP as ✅ Done in `AGENTS.md`, `README.md`, `wiki/future.md`; check off completed sub-items in `wiki/phase-elements.md` (4q/4r).
- [ ] **Add error logging on write failure** — `av_interleaved_write_frame` failure in `rtmp_sink_write` returns `ZST_ERROR` silently; add `ZST_LOG_ERROR`.  (`src/rtmp_sink.c:107-110`)
- [ ] **Write trailer on EOS** — `rtmp_sink_check_eos` posts the bus event but doesn't call `av_write_trailer()` first; stream may not be finalized until `stop`.  (`src/rtmp_sink.c:116-127`)
- [ ] **NULL param guards in property functions** — Neither `set_property` nor `get_property` validates `el`, `name`, or `value` pointers (both files).
- [ ] **No mutex for thread-safe pad push** — `rtmp_source` worker thread calls `zst_pad_push()` without a mutex; race possible if `stop` is called mid-push.  (`src/rtmp_source.c`)

### P3 — Low / Enhancement

- [ ] **Add `open`/`close` ops** — Move URL validation and FFmpeg context allocation to `open` (NULL→READY); keep `start` for thread launch.  Matches RTSP source pattern.
- [ ] **Add `get_caps` op** — Return video/audio caps templates so pipeline introspection and auto-linking work.
- [ ] **Add property specs to plugin descriptors** — Both elements accept a `url` property but declare `nb_properties = 0`.  Add `zst_property_spec_t` tables and wire them into the builtin descriptor table.
- [ ] **Sink: negotiate codec from upstream caps** — Currently hardcodes `AV_CODEC_ID_H264` / `AV_CODEC_ID_AAC`.  Should read upstream caps to support H.265 video.  (`src/rtmp_sink.c:58,67`)
- [ ] **Sink: populate codec parameters** — Set `codecpar->width`, `height`, `sample_rate`, `channels` from upstream caps or metadata.
- [ ] **Source: accept URL at construction** — Change `zst_rtmp_source_create(void)` to `zst_rtmp_source_create(const char* url)` (NULL allowed) for API parity with RTSP source.
- [ ] **Reconnection with back-off** — Both source and sink lack auto-reconnect on stream loss (planned in phase-elements.md 4q/4r).
- [ ] **Additional properties** — `live`, `buffer_time`, `swf_url` for source; `live` for sink (planned in phase-elements.md).

