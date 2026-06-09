# Future

## queue element
不是只有 queue object。
而是：
```queue element```
像 GStreamer 那樣。

這會完全改變 threading model。

## event bus
```
MM_EVENT_EOS
MM_EVENT_ERROR
MM_EVENT_STATE_CHANGED
```

## caps negotiation
現在 pipeline 還沒 format negotiation。

實際上非常重要：

```
NV12 -> YUV420P
48000 -> 44100
```

## allocator API
做 zero-copy 必備。

## clock
A/V sync 核心。