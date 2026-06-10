# Implementation Plan

All phases are now documented in separate files for easier maintenance.

| Phase | Document | Lines | Status |
|-------|----------|-------|--------|
| 0–3   | [Core Framework](phase-core.md) | ~190 | ✅ Complete |
| 4     | [Element Implementations](phase-elements.md) | ~290 | ✅ 4a-4i, 4l, 4n, 4s done; 📝 4j-4k, 4m, 4o-4r, 4t-4u planned |
| 5–7   | [Infrastructure](phase-infrastructure.md) | ~58 | ✅ Complete |
| 8     | [Advanced Features](phase-advanced.md) | ~115 | 🔄 In Progress |
| 9–10  | [Future Work](phase-future.md) | ~34 | ⬜ Not Started |

---

## Quick Status

| Area | Status | Notes |
|------|--------|-------|
| Scaffolding (0) | ✅ Done | CMake, Docker, git, AGENTS.md |
| Core Framework (1) | ✅ Done | All 8 core modules |
| Scheduler (2) | ✅ Done | Topological sort, push/pull, EOS |
| Queue Element (3) | ✅ Done | First-class queue with worker thread |
| Logging (3.5) | ✅ Done | Compile-time log levels, thread-safe |
| Elements (4) | ✅ 4a-4i, 4l, 4n, 4s done | 12 elements implemented; 4j-4k, 4m, 4o-4r, 4t-4u planned |
| Caps Negotiation (5) | ✅ Done | Intersection, auto-negotiation |
| Event Bus (6) | ✅ Done | Error/state/EOS notifications |
| Dynamic Plugins (7) | ✅ Done | dlopen-based loading |
| Allocator API (8a) | ✅ Mostly done | Pool + elements migration done; default sizing & tests pending |
| Clock (8b) | ✅ Done | System clock, pipeline integration |
| Testing & CI (9) | ⬜ Planned | CI pipeline, stress tests, static analysis |
| Documentation (10) | ⬜ Planned | Doxygen API ref, tutorials |
| Advanced Features (8c) | 📝 Planned | Element bin, pad probes, segment seeking |

