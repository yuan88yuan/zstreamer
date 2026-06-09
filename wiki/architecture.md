# Architecture

`zstreamer` is a **GStreamer-inspired** multimedia pipeline framework written in C11.  
It decomposes media processing into a directed graph of **elements** connected via **pads**, with data flowing as **buffers** through **queues** driven by a **scheduler**.

## Core Concepts

### Buffer (`mm_buffer`)
The fundamental data carrier — a reference-counted blob with:

| Field      | Purpose                                 |
|------------|-----------------------------------------|
| `type`     | Video/audio frame, packet, or user      |
| `refcount` | Atomic ref-counting for zero-copy sharing |
| `pts/dts`  | Presentation / decode timestamps        |
| `duration` | Duration of the data                    |
| `memory`   | Typed memory descriptor (CPU, DMABUF, CUDA, Vulkan) |
| `payload`  | Opaque typed payload (video/audio frame structs) |
| `destroy`  | Optional custom destructor              |

**Lifecycle:**
```
mm_buffer_create() → refcount = 1
mm_buffer_ref()    → refcount++
mm_buffer_unref()  → refcount--; free when 0
```

### Pad (`mm_pad`)
Connection point on an element. Two directions:

- **SRC pad** — emits data (source / output)
- **SINK pad** — receives data (sink / input)

Pads are linked peer-to-peer:
```
src_pad → peer → sink_pad
sink_pad → peer → src_pad
```

Each pad carries optional **caps** (media type negotiation) and **push/pull** function pointers for both task-based and pull-based data flow.

### Element (`mm_element`)
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

### Pipeline (`mm_pipeline`)
An ordered container of elements. Its primary job is **state propagation** — calling `mm_element_set_state()` on every element in sequence.

```
pipe = mm_pipeline_create()
mm_pipeline_add(pipe, src)
mm_pipeline_add(pipe, encoder)
mm_pipeline_set_state(pipe, MM_STATE_PLAYING)
```

### Queue (`mm_queue`)
Thread-safe blocking queue between processing stages.

- **SYNC mode**: bounded with back-pressure (push blocks when full)
- Configurable limits: max buffers, max bytes, max duration
- Timeout-aware `push` / `pop`
- `flush` for cleanup on state transitions

Implemented with `pthread_mutex` + `pthread_condvar`.

### Scheduler (`mm_scheduler`)
Drives the pipeline's execution model.

| Mode            | Behaviour                            |
|-----------------|---------------------------------------|
| SINGLE_THREAD   | Sequential processing in calling thread |
| MULTI_THREAD    | Worker thread pool per element chain    |

In multi-thread mode each worker pops from its input queue, calls `process()`, and pushes to the next stage — a classic **pipeline parallelism** pattern.

### Plugin (`mm_plugin`)
Dynamic element loading via `dlopen()`:

- Each `.so` exports `mm_get_plugin()`
- Plugin descriptor carries name, author, version
- Element factory function creates named elements

## Pipeline Data Flow

```
┌──────────┐    ┌──────────┐    ┌──────────┐
│ v4l2src  │───→│ h264enc  │───→│ mp4mux   │──┐
└──────────┘    └──────────┘    └──────────┘  │
                                              │  ┌──────────┐
┌──────────┐    ┌──────────┐    ┌──────────┐  ├─→│ filesink  │
│ alsasrc  │───→│ aacenc   │───→│ mp4mux   │──┘  └──────────┘
└──────────┘    └──────────┘    └──────────┘
     ↑               ↑                ↑
   [queue]         [queue]          [queue]
```

Queues decouple producers from consumers, allowing each stage to run on its own thread.

## Design Principles

1. **Minimal dependencies** — core needs only pthreads; optional plugins bring in libv4l2, x264, ffmpeg.
2. **Zero-copy by default** — buffers are ref-counted and shared across pads.
3. **Explicit state machine** — every resource transition is traceable.
4. **Pluggable everything** — elements are loaded at runtime; scheduler strategy is configurable.
5. **C11** — portable, embeddable, FFI-friendly.
