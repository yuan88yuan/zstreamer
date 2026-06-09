# Implementation Plan

All phases are now documented in separate files for easier maintenance.

| Phase | Document | Lines | Status |
|-------|----------|-------|--------|
| 0–3   | [Core Framework](phase-core.md) | ~190 | ✅ Complete |
| 4     | [Element Implementations](phase-elements.md) | ~90 | ✅ Complete |
| 5–7   | [Infrastructure](phase-infrastructure.md) | ~58 | ✅ Complete |
| 8     | [Advanced Features](phase-advanced.md) | ~115 | 🔄 In Progress |
| 9–11  | [Future Work](phase-future.md) | ~66 | ⬜ Not Started |

---

## Quick Status

| Area | Status | Notes |
|------|--------|-------|
| Scaffolding (0) | ✅ Done | CMake, Docker, git, AGENTS.md |
| Core Framework (1) | ✅ Done | All 8 core modules |
| Scheduler (2) | ✅ Done | Topological sort, push/pull, EOS |
| Queue Element (3) | ✅ Done | First-class queue with worker thread |
| Logging (3.5) | ✅ Done | Compile-time log levels, thread-safe |
| Elements (4) | ✅ Done | 8 elements: V4L2, x264, ALSA, AAC, etc. |
| Caps Negotiation (5) | ✅ Done | Intersection, auto-negotiation |
| Event Bus (6) | ✅ Done | Error/state/EOS notifications |
| Dynamic Plugins (7) | ✅ Done | dlopen-based loading |
| Allocator API (8a) | ✅ Done | CPU allocator + buffer pools + non-blocking acquire |
| Clock (8b) | ✅ Done | System clock, pipeline integration |
| Text Rendering (11a) | ✅ Done | Text overlay via libfreetype |
| Testing & CI (9) | ⬜ Planned | CI pipeline, stress tests, static analysis |
| Documentation (10) | ⬜ Planned | Doxygen API ref, tutorials |
| Advanced Features (8c) | ⬜ Planned | Element bin, probes, seeking |
| Text Source (11b-c) | ⬜ Stretch | Text source element, SRT parser |
