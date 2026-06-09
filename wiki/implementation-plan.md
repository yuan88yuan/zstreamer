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

## Phase 2 — Scheduler Integration & Pipeline Wiring

Connect the dots between the scheduler and the element graph.

- [ ] **Pad push/pull semantics**: wire element `process()` into pad push/pull callbacks
- [ ] **Scheduler element iteration**: walk the pipeline in topological order, assign element chains to worker threads
- [ ] **Queue auto-insertion**: automatically insert queues between linked elements when multi-thread scheduler is used
- [ ] **State machine hardening**: validate transitions, handle error rollback
- [ ] **EOS signalling**: propagate end-of-stream through the pipeline

**Test deliverables:**
- Simple pipeline with 2–3 mock elements feeding buffers end-to-end
- Multi-thread stress test with queue back-pressure

---

## Phase 3 — Real Element Implementations

Replace stubs with working hardware/codec integration.

### 3a — V4L2 Source
- [ ] Open video device (`/dev/video0`)
- [ ] Enumerate formats and resolutions
- [ ] `VIDIOC_STREAMON` / `VIDIOC_STREAMOFF`
- [ ] Capture frames into mm_buffer
- [ ] Handle non-blocking I/O (poll + epoll)

**Dependencies:** `libv4l2` (in Docker)

### 3b — H.264 Encoder
- [ ] Integrate x264 library
- [ ] Accept raw NV12/YUV420P frames
- [ ] Emit H.264 annex‑B packets as mm_buffer
- [ ] Rate control (CBR, CRF)

**Dependencies:** `libx264-dev`

### 3c — MP4 Muxer
- [ ] Use FFmpeg `libavformat` / `libavcodec`
- [ ] Write MP4 with proper moov box (moov at end or faststart)
- [ ] Handle video + audio interleaving

**Dependencies:** `libavformat-dev`, `libavcodec-dev`, `libavutil-dev`

### 3d — File Sink
- [ ] Debugged — basic FILE* write already in place
- [ ] Add fwrite of buffer data

### 3e — Audio Source (ALSA)
- [ ] Capture from `hw:0` via `libasound`

**Dependencies:** `libasound2-dev`

---

## Phase 4 — Caps Negotiation & Pad Pruning

Before linking, elements advertise what they can produce/consume.

- [ ] `mm_caps_t` structure: list of `{media_type, width, height, format, ...}`
- [ ] `mm_pad_negotiate()`: find compatible caps between src and sink
- [ ] Caps intersection (e.g. encoder accepts NV12 → muxer accepts NV12)

---

## Phase 5 — Dynamic Plugins

- [ ] Build each element as a separate `.so`
- [ ] Plugin discovery path (`ZSTREAMER_PLUGIN_PATH` env var, `/usr/lib/zstreamer/`)
- [ ] Ref-counted plugin registry

---

## Phase 6 — Advanced Features

- [ ] **Element bin**: composite element that contains sub-pipeline
- [ ] **Pad blocking / probes**: intercept buffers for analysis (like GstPad probes)
- [ ] **Segment seeking**: timestamp-based segment clipping
- [ ] **Custom allocators**: DMABUF, CUDA memory pools
- [ ] **Zero-copy GPU path**: import DMABUF → CUDA → encode → export

---

## Phase 7 — Testing & CI

- [ ] **Docker Compose** for multi-service testing (e.g. virtual V4L2 loopback)
- [ ] **CI pipeline** (GitHub Actions): build, unit test, docker build, integration test
- [ ] **Static analysis**: `cppcheck`, `clang-tidy`
- [ ] **Valgrind**: memory leak checks in CI
- [ ] **Fuzz testing**: `mm_queue_push/pop` with random timeouts and multi-thread interleaving

---

## Phase 8 — Documentation

- [ ] API reference docs (Doxygen)
- [ ] Tutorial: "Recording a webcam to MP4 in 5 steps"
- [ ] Performance tuning guide
- [ ] Plugin authoring guide
