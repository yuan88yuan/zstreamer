# Future Work — Phases 9–11

## Phase 9 — Testing & CI

- [ ] Docker Compose for multi-service testing (e.g. v4l2loopback)
- [ ] CI pipeline (GitHub Actions): build, unit test, docker build, integration test
- [ ] Caps negotiation fuzzing
- [ ] Event bus stress test
- [ ] Queue element stress test
- [ ] Clock precision test
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

---

## Phase 11 — Text Rendering  (✅ done 11a)

A **text overlay element** that composites text (subtitles, timestamps, labels) onto
raw video frames. Follows the same element pattern as other processing elements:
single sink pad (raw video in), single src pad (raw video with text out).

### 11a — Text Overlay Element

- [x] `text_overlay` element with 1 sink pad (video/x-raw) + 1 src pad (video/x-raw)
- [x] Configurable text string (via element property or secondary text sink pad)
- [x] Backend: `libfreetype` for font rasterization (glyph bitmap generation)
- [x] Text layout: multi-line support with word wrapping
- [x] Configurable font family, size, colour, outline/shadow
- [x] Configurable position: absolute (x, y) or relative (centre, top-left, bottom-right)
- [x] Alpha blending of text bitmap onto YUV420P / NV12 frames
- [x] PTS passthrough (text overlay preserves video timestamps)
- [x] EOS passthrough
- [x] Caps negotiation: accept/caps on sink pad, same caps on src pad (passthrough)

**Dependencies:** `libfreetype-dev` (added to Dockerfile)

### 11b — Text Source Element (stretch goal)

- [ ] `text_source` element: generates video frames with rendered text (no video input)
- [ ] Useful for test patterns, title cards, and simple slideshows
- [ ] Configurable resolution, framerate, text content, background colour

### 11c — SRT Subtitle Parser (stretch goal)

- [ ] Parse SRT subtitle format into timed text events
- [ ] Feed parsed text segments to `text_overlay` at correct PTS
- [ ] Support ASS/SSA format parsing (advanced styling)

**Test deliverables:**
- [x] Unit test: render text onto a known frame, verify pixels at expected positions
- [x] Unit test: multi-line text wrapping
- [x] Unit test: EOS passthrough
- [x] Unit test: caps negotiation
- [x] Unit test: property get/set for font size, colour, position
- [x] Integration test: `v4l2src → text_overlay → filesink` produces video with visible text
