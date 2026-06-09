# Core Framework — Phases 0–3

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
