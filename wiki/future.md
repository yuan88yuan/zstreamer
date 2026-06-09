# Future

These features are planned. See `architecture.md` (Future Direction section) for how they fit into the system architecture, and `implementation-plan.md` for the phased build order.

---

## queue element
不是只有 queue object。
而是：
```queue element```
像 GStreamer 那樣。

這會完全改變 threading model。

See: Phase 3 in implementation-plan.md

---

## event bus
```
MM_EVENT_EOS
MM_EVENT_ERROR
MM_EVENT_STATE_CHANGED
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