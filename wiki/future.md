# Future

These features are still planned. See `architecture.md` (Future Direction section) for how they fit into the system architecture, and `implementation-plan.md` for the phased build order.

**Implemented already:**
- ✅ Queue Element — see `src/zst_queue_element.c`
- ✅ All element implementations (V4L2, ALSA, x264, AAC, MP4 mux, file sink)

---

## event bus
```
ZST_EVENT_EOS
ZST_EVENT_ERROR
ZST_EVENT_STATE_CHANGED
```

See: Phase 6 in implementation-plan.md

---

## caps negotiation
現在 pipeline 還沒 format negotiation。

實際上非常重要：

```
NV12 -> YUV420P
48000 -> 44100
```

See: Phase 5 in implementation-plan.md

---

## allocator API
做 zero-copy 必備。

See: Phase 8a in implementation-plan.md

---

## clock
A/V sync 核心。

See: Phase 8b in implementation-plan.md

---

## text rendering

Subtitle / caption / label 文字疊加在 video frame 上。

實際上是 video pipeline 很常見的需求：

```
v4l2src → queue → text_overlay → queue → h264enc → ...
```

See: Phase 11 in implementation-plan.md
