# Future Work — Phases 9–10

## Phase 9 — Testing & CI

- [x] Docker Compose for multi-service testing (e.g. v4l2loopback)
- [x] CI pipeline (GitHub Actions): build, unit test, docker build, integration test
- [ ] Caps negotiation fuzzing
- [x] Event bus stress test
- [x] Queue element stress test
- [x] Clock precision test
- [ ] Static analysis: `cppcheck`, `clang-tidy`
- [ ] Valgrind memory leak checks in CI

---

## Phase 10 — Documentation

- [ ] API reference docs (Doxygen)
- [ ] Tutorial: "Recording a webcam to MP4 in 5 steps"
- [ ] Caps negotiation deep-dive
- [ ] Event bus patterns
- [ ] Allocator + zero-copy guide
- [ ] Clock and A/V sync guide
- [ ] Queue element threading model explainer
- [ ] Plugin authoring guide
