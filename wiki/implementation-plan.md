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

## Phase 3.5 — Logging System  (✅ done)

A lightweight, thread-safe logging subsystem replacing ad-hoc `printf` calls.
Provides compile-time-strippable, category-tagged log macros with runtime level
filtering, source location capture, and custom output handler support.

| Component   | Status | Notes                                                      |
|-------------|--------|------------------------------------------------------------|
| zst_log     | ✅ done  | Log levels (ERROR..TRACE), colour output, compile-time strip |

- [x] `ZST_LOG_ERROR` / `ZST_LOG_WARN` / `ZST_LOG_INFO` / `ZST_LOG_DEBUG` / `ZST_LOG_TRACE` macros
- [x] Compile-time ceiling (`ZST_LOG_LEVEL` define; debug builds default TRACE, release WARNING)
- [x] Runtime level filter (`zst_log_set_level` / `zst_log_get_level`)
- [x] Default handler: stderr with HH:MM:SS.mmm timestamp and ANSI colour when connected to a TTY
- [x] Custom handler callback (`zst_log_set_handler`)
- [x] Source location capture (`__FILE__`, `__LINE__`, `__func__`)
- [x] Category tag per message (e.g. `"v4l2src"`, `"alsasrc"`)
- [x] Thread-safe output via `pthread_mutex`
- [x] All existing `printf`-based diagnostics migrated to `ZST_LOG_*`

**Test deliverables:** ✅ Unit tests for level filtering, compile-time strip, custom handler, thread safety

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

### 8a — Allocator API  (from `wiki/future.md`)  (✅ done)
- [x] `zst_allocator_t` interface: `alloc`, `free`, ref-counting
- [x] Default CPU allocator (malloc/free)
- [ ] DMABUF allocator (Linux dma-buf)
- [ ] CUDA / Vulkan device memory allocators
- [ ] Buffer pools to eliminate per-frame allocation

  **`zst_buffer_pool_t` — a recyclable pool of pre-allocated buffers**

  Every source element currently calls `zst_buffer_create()` per frame (see
  `v4l2_source.c`, `alsa_source.c`, `video_scaler.c`, etc.). A buffer pool
  pre-allocates a set of buffers upfront and recycles them, eliminating
  `malloc`/`free` overhead on every frame.

  **Data structure and lifecycle:**
  - [x] `zst_buffer_pool_t` struct with a LIFO/freelist of buffers
  - [x] Backed by a `zst_allocator_t` — pool allocates new buffers via allocator
  - [x] Config: `min_buffers`, `max_buffers`, `buffer_size`, `buffer_type`
  - [x] Thread-safe acquire/release via `pthread_mutex` + `pthread_condvar`
  - [x] Watermark callbacks: low-watermark triggers pre-fill, high-watermark triggers drain
  - [x] `zst_buffer_pool_create(allocator, config)` / `_destroy()` / `_flush()`

  **Acquire / release API:**
  - [x] `zst_buffer_pool_acquire(pool, timeout_ms)` — returns a buffer from the pool;
        blocks if empty until a buffer is returned or timeout expires
  - [x] `zst_buffer_pool_release(pool, buf)` — returns the buffer to the pool;
        resets refcount to 1, clears flags/metadata (but keeps underlying memory for reuse)
  - [ ] Optional non-blocking acquire with `ZST_POOL_ACQUIRE_NONBLOCK` flag
  - [x] On release: if pool is at capacity, actually free the buffer instead of recycling

  **Integration with zst_buffer:**
  - [x] `zst_buffer_t` gets an optional `pool` back-pointer (or reuse `memory.priv`)
  - [x] `zst_buffer_create_with_pool(pool)` — acquire from pool instead of malloc
  - [x] `zst_buffer_unref()` checks for pool back-pointer: if pool is set, call
        `pool->release(buf)` instead of `free`; otherwise normal free path
  - [x] Pool buffers skip the `destroy` callback on recycle (only called on final unref when
        pool itself is destroyed)

  **Usage in elements (migration):**
  - [x] `v4l2_source`: allocate pool during `open()`, acquire per-frame in process()
        instead of `zst_buffer_create()`; release happens automatically on `unref`
  - [x] `alsa_source`: same pattern for audio frames
  - [x] `video_scaler`: pool for output buffers
  - [x] `audio_resampler`: pool for output buffers
  - [x] `h264_encoder` / `aac_encoder`: packet pool for encoded output
  - [ ] `queue_element`: optionally attach pool to queue — return consumed buffers
        to the upstream pool automatically

  **Auto-configuration from caps:**
  - [ ] `zst_buffer_pool_config_from_caps(caps)` — derive `buffer_size` from
        resolution × pixel format (video) or sample_rate × channels × format (audio)
  - [ ] Default pool sizing: `min_buffers` = number of queue elements in pipeline + 2
        (so there's always a spare buffer circulating)

  **Test deliverables:**
  - [ ] Unit test: acquire/recycle loop (N buffers, M cycles, no net allocation)
  - [ ] Unit test: acquire blocks when pool exhausted, unblocks on release
  - [ ] Unit test: acquire with timeout returns NULL on expiry
  - [ ] Unit test: pool-backed buffer unref returns buffer to pool
  - [ ] Unit test: pool flush frees all cached buffers
  - [ ] Integration test: `v4l2src → queue → filesink` with pool, verify zero
        calls to `malloc` after warm-up phase

### 8b — Clock  (from `wiki/future.md`)  (✅ done)
- [x] `zst_clock_t` interface: `get_time`, `wait`
- [x] System clock wrapping `CLOCK_MONOTONIC`
- [x] Pipeline-level master clock selection
- [ ] Clock slaving for A/V sync — see [`wiki/clock-slaving.md`](clock-slaving.md)
  for detailed design and task breakdown

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

---

## Phase 11 — Text Rendering  (✅ done 11a)

A **text overlay element** that composites text (subtitles, timestamps, labels) onto
raw video frames. Follows the same element pattern as other processing elements:
single sink pad (raw video in), single src pad (raw video with text out).

### 11a — Text Overlay Element

- [x] `text_overlay` element with 1 sink pad (video/x-raw) + 1 src pad (video/x-raw)
- [x] Configurable text string (via element property or secondary text sink pad)
- [x] Backend: `libfreetype` for font rasterization (glyph bitmap generation)
- [x] Text layout: multi-line support with word wrapping
- [x] Configurable font family, size, colour, outline/shadow
- [x] Configurable position: absolute (x, y) or relative (centre, top-left, bottom-right)
- [x] Alpha blending of text bitmap onto YUV420P / NV12 frames
- [x] PTS passthrough (text overlay preserves video timestamps)
- [x] EOS passthrough
- [x] Caps negotiation: accept/caps on sink pad, same caps on src pad (passthrough)

**Dependencies:** `libfreetype-dev` (added to Dockerfile)

### 11b — Text Source Element (stretch goal)

- [ ] `text_source` element: generates video frames with rendered text (no video input)
- [ ] Useful for test patterns, title cards, and simple slideshows
- [ ] Configurable resolution, framerate, text content, background colour

### 11c — SRT Subtitle Parser (stretch goal)

- [ ] Parse SRT subtitle format into timed text events
- [ ] Feed parsed text segments to `text_overlay` at correct PTS
- [ ] Support ASS/SSA format parsing (advanced styling)

**Test deliverables:**
- [x] Unit test: render text onto a known frame, verify pixels at expected positions
- [x] Unit test: multi-line text wrapping
- [x] Unit test: EOS passthrough
- [x] Unit test: caps negotiation
- [x] Unit test: property get/set for font size, colour, position
- [x] Integration test: `v4l2src → text_overlay → filesink` produces video with visible text
