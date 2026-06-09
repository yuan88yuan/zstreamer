# Implementation Plan

## Phase 0 — Project Scaffolding  (✅ done)

- [x] AGENTS.md with project overview and build instructions
- [x] .gitignore for build artifacts and IDE files
- [x] Git repository initialised and pushed to GitHub
- [x] CMakeLists.txt with core library + test targets
- [x] Dockerfile with build dependencies (ubuntu 24.04, cmake, pthreads, multimedia libs)

---

## Phase 1 — Core Framework  (✅ done)

The fundamental types and lifecycle management. Everything after this can be tested with unit tests.

| Component   | Status | Notes                                                      |
|-------------|--------|------------------------------------------------------------|
| mm_types    | ✅ done  | Base types, error codes, forward declarations               |
| mm_buffer   | ✅ done  | Ref-counted buffer with typed memory; destroy callback      |
| mm_pad      | ✅ done  | SRC/SINK pads with peer linking; caps stub                  |
| mm_element  | ✅ done  | Ops vtable, state machine (NULL/READY/PAUSED/PLAYING), pads |
| mm_pipeline | ✅ done  | Element container, state propagation                        |
| mm_queue    | ✅ done  | Thread-safe bounded queue (mutex + condvar), timeout push/pop |
| mm_scheduler| ✅ done  | Single-thread + multi-thread worker pool                    |
| mm_plugin   | ✅ done  | dlopen loader, plugin entry point                           |
| test_core   | ✅ done  | 15 unit tests covering all core components                  |

**Current build status:** `cmake .. && make && ctest` — all tests pass.

---

## Phase 2 — Scheduler Integration & Pipeline Wiring (✅ done)

Connect the dots between the scheduler and the element graph with an internal queueing model (queues auto-inserted by the scheduler).

- [x] **Pad push/pull semantics**: wire element `process()` into pad push/pull callbacks
- [x] **Scheduler element iteration**: walk the pipeline in topological order, assign element chains to worker threads
- [x] **Queue auto-insertion**: automatically insert queues between linked elements when multi-thread scheduler is used
- [x] **State machine hardening**: validate transitions, handle error rollback
- [x] **EOS signalling**: propagate end-of-stream through the pipeline

**Test deliverables:**
- [x] Simple pipeline with 2–3 mock elements feeding buffers end-to-end
- [x] Multi-thread stress test with queue back-pressure

---

## Phase 3 — Queue Element  (from `wiki/future.md`)

Refactor from internal/invisible queues to **first-class queue elements** — like GStreamer's `queue` element. This changes the threading model: users explicitly insert queue elements at pipeline boundaries.

```
v4l2src → queue → h264enc → queue → mp4mux → queue → filesink
          ^^^^^              ^^^^^            ^^^^^
      explicit boundary  explicit boundary  explicit boundary
```

- [ ] Implement `mm_queue_element` — a full `mm_element` with one sink pad + one src pad, backed by a `mm_queue_t` internally
- [ ] Remove scheduler auto-queue-insertion; users own their threading boundaries
- [ ] Queue element lifetime hooks: `open` initialises the queue, `close` flushes it, `start/stop` manage the worker thread
- [ ] Propagate EOS through the queue element (flush on EOS)
- [ ] Configurable via `mm_queue_config_t` (max buffers, bytes, duration)
- [ ] **Non-blocking mode**: `MM_QUEUE_ASYNC` drops buffers instead of blocking when full

**Why this matters:** The queue element is the foundation of the threading model. Every queue boundary is a potential thread switch. Explicit queues give users control over buffering, back-pressure, and thread placement.

---

## Phase 4 — Real Element Implementations

Replace stubs with working hardware/codec integration.

### 4a — V4L2 Source
- [ ] Open video device (`/dev/video0`)
- [ ] Enumerate formats and resolutions
- [ ] `VIDIOC_STREAMON` / `VIDIOC_STREAMOFF`
- [ ] Capture frames into mm_buffer
- [ ] Handle non-blocking I/O (poll + epoll)

**Dependencies:** `libv4l2` (in Docker)

### 4b — H.264 Encoder
- [ ] Integrate x264 library
- [ ] Accept raw NV12/YUV420P frames
- [ ] Emit H.264 annex‑B packets as mm_buffer
- [ ] Rate control (CBR, CRF)

**Dependencies:** `libx264-dev`

### 4c — MP4 Muxer
- [ ] Use FFmpeg `libavformat` / `libavcodec`
- [ ] Write MP4 with proper moov box (moov at end or faststart)
- [ ] Handle video + audio interleaving

**Dependencies:** `libavformat-dev`, `libavcodec-dev`, `libavutil-dev`

### 4d — File Sink
- [ ] Debugged — basic FILE* write already in place
- [ ] Add fwrite of buffer data

### 4e — Audio Source (ALSA)
- [ ] Capture from `hw:0` via `libasound`

**Dependencies:** `libasound2-dev`

### 4f — AAC Encoder
- [ ] Integrate FDK-AAC or FFmpeg AAC encoder
- [ ] Accept PCM frames, emit ADTS/LATM packets

---

## Phase 5 — Caps Negotiation  (from `wiki/future.md`)

Arguably the most important missing piece. Without caps negotiation, the pipeline can't verify or convert between formats. Elements must advertise what they produce (src caps) and what they consume (sink caps), and adjacent pads must agree before linking.

- [ ] **`mm_caps_t` structure**: a list of structures each describing:
  - `media_type` (e.g. `"video/x-raw"`, `"video/x-h264"`)
  - Video: `width`, `height`, `framerate`, `pixel_format` (NV12, YUV420P, etc.)
  - Audio: `channels`, `sample_rate`, `format` (S16LE, F32LE, etc.)
- [ ] **Caps intersection**: `mm_caps_intersect(src_caps, sink_caps) → mm_caps_t*` — find the highest-priority overlapping format
- [ ] **Pad caps API**: `mm_pad_set_caps()`, `mm_pad_get_caps()`, `mm_pad_negotiate()`
- [ ] **Auto-negotiation at link time**: when `mm_pad_link()` is called, automatically negotiate compatible caps
- [ ] **Caps-query mechanism**: element ops vtable gains `get_caps(pad, direction)` so sources can report what they produce, sinks what they accept
- [ ] **Conversion elements**: implement format converters (e.g. NV12 ↔ YUV420P, S16LE ↔ F32LE, 48000 ↔ 44100 sample rate) as built-in helper elements or auto-inserted converters

**Why this matters:** Without caps, a user can link a V4L2 source producing NV12 to an encoder expecting YUV420P and get silent garbage. Caps negotiation makes the pipeline self-describing and safe.

---

## Phase 6 — Event Bus  (from `wiki/future.md`)

An async notification system that decouples error/state/EOS signalling from the data path.

```
Application               Event Bus
    │                         │
    ├── mm_bus_attach() ──────┤
    │                         │
    │         ┌─ Element 1 ───┤── MM_EVENT_EOS
    │         │               │
    │─── Pipeline ─ Element 2 ┤── MM_EVENT_ERROR
    │         │               │
    │         └─ Element 3 ───┤── MM_EVENT_STATE_CHANGED
    │                         │
    └── mm_bus_pop() ─────────┘
```

**Event types:**

| Event                   | Payload                           |
|-------------------------|-----------------------------------|
| `MM_EVENT_EOS`          | element name                      |
| `MM_EVENT_ERROR`        | error code + message string       |
| `MM_EVENT_STATE_CHANGED`| old_state, new_state, element     |
| `MM_EVENT_WARNING`      | message string                    |

- [ ] `mm_bus_t` — thread-safe event queue (can reuse `mm_queue_t` internally)
- [ ] `mm_bus_post(bus, event)` — called by elements/pipeline
- [ ] `mm_bus_pop(bus, timeout_ms)` — called by application (sync wait)
- [ ] `mm_bus_set_handler(bus, callback, user_data)` — async callback dispatch
- [ ] Wire pipeline lifecycle: post `MM_EVENT_STATE_CHANGED` on every state transition
- [ ] Wire element `process()`: if it returns `MM_EOF`, the pipeline posts `MM_EVENT_EOS`
- [ ] Wire error returns: `MM_ERROR` → `MM_EVENT_ERROR` on the bus

**Why this matters:** Currently errors and state changes are communicated through return codes that the application must poll. The event bus lets applications listen asynchronously — critical for GUI apps, reactive systems, and clean error recovery.

---

## Phase 7 — Dynamic Plugins

- [ ] Build each element as a separate `.so`
- [ ] Plugin discovery path (`ZSTREAMER_PLUGIN_PATH` env var, `/usr/lib/zstreamer/`)
- [ ] Ref-counted plugin registry
- [ ] Plugin versioning and compatibility checks

---

## Phase 8 — Advanced Features

### 8a — Allocator API  (from `wiki/future.md`)
Essential for zero-copy. Buffers must be able to come from custom memory pools (GPU, DMABUF, pre-allocated DMA buffers) instead of always malloc'd CPU memory.

- [ ] `mm_allocator_t` interface:
  - `alloc(size, alignment) → mm_memory_t`
  - `free(mm_memory_t*)`
  - `mm_allocator_ref()` / `mm_allocator_unref()`
- [ ] Default CPU allocator (wraps malloc/free)
- [ ] DMABUF allocator (linux `dma-buf`)
- [ ] CUDA allocator (cudaMalloc / cudaFree)
- [ ] Vulkan allocator (vkAllocateMemory)
- [ ] Integrate with `mm_buffer_create_from_allocator()` — buffer takes ownership of allocator-allocated memory
- [ ] **Buffer pool**: pre-allocate a pool of buffers from an allocator to avoid per-frame malloc churn

### 8b — Clock  (from `wiki/future.md`)
The A/V sync core. A master clock that drives playback timing.

- [ ] `mm_clock_t` interface:
  - `mm_clock_get_time(clock) → mm_time_t` (nanosecond precision)
  - `mm_clock_wait(clock, target_time)` — block until target
- [ ] **Default system clock**: wraps `clock_gettime(CLOCK_MONOTONIC)`
- [ ] **Pipeline clock**: pipeline picks a master clock; all elements synchronise to it
- [ ] **Clock slaving**: slave clock adjusts to master (like GstClock's `MM_CLOCK_OPTION_SLAVE`)
- [ ] **Jitter measurement**: track how far off elements are from the clock

### 8c — Other Advanced Features
- [ ] **Element bin**: composite element that contains a sub-pipeline (for reusability)
- [ ] **Pad blocking / probes**: intercept buffers flowing through a pad for analysis, tee, or injection (like GstPad probes)
- [ ] **Segment seeking**: timestamp-based segment clipping

---

## Phase 9 — Testing & CI

- [ ] **Docker Compose** for multi-service testing (e.g. virtual V4L2 loopback via `v4l2loopback`)
- [ ] **CI pipeline** (GitHub Actions): build, unit test, docker build, integration test
- [ ] **Caps negotiation fuzzing**: test all possible format combinations
- [ ] **Event bus stress test**: thousands of events posted from multiple threads
- [ ] **Queue element stress test**: multiple producers/consumers, verify no deadlocks
- [ ] **Clock precision test**: measure jitter under load
- [ ] **Static analysis**: `cppcheck`, `clang-tidy`
- [ ] **Valgrind**: memory leak checks in CI
- [ ] **Fuzz testing**: `mm_queue_push/pop` with random timeouts and multi-thread interleaving

---

## Phase 10 — Documentation

- [ ] API reference docs (Doxygen)
- [ ] Tutorial: "Recording a webcam to MP4 in 5 steps"
- [ ] Caps negotiation deep-dive
- [ ] Event bus patterns: error handling, async monitoring
- [ ] Allocator + zero-copy guide
- [ ] Clock and A/V sync guide
- [ ] Queue element threading model explainer
- [ ] Performance tuning guide
- [ ] Plugin authoring guide
