# Phase 10 — Documentation

## 1. Doxygen Setup & Integration
- [x] Configure `Doxyfile` to output HTML/LaTeX docs.
- [x] Ensure public API header files are fully documented (`include/` directory).
- [x] Integrate Doxygen build into `CMakeLists.txt` via a `docs` target.
- [x] Configure GitHub Actions to auto-generate and deploy Doxygen HTML output to GitHub Pages on merges to main.

## 2. Tutorials
- [x] **Getting Started:** "Recording a webcam to MP4 in 5 steps". This should walk through initialization, creating elements (`v4l2_source`, `h264_encoder`, `mp4_muxer`, `file_sink`), linking pads, starting the pipeline, and graceful shutdown on EOS.
- [x] **Network Streaming:** "Streaming live video over RTSP". Guide setting up the `rtsp_server` and `rtsp_source`/`rtsp_sink`.

## 3. Architecture Deep-Dives
- [ ] **Caps Negotiation:** Deep dive into how pads resolve format intersection, structure fields, and dynamic reconfiguration.
- [ ] **Event Bus Patterns:** Explore `zst_bus_t`, handling errors, EOS propagation, state changes, and segment messages asynchronously.
- [ ] **Allocator + Zero-Copy Guide:** How `zst_allocator_t` and buffer pools prevent memory allocation during `PLAYING` state. Describe `zst_allocator_dmabuf_t` and device-specific memory.
- [ ] **Clock and A/V Sync Guide:** Detailed mechanics of `zst_clock_t`, master/slave relationships, and how the scheduler handles `ZST_FLOW_DROPPED` for late frames.
- [ ] **Queue Threading Model Explainer:** How `zst_queue_t` decouple pipeline branches with thread pools and atomic ref-counting.

## 4. Plugin Authoring
- [ ] **Plugin Authoring Guide:** Steps to implement a custom element `zst_element_ops_t`, property registration, capabilities setup, and dynamic loading via `zst_plugin_registry_t`.
- [ ] **Testing Plugins:** How to properly write tests in `tests/test_core.c` using standard mock sources/sinks and ensuring the plugin builds gracefully.