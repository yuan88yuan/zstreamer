# AGENTS.md — zstreamer

## Project Overview

`zstreamer` is a lightweight, modular multimedia streaming/pipeline framework written in C11.  
It provides a **GStreamer-like** pipeline architecture: elements connected via pads, data flowing as reference-counted buffers through thread-safe queues, driven by a configurable scheduler.

**GitHub:** https://github.com/zzlee/zstreamer

---

## Directory Layout

```
.
├── AGENTS.md          ← This file — project context for AI coding agents
├── CMakeLists.txt     ← CMake build system
├── Dockerfile         ← Ubuntu 24.04 dev environment
├── .dockerignore
├── .gitignore
├── include/           ← Public API headers
│   ├── mm_types.h     ← Base types, result codes, struct forward decls
│   ├── mm_buffer.h    ← Reference-counted buffer + typed memory
│   ├── mm_pad.h       ← SRC/SINK connection pads
│   ├── mm_element.h   ← Element ops vtable + state machine
│   ├── mm_pipeline.h  ← Element container with state propagation
│   ├── mm_queue.h     ← Thread-safe bounded buffer queue
│   ├── mm_scheduler.h ← Single / multi-thread pipeline driver
│   └── mm_plugin.h    ← Dynamic plugin loading (dlopen)
├── src/               ← Core library + element implementations
│   ├── mm_buffer.c
│   ├── mm_pad.c
│   ├── mm_element.c
│   ├── mm_pipeline.c
│   ├── mm_queue.c
│   ├── mm_scheduler.c
│   ├── mm_plugin.c
│   ├── v4l2_source.c  ← V4L2 camera capture (stub)
│   ├── h264_encoder.c ← x264 H.264 encoder (stub)
│   ├── mp4_muxer.c    ← FFmpeg/libavformat MP4 muxer (stub)
│   ├── file_sink.c    ← FILE* writer (stub, open implemented)
│   ├── alsa_source.c  ← ALSA audio capture (stub)
│   └── aac_encoder.c  ← AAC audio encoder (stub)
├── tests/
│   ├── test_core.c    ← 15 unit tests: buffer, pad, element, pipeline, queue
│   └── example_record.c ← Full pipeline demo (links all elements)
└── wiki/
    ├── architecture.md        ← Detailed design doc
    ├── implementation-plan.md ← Step-by-step roadmap (10 phases)
    ├── pipeline-flow.md       ← Scheduler flow diagram
    └── future.md              ← Planned features with Chinese notes
```

---

## Build

```bash
# Native
mkdir build && cd build
cmake .. -DBUILD_TESTS=ON
make -j$(nproc)
ctest --output-on-failure

# Docker — one-shot test (fastest, uses cached build)
docker build -t zstreamer .
docker run --rm zstreamer                     # runs ctest --output-on-failure

# Docker — verbose test output
docker run --rm --entrypoint bash zstreamer \
    -c "/workspace/build/ctest -V"

# Docker — interactive shell (source + build tree available)
docker run --rm -it zstreamer bash            # starts in /workspace
# then: cd /workspace/build && ctest -V

# Docker — live code mount (edit on host, rebuild in container, no docker build needed)
docker run --rm -it \
    -v $(pwd):/workspace \
    zstreamer bash
# then: cd /workspace/build && cmake .. && make -j && ctest -V

# Docker — rebuild after source changes (cache-friendly)
docker build -t zstreamer . && docker run --rm zstreamer
```

### Build Options

| Option            | Default | Description                           |
|-------------------|---------|---------------------------------------|
| `BUILD_TESTS`     | ON      | Build unit tests                      |
| `BUILD_SHARED`    | OFF     | Build core as `.so` instead of `.a`   |
| `ENABLE_PLUGINS`  | ON      | Enable dlopen-based plugin loading    |

### Docker Targets

The Dockerfile has two build targets:

| Target | Command                                    | Purpose                       |
|--------|--------------------------------------------|-------------------------------|
| `ci`   | `docker run --rm zstreamer`                 | One-shot `ctest` (default)    |
| `dev`  | `docker run --rm -it zstreamer bash`        | Interactive shell with build  |

---

## Architecture

| Component      | Role                                                  |
|----------------|-------------------------------------------------------|
| **mm_pipeline**| Container of elements; propagates state to all        |
| **mm_element** | Processing node with src/sink pads + ops vtable       |
| **mm_pad**     | Connection point; linked peer-to-peer between elements|
| **mm_buffer**  | Ref-counted data carrier with typed memory + timestamps|
| **mm_queue**   | Thread-safe bounded queue (mutex + condvar)           |
| **mm_scheduler**| Drives pipeline: single-thread inline or multi-thread pool |
| **mm_plugin**  | `dlopen()`-based dynamic element loading              |

### State Machine

```
MM_STATE_NULL  ──open──→  MM_STATE_READY  ──start──→  MM_STATE_PLAYING
     ↑                        │                              │
     └────────close───────────┘               stop────────────┘
```

`MM_STATE_PAUSED` is reserved for future preroll support.

---

## Current Status

| Phase               | Status                           |
|----------------------|----------------------------------|
| Scaffolding          | ✅ CMake, Docker, git, AGENTS.md |
| Core Framework       | ✅ All 8 core modules implemented|
| Unit Tests           | ✅ 18 tests, all passing         |
| Scheduler Integration| ✅ Topological sort, wiring, auto-queues, EOS |
| Element Stubs        | 📝 Implementations needed for v4l2, x264, mp4, file |
| Caps Negotiation     | 📝 Future                        |
| Dynamic Plugins      | 📝 Future                        |

---

## Coding Conventions

- **Language:** C11 (`-std=c11`)
- **Naming:** `mm_` prefix for all public symbols, `snake_case`
- **Error handling:** Return `mm_result_t` — `MM_OK` (0) on success, negative on error
- **Ownership:** Buffers are ref-counted; elements own their pads; pipeline owns elements
- **Thread safety:** Queue is MT-safe; buffer refcount is atomic; element/pipeline ops are NOT thread-safe (serialised by scheduler)

## Key Files for Agent Context

When working on this project, the most important files to read first:

1. `include/mm_types.h` — All forward declarations and error codes
2. `include/mm_buffer.h` — Buffer structure (used everywhere)
3. `wiki/architecture.md` — Full architectural understanding
4. `wiki/implementation-plan.md` — What's done and what's next
5. `CMakeLists.txt` — Build targets and dependencies
