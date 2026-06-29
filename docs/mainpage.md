# zstreamer Documentation

## Introduction

zstreamer is a lightweight, modular multimedia streaming and pipeline framework written in **C11**.
It implements a **GStreamer-inspired** architecture: elements connected via pads,
data flowing as reference-counted buffers through thread-safe queues, driven by a
configurable scheduler.

## Key Features

- **GStreamer-like pipeline model** — elements, pads, buffers, queues, caps negotiation
- **Thread-safe design** — bounded queues, atomic ref-counting, multi-threaded scheduler
- **50+ built-in element types** — sources, sinks, codecs, muxers, demuxers, filters, network I/O
- **Dynamic plugins** — `dlopen`-based element loading at runtime
- **Async event bus** — error, EOS, state-change, and warning notifications
- **Clock & A/V sync** — system clock, QoS dropping, clock slaving
- **Adaptive Stream Demuxing** — dynamic pads, stream info queries, and safe runtime graph reconfiguration
- **Hardware acceleration** — CUDA, Vulkan, oneAPI (SYCL), VA-API, NVIDIA V4L2, DMABUF
- **Zero-copy by default** — reference-counted buffers with typed memory backends (CPU, DMABUF, CUDA, Vulkan, oneAPI)
- **Lightweight logging** — compile-time level filtering with custom handler support
- **Network protocols** — RTSP (server + client), RTMP, SRT, HTTP, raw TCP/UDP

## API Reference

The core framework headers provide the building blocks for all pipeline applications:

| Header | Purpose |
|--------|---------|
| @ref zst_types.h | Base types, result codes, forward declarations |
| @ref zst_buffer.h | Reference-counted data carrier with typed memory |
| @ref zst_pad.h | SRC/SINK connection pads, probes, blocking, segments |
| @ref zst_element.h | Element ops vtable, state machine, property API |
| @ref zst_bin.h | Composite element containers and ghost pads |
| @ref zst_pipeline.h | Pipeline container with state propagation |
| @ref zst_queue.h | Thread-safe bounded buffer queue |
| @ref zst_scheduler.h | Single / multi-thread pipeline driver |
| @ref zst_bus.h | Async event bus for error/EOS/state/segment notifications |
| @ref zst_caps.h | Caps negotiation — media types, resolution, format intersection |
| @ref zst_segment.h | Segment seeking / clipping |
| @ref zst_clock.h | Clock and A/V sync |
| @ref zst_allocator.h | Memory allocator interface (CPU, DMABUF, CUDA, Vulkan, oneAPI) |
| @ref zst_buffer_pool.h | Pre-allocated buffer recycling |
| @ref zst_plugin.h | Dynamic plugin loading (dlopen) |
| @ref zst_log.h | Lightweight compile-time logging |
| @ref zst_stream.h | Stream tracking (dynamic pads, adaptive demuxing) |
| @ref zst_pad_event.h | In-band pad events (stream-start, caps, segment, EOS) |
| @ref zst_element_factory.h | Factory registration and element instantiation |
| @ref zst_rtsp_server.h | Multi-session RTSP server API |
| @ref zst_srt.h | SRT subtitle types |

For detailed guides, tutorials, and deep-dives see:
- [Getting Started Tutorial](@ref tutorials_getting_started)
- [Architecture Overview](@ref architecture_architecture)
- [Plugin Authoring Guide](@ref plugin_authoring)
- [Testing Plugins](@ref testing_plugins)
