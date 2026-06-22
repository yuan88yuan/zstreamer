# Implementation Plan

All phases are now documented in separate files for easier maintenance.

| Phase | Document | Lines | Status |
|-------|----------|-------|--------|
| 0–3   | [Core Framework](phase-core.md) | ~190 | ✅ Complete |
| 4     | [Element Implementations](phase-elements.md) | ~450 | ✅ Done (34 implemented, including V4L2 Sink) + planned additions |
| 5–7   | [Infrastructure](phase-infrastructure.md) | ~58 | ✅ Complete |
| 8     | [Advanced Features](phase-advanced.md) | ~417 | 🔄 In Progress |
| 9     | [Testing & CI](phase-testing-ci.md) | ~10 | 🔄 In Progress |
| 10    | [Documentation](phase-documentation.md) | ~25 | ✅ Complete |
| Post-P0 | [RTMP Hardening](phase-rtmp-hardening.md) | ~60 | ✅ Done |
| Post-P0 | [RTSP Media-On-Demand](phase-rtsp-mod.md) | ~70 | ✅ Done |
| Post-P0 | [Xilinx VCU Integration](phase-xilinx-vcu.md) | ~30 | ⬜ Planned |
| Post-P0 | [V4L2 DMA-BUF Exporter](phase-v4l2-expbuf.md) | ~110 | ✅ Implemented |
| Post-P0 | [Intel oneAPI Video Encoder](phase-elements.md#4ag--intel-oneapi-video-encoder) | ~20 | ✅ Done |
| Post-P0 | [VA-API Video Encoder](phase-elements.md#4ah--va-api-video-encoder) | ~20 | ✅ Done |
| Post-P0 | [VA-API Video Decoder](phase-elements.md#4ai--va-api-video-decoder-vaapidec) | ~20 | ⬜ Planned |
| Post-P0 | VA-API Encoder DMABUF zero-copy import path | ~TBD | ✅ Done |
| Cross-Compile | [ARM64 Cross-Compilation](cross-compilation.md) | ~50 | ✅ Done |
| Bugfix  | [Clock Sync Debug](clock-sync-debug.md) | ~200 | ✅ Fixed — scheduler clock sync comparison was broken |

---

## Quick Status

| Area | Status | Notes |
|------|--------|-------|
| Scaffolding (0) | ✅ Done | CMake, Docker, git, AGENTS.md |
| Core Framework (1) | ✅ Done | All 8 core modules |
| Scheduler (2) | ✅ Done | Topological sort, push/pull, EOS |
| Queue Element (3) | ✅ Done | First-class queue with worker thread |
| Logging (3.5) | ✅ Done | Compile-time log levels, thread-safe |
| Elements (4) | ✅ Done + planned additions | 34 elements implemented; HTTP Source and V4L2 Sink done |
| Caps Negotiation (5) | ✅ Done | Intersection, auto-negotiation |
| Event Bus (6) | ✅ Done | Error/state/EOS notifications |
| Dynamic Plugins (7) | ✅ Done | dlopen-based loading |
| Allocator API (8a) | ✅ Done | Pool + elements migration done; comprehensive pool unit tests completed |
| Clock (8b) | ✅ Done | System clock, pipeline integration |
| Testing & CI (9) | 🔄 In Progress | CI pipeline, stress tests, static analysis |
| Documentation (10) | ✅ Done | Doxygen API ref, tutorials, deep-dives, plugin guide |
| Advanced Features (8c) | ✅ Done | Element bin, pad probes, segment seeking |
| Element Public API (8d) | ✅ Done | Descriptor ABI, plugin introspection, typed properties, official element metadata, convenience headers, library & installation layout |

---

## RTMP Source/Sink Hardening  (Post-P0)

Detailed tasks and checklist for RTMP Source/Sink Hardening have been moved to a separate document:
- [RTMP Hardening Plan & Status](phase-rtmp-hardening.md)


## RTSP Server Media-On-Demand (Post-P0)

Detailed tasks and checklist for RTSP Server Media-On-Demand refactoring have been moved to a separate document:
- [RTSP Media-On-Demand Refactoring Plan](phase-rtsp-mod.md)


## V4L2 DMA-BUF Exporter (Post-P0)

Detailed tasks and checklist for V4L2 source element DMA-BUF exporter support:
- [V4L2 DMA-BUF Exporter Plan](phase-v4l2-expbuf.md)
