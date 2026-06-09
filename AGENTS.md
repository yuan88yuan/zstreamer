# AGENTS.md — zstreamer

## Project Overview

`zstreamer` is a lightweight, modular multimedia streaming/pipeline framework written in C.  
It provides a GStreamer-like pipeline architecture with elements, pads, buffers, queues, and a scheduler.

## Directory Layout

- `include/` — Public header files (types, buffer, pad, element, pipeline, queue, scheduler, plugin)
- `src/` — Element implementations (v4l2 source, h264 encoder, mp4 muxer, file sink)
- `tests/` — Example/test programs
- `wiki/` — Design notes and documentation

## Build

```bash
mkdir build && cd build
cmake ..
make
```

*(or the project's preferred build system)*

## Architecture

- **mm_pipeline** — Container of elements
- **mm_element** — Processing node with src/sink pads
- **mm_pad** — Connection point for linking elements
- **mm_buffer** — Data buffer (video/audio frames, packets)
- **mm_queue** — Thread-safe buffer queue (sync/async)
- **mm_scheduler** — Drives the pipeline (single or multi-threaded)
- **mm_plugin** — Dynamic plugin loading

## State Machine

`MM_STATE_NULL → READY → PAUSED → PLAYING`
