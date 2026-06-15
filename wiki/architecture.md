# Architecture

`zstreamer` is a **GStreamer-inspired** multimedia pipeline framework written in C11.  
It decomposes media processing into a directed graph of **elements** connected via **pads**, with data flowing as **buffers** through **queues** driven by a **scheduler**.

## Core Concepts

### Buffer (`zst_buffer`)
The fundamental data carrier — a reference-counted blob with:

| Field      | Purpose                                 |
|------------|-----------------------------------------|
| `type`     | Video/audio frame, packet, or user      |
| `refcount` | Atomic ref-counting for zero-copy sharing |
| `pts/dts`  | Presentation / decode timestamps        |
| `duration` | Duration of the data                    |
| `memory`   | Typed memory descriptor (CPU, DMABUF, CUDA, Vulkan, oneAPI) |
| `payload`  | Opaque typed payload (video/audio frame structs) |
| `destroy`  | Optional custom destructor              |

**Lifecycle:**
```
zst_buffer_create() → refcount = 1
zst_buffer_ref()    → refcount++
zst_buffer_unref()  → refcount--; free when 0
```

### Pad (`zst_pad`)
Connection point on an element. Two directions:

- **SRC pad** — emits data (source / output)
- **SINK pad** — receives data (sink / input)

Pads are linked peer-to-peer:
```
src_pad → peer → sink_pad
sink_pad → peer → src_pad
```

Each pad carries optional **caps** (media type negotiation) and **push/pull** function pointers for both task-based and pull-based data flow.

### Element (`zst_element`)
A processing node in the pipeline. Elements implement the **ops** vtable:

| Op       | Called during state transition |
|----------|--------------------------------|
| `open`   | NULL → READY (allocate resources) |
| `close`  | READY → NULL (release resources) |
| `start`  | PAUSED → PLAYING (start streaming) |
| `stop`   | PLAYING → PAUSED (stop streaming) |
| `process`| Active streaming: transform `in` → `out` |

Elements own an array of source pads and sink pads. Multi-pad elements (e.g. a muxer with separate video/audio inputs) add multiple pads.

### State Machine

```
NULL  ──open──→  READY  ──start──→  PLAYING
  ↑                │                    │
  └──close──┘      └──────stop─────────┘
```

`PAUSED` is reserved but not yet wired — elements can optionally implement it for preroll.

### Pipeline (`zst_pipeline`)
An ordered container of elements. Its primary job is **state propagation** — calling `zst_element_set_state()` on every element in sequence.

```
pipe = zst_pipeline_create()
zst_pipeline_add(pipe, src)
zst_pipeline_add(pipe, encoder)
zst_pipeline_set_state(pipe, ZST_STATE_PLAYING)
```

### Queue (`zst_queue`)
Thread-safe blocking queue between processing stages.

- **SYNC mode**: bounded with back-pressure (push blocks when full)
- **ASYNC mode**: drops buffers when full (best-effort)
- Configurable limits: max buffers, max bytes, max duration
- Timeout-aware `push` / `pop` (including `timeout_ms=0` for try-lock)
- `flush` for cleanup on state transitions

Implemented with `pthread_mutex` + `pthread_condvar`.

### Queue Element (`zst_queue_element`)
A first-class `zst_element` subclass wrapping `zst_queue_t`. Unlike internal queues, the queue element is explicitly placed in the pipeline by the user. Each queue element has:

- One **sink pad** — receives buffers into the queue
- One **src pad** — pushes dequeued buffers downstream
- A **worker thread** that pops from the queue and pushes via `zst_pad_push()`

```
v4l2src → queue → h264enc → queue → mp4mux → queue → filesink
```

Every queue element is a threading boundary: upstream runs in its thread, downstream runs in the queue's thread.

### Scheduler (`zst_scheduler`)
Drives the pipeline's execution model.

| Mode            | Behaviour                            |
|-----------------|---------------------------------------|
| SINGLE_THREAD   | Sequential processing in calling thread |
| MULTI_THREAD    | Worker thread pool per element chain    |

In multi-thread mode each worker pops from its input queue, calls `process()`, and pushes to the next stage — a classic **pipeline parallelism** pattern.

### Element Implementations

All six pipeline elements are fully implemented with real hardware/codec integration:

| Element         | Library            | Status |
|-----------------|--------------------|--------|
| V4L2 Source     | `libv4l2`          | ✅ Real device + synthetic mock fallback |
| ALSA Source     | `libasound`        | ✅ Real device + synthetic mock fallback |
| H.264 Encoder   | `libx264`          | ✅ ultrafast preset, CRF rate control |
| AAC Encoder     | `libavcodec`       | ✅ FFmpeg AAC, S16→FLTP conversion |
| MP4 Muxer       | `libavformat`      | ✅ Fragmented MP4, custom AVIO, EOS tracking |
| File Sink       | stdio `FILE*`      | ✅ fwrite of buffer data |
| Video Scaler    | `libswscale`       | 📝 Planned — scaling + pixel format conversion |
| Audio Resampler | `libswresample`    | 📝 Planned — sample rate + format conversion |

### Plugin (`zst_plugin`)
Dynamic element loading via `dlopen()`:

- Each `.so` exports `zst_get_plugin()`
- Plugin descriptor carries name, author, version
- Element factory function creates named elements

## Pipeline Data Flow

```
┌──────────┐    ┌───────────┐    ┌──────────┐    ┌───────────┐    ┌──────────┐    ┌───────────┐    ┌──────────┐
│ v4l2src  │───→│ queue_el  │───→│ h264enc  │───→│ queue_el  │───→│ mp4mux   │───→│ queue_el  │───→│ filesink │
└──────────┘    └───────────┘    └──────────┘    └───────────┘    └──────────┘    └───────────┘    └──────────┘

┌──────────┐    ┌───────────┐    ┌──────────┐    ┌───────────┐      ┆
│ alsasrc  │───→│ queue_el  │───→│ aacenc   │───→│ queue_el  │──────┘
└──────────┘    └───────────┘    └──────────┘    └───────────┘
```

Explicit queue elements define threading boundaries. The scheduler assigns
one thread per source element; queue elements each have their own worker
thread for pushing downstream, decoupling producers from consumers.

## Design Principles

1. **Minimal dependencies** — core needs only pthreads; optional plugins bring in libv4l2, x264, ffmpeg.
2. **Zero-copy by default** — buffers are ref-counted and shared across pads.
3. **Explicit state machine** — every resource transition is traceable.
4. **Pluggable everything** — elements are loaded at runtime; scheduler strategy is configurable.
5. **C11** — portable, embeddable, FFI-friendly.

---

## Recently Implemented

The following features were previously planned and have been implemented. See `wiki/implementation-plan.md` for details.

### Event Bus  (✅ done — Phase 6)

An async notification channel (`zst_bus_t`) decoupled from the data path. Elements and the pipeline post events (`EOS`, `ERROR`, `STATE_CHANGED`) to the bus; applications listen via `zst_bus_pop()` or a callback.

### Caps Negotiation  (✅ done — Phase 5)

Pads now carry rich caps with dimensions, format, framerate, channels, sample rate.
- `zst_caps_intersect()` to find compatible formats
- Auto-negotiation at link time
- Video scaler (`libswscale`) and audio resampler (`libswresample`) are available
  for format conversion — see Phase 4g/4h in `implementation-plan.md`

### Allocator API  (✅ done — Phase 8a)

`zst_allocator_t` interface for custom memory backends:
- [x] Default CPU allocator (malloc/free) with refcounting
- [x] Integrated with `zst_buffer_create_with_allocator()`
- [ ] DMABUF (Linux dma-buf for zero-copy between HW blocks) — future
- [ ] CUDA / Vulkan / oneAPI (SYCL) device memory — future
- [ ] Buffer pools to eliminate per-frame allocation — future

### Clock  (✅ done — Phase 8b)

`zst_clock_t` master clock wrapping `CLOCK_MONOTONIC`, with:
- [x] `zst_clock_get_time()` / `zst_clock_wait()`
- [x] Pipeline-level clock selection (`zst_pipeline_set_clock`)
- [ ] Clock slaving for A/V sync — future
- [ ] Jitter measurement — future
