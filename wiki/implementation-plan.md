# Implementation Plan

## Phase 0 — Project Scaffolding  (✅ done)

- [x] AGENTS.md with project overview and build instructions
- [x] .gitignore for build artifacts and IDE files
- [x] Git repository initialised and pushed to GitHub
- [x] CMakeLists.txt with core library + test targets
- [x] Dockerfile with build dependencies (ubuntu 24.04, cmake, pthreads, multimedia libs)

---

## Phase 1 — Core Framework  (✅ done)

The fundamental types and lifecycle management.

| Component   | Status | Notes                                                      |
|-------------|--------|------------------------------------------------------------|
| zst_types    | ✅ done  | Base types, error codes, forward declarations               |
| zst_buffer   | ✅ done  | Ref-counted buffer with typed memory; destroy callback      |
| zst_pad      | ✅ done  | SRC/SINK pads with peer linking; default push/pull callbacks|
| zst_element  | ✅ done  | Ops vtable, state machine (NULL/READY/PAUSED/PLAYING), pads |
| zst_pipeline | ✅ done  | Element container, state propagation, topological sort      |
| zst_queue    | ✅ done  | Thread-safe bounded queue, timeout, bytes/duration limits, async mode |
| zst_scheduler| ✅ done  | Single-thread + multi-thread worker pool, EOS propagation   |
| zst_plugin   | ✅ done  | dlopen loader, plugin entry point                           |

---

## Phase 2 — Scheduler Integration & Pipeline Wiring  (✅ done)

- [x] **Pad push/pull semantics**: default callbacks chain `process()` → push downstream
- [x] **zst_pad_push() / zst_pad_pull()** — walk the pad graph
- [x] **zst_pad_reset_callbacks()** — restore defaults after custom override
- [x] **Topological sort**: `zst_pipeline_topological_sort()` to ensure correct element order
- [x] **Scheduler worker loop**: round-robin element assignment across worker threads
- [x] **State machine hardening**: validate transitions, handle error rollback
- [x] **EOS signalling**: `ZST_BUFFER_FLAG_EOS` propagated through pad graph

**Test deliverables:** ✅ 19 unit tests

---

## Phase 3 — Queue Element  (✅ done)

Explicit queue elements as first-class `zst_element` subclasses — like GStreamer's `queue` element. Users insert them at pipeline boundaries to control buffering and threading.

```
v4l2src → queue → h264enc → queue → mp4mux → queue → filesink
          ^^^^^            ^^^^^            ^^^^^
      explicit boundary  explicit boundary  explicit boundary
```

- [x] `zst_queue_element_create()` — full `zst_element` with sink pad + src pad + worker thread
- [x] Lifecycle hooks: `open` creates queue, `close` destroys it, `start/stop` manage thread
- [x] EOS passthrough through the queue element
- [x] Configurable via `zst_queue_config_t` (max buffers, bytes, duration)
- [x] `ZST_QUEUE_ASYNC` mode drops buffers when full
- [x] `example_record.c` updated with explicit queue elements
- [x] Multi-threaded scheduler test uses queue elements

**Test deliverables:** ✅ Multi-threaded pipeline test with queue elements

---

## Phase 4 — Real Element Implementations  (✅ done)

Eight elements are fully implemented with real hardware/codec integration and synthetic fallbacks for headless environments.
Two more handle format conversion (scaling, resampling) — essential once caps negotiation (Phase 5) requires automatic conversion between mismatched formats.

### 4a — V4L2 Source  (✅ done)
- [x] Open `/dev/video0` with O_RDWR | O_NONBLOCK
- [x] Format negotiation: `VIDIOC_S_FMT` (YUYV, 640×480)
- [x] MMAP buffer setup: `VIDIOC_REQBUFS` / `QUERYBUF` / `QBUF`
- [x] `VIDIOC_STREAMON` / `VIDIOC_STREAMOFF`
- [x] poll-based non-blocking capture with timeout
- [x] YUYV → YUV420P colour space conversion
- [x] **Synthetic fallback** when no camera: moving vertical bar pattern, 30 fps

**Dependencies:** `libv4l-dev` (in Docker)

### 4b — H.264 Encoder  (✅ done)
- [x] x264 integration: `x264_param_default_preset("ultrafast", "zerolatency")`
- [x] CRF rate control (23)
- [x] Accept I420 YUV planes from `zst_video_frame_t` payload
- [x] NAL unit concatenation into `zst_buffer` packets
- [x] PTS passthrough
- [x] EOS passthrough
- [x] Lazy initialization on first frame (handles dynamic resolution)

**Dependencies:** `libx264-dev` (in Docker)

### 4c — MP4 Muxer  (✅ done)
- [x] FFmpeg `libavformat` integration
- [x] Custom AVIO write callback pushes buffers downstream (not to file)
- [x] Video stream (H.264) + audio stream (AAC)
- [x] Fragmented MP4: `frag_keyframe+empty_moov+default_base_moof`
- [x] Per-stream EOS tracking: muxer waits for both video + audio EOS before propagating
- [x] Proper `av_write_trailer()` on stop

**Dependencies:** `libavformat-dev`, `libavcodec-dev`, `libavutil-dev` (in Docker)

### 4d — File Sink  (✅ done)
- [x] FILE* writer: `fopen`, `fwrite`, `fclose`
- [x] Writes buffer memory data to file
- [x] Proper `close` lifecycle hook

### 4e — ALSA Audio Source  (✅ done)
- [x] `snd_pcm_open("default", SND_PCM_STREAM_CAPTURE)`
- [x] Parameter setup: S16_LE, 44100Hz, stereo, 0.5s latency
- [x] `snd_pcm_readi()` for capture
- [x] Underrun / xrun recovery (`-EPIPE` → `snd_pcm_prepare`)
- [x] **Synthetic fallback**: 440Hz square wave, 44100Hz timing with nanosleep

**Dependencies:** `libasound2-dev`

### 4f — AAC Encoder  (✅ done)
- [x] FFmpeg `libavcodec` AAC encoder: `avcodec_find_encoder(AV_CODEC_ID_AAC)`
- [x] S16LE interleaved → FLTP float planar conversion
- [x] `avcodec_send_frame()` / `avcodec_receive_packet()` API
- [x] 128kbps bitrate
- [x] EOS passthrough

**Dependencies:** `libavcodec-dev` (in Docker)

### 4g — Video Scaler  (✅ done)

A conversion element that scales video frames and converts pixel formats. Deployed
when a source's output caps (e.g. 1080p NV12) don't match the next element's input
caps (e.g. 720p I420).

- [x] **Interface**: single sink pad, single src pad — accepts raw video, outputs raw video
- [x] **Backend**: `libswscale` from FFmpeg (`sws_getContext` / `sws_scale`)
- [x] **Auto-configuration**: on first frame, allocate the SWS context based on input resolution/format and configured output resolution/format
- [x] Configurable target: `width`, `height`, `pixel_format` — or passthrough if formats match
- [x] **Synthetic fallback**: naive nearest-neighbour scaling if `libswscale` unavailable
- [x] EOS passthrough

**Dependencies:** `libswscale-dev` (in Docker)

### 4h — Audio Resampler  (✅ done)

Converts audio sample rate and format. Needed when source sample rate (e.g. ALSA
at 48000Hz) differs from what the encoder expects (e.g. AAC at 44100Hz), or when
format mismatches (S16LE ↔ F32LE).

- [x] **Interface**: single sink pad, single src pad — accepts raw audio, outputs raw audio
- [x] **Backend**: `libswresample` from FFmpeg (`swr_alloc_set_opts` / `swr_convert`)
- [x] **Auto-configuration**: on first frame, allocate SWR context from input/output params
- [x] Configurable: `sample_rate`, `sample_format`, `channels` — passthrough if matching
- [x] **Synthetic fallback**: linear interpolation resampling if `libswresample` unavailable
- [x] EOS passthrough

**Dependencies:** `libswresample-dev` (in Docker)

---

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

---

## Phase 8 — Advanced Features

### 8a — Allocator API  (from `wiki/future.md`)
- [ ] `zst_allocator_t` interface: `alloc`, `free`, ref-counting
- [ ] Default CPU allocator (malloc/free)
- [ ] DMABUF allocator (Linux dma-buf)
- [ ] CUDA / Vulkan device memory allocators
- [ ] Buffer pools to eliminate per-frame allocation

### 8b — Clock  (from `wiki/future.md`)
- [ ] `zst_clock_t` interface: `get_time`, `wait`
- [ ] System clock wrapping `CLOCK_MONOTONIC`
- [ ] Pipeline-level master clock selection
- [ ] Clock slaving for A/V sync

### 8c — Other Advanced Features
- [ ] Element bin (composite sub-pipeline)
- [ ] Pad blocking / probes (buffer interception)
- [ ] Segment seeking (timestamp-based clipping)

---

## Phase 9 — Testing & CI

- [ ] Docker Compose for multi-service testing (e.g. v4l2loopback)
- [ ] CI pipeline (GitHub Actions): build, unit test, docker build, integration test
- [ ] Caps negotiation fuzzing
- [ ] Event bus stress test
- [ ] Queue element stress test
- [ ] Clock precision test
- [ ] Static analysis: `cppcheck`, `clang-tidy`
- [ ] Valgrind memory leak checks in CI

---

## Phase 10 — Documentation

- [ ] API reference docs (Doxygen)
- [ ] Tutorial: "Recording a webcam to MP4 in 5 steps"
- [ ] Caps negotiation deep-dive
- [ ] Event bus patterns
- [ ] Allocator + zero-copy guide
- [ ] Clock and A/V sync guide
- [ ] Queue element threading model explainer
- [ ] Plugin authoring guide
