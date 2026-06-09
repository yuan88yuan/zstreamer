# Infrastructure — Phases 5–7

## Phase 5 — Caps Negotiation  (✅ done)

Arguably the most important missing piece. Without caps negotiation, the pipeline can't verify or convert between formats. Elements must advertise what they produce (src caps) and what they consume (sink caps), and adjacent pads must agree before linking.

- [x] `zst_caps_t` structure: a list of structures each describing:
  - `media_type` (e.g. `"video/x-raw"`, `"video/x-h264"`)
  - Video: `width`, `height`, `framerate`, `pixel_format` (NV12, YUV420P, etc.)
  - Audio: `channels`, `sample_rate`, `format` (S16LE, F32LE, etc.)
- [x] Caps intersection: `zst_caps_intersect(src_caps, sink_caps) → zst_caps_t*`
- [x] Pad caps API: `zst_pad_set_caps()`, `zst_pad_get_caps()`, `zst_pad_negotiate()`
- [x] Auto-negotiation at link time
- [x] Caps-query mechanism in element ops vtable

**Conversion elements** (4g video-scaler, 4h audio-resampler) will be auto-inserted by the
negotiation process when formats don't match.

**Why this matters:** Without caps, linking NV12→YUV420P gives silent garbage.

---

## Phase 6 — Event Bus  (✅ done)

An async notification channel (`zst_bus_t`) that decouples error/state/EOS from the data path. Events: `EOS`, `ERROR`, `STATE_CHANGED`, `WARNING`.

- [x] `zst_bus_t` — thread-safe event queue
- [x] `zst_bus_post()` / `zst_bus_pop(timeout_ms)`
- [x] Async callback dispatch
- [x] Wire pipeline lifecycle events
- [x] Wire error returns → `ZST_EVENT_ERROR`

---

## Phase 7 — Dynamic Plugins  (✅ done)

- [x] Build each element as a separate `.so`
- [x] Plugin discovery path (`ZSTREAMER_PLUGIN_PATH` env var)
- [x] Ref-counted plugin registry
