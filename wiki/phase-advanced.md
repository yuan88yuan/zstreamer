# Advanced Features — Phase 8

## 8a — Allocator API  (from `wiki/future.md`)  (✅ done)
- [x] `zst_allocator_t` interface: `alloc`, `free`, ref-counting
- [x] Default CPU allocator (malloc/free)
- [ ] DMABUF allocator (Linux dma-buf)
- [ ] CUDA / Vulkan device memory allocators
- [ ] Buffer pools to eliminate per-frame allocation

  **`zst_buffer_pool_t` — a recyclable pool of pre-allocated buffers**

  Every source element currently calls `zst_buffer_create()` per frame (see
  `v4l2_source.c`, `alsa_source.c`, `video_scaler.c`, etc.). A buffer pool
  pre-allocates a set of buffers upfront and recycles them, eliminating
  `malloc`/`free` overhead on every frame.

  **Data structure and lifecycle:**
  - [x] `zst_buffer_pool_t` struct with a LIFO/freelist of buffers
  - [x] Backed by a `zst_allocator_t` — pool allocates new buffers via allocator
  - [x] Config: `min_buffers`, `max_buffers`, `buffer_size`, `buffer_type`
  - [x] Thread-safe acquire/release via `pthread_mutex` + `pthread_condvar`
  - [x] Watermark callbacks: low-watermark triggers pre-fill, high-watermark triggers drain
  - [x] `zst_buffer_pool_create(allocator, config)` / `_destroy()` / `_flush()`

  **Acquire / release API:**
  - [x] `zst_buffer_pool_acquire(pool, timeout_ms)` — returns a buffer from the pool;
        blocks if empty until a buffer is returned or timeout expires
  - [x] `zst_buffer_pool_release(pool, buf)` — returns the buffer to the pool;
        resets refcount to 1, clears flags/metadata (but keeps underlying memory for reuse)
  - [x] Optional non-blocking acquire with `ZST_POOL_ACQUIRE_NONBLOCK` flag
  - [x] On release: if pool is at capacity, actually free the buffer instead of recycling

  **Integration with zst_buffer:**
  - [x] `zst_buffer_t` gets an optional `pool` back-pointer (or reuse `memory.priv`)
  - [x] `zst_buffer_create_with_pool(pool)` — acquire from pool instead of malloc
  - [x] `zst_buffer_unref()` checks for pool back-pointer: if pool is set, call
        `pool->release(buf)` instead of `free`; otherwise normal free path
  - [x] Pool buffers skip the `destroy` callback on recycle (only called on final unref when
        pool itself is destroyed)

  **Usage in elements (migration):**
  - [x] `v4l2_source`: allocate pool during `open()`, acquire per-frame in process()
        instead of `zst_buffer_create()`; release happens automatically on `unref`
  - [x] `alsa_source`: same pattern for audio frames
  - [x] `video_scaler`: pool for output buffers
  - [x] `audio_resampler`: pool for output buffers
  - [x] `h264_encoder` / `aac_encoder`: packet pool for encoded output
  - [x] `queue_element`: optionally attach pool to queue — return consumed buffers
        to the upstream pool automatically

  **Auto-configuration from caps:**
  - [x] `zst_buffer_pool_config_from_caps(caps)` — derive `buffer_size` from
        resolution × pixel format (video) or sample_rate × channels × format (audio)
  - [ ] **Default pool sizing** — topology-aware `min_buffers` adjustment

    **Problem:** `zst_buffer_pool_config_from_caps()` only receives caps and has no
    access to the pipeline topology. Elements themselves don't (and shouldn't) hold a
    `zst_pipeline_t*` reference, making it impossible to count queue elements from
    inside an element's `open()` callback.

    **Design decision — two-step configuration:**

    1. **Format sizing** (existing, unchanged): `config_from_caps(caps)` sets
       `buffer_size` from resolution/sample rate, with `min_buffers=2, max_buffers=8`.

    2. **Topology sizing** (new, at pipeline-build time): a pipeline-level helper
       queries the element graph and adjusts pool configs before `start()`:

    ```c
    void zst_pool_config_default_size(zst_buffer_pool_config_t* config,
                                       zst_pipeline_t* pipeline)
    {
        int n_queues = zst_pipeline_count_elements_of_type(pipeline, "queue");
        if (n_queues > 0 && config->min_buffers < n_queues + 2) {
            config->min_buffers = n_queues + 2;
            if (config->max_buffers < config->min_buffers)
                config->max_buffers = config->min_buffers * 2;
        }
    }
    ```

    Called via `zst_pipeline_foreach_element()` before the pipeline transitions to
    PLAYING. This keeps topology decisions at the composition layer — elements
    remain agnostic of the pipeline they live in (important once Phase 8c's Element
    Bin allows nested pipelines).

    - [ ] Implement `zst_pipeline_count_elements_of_type(pipeline, type_name)`
    - [ ] Implement `zst_pool_config_default_size()` helper
    - [ ] Wire into pipeline start sequence or provide a convenience wrapper

  **Test deliverables:**
  - [ ] Unit test: acquire/recycle loop (N buffers, M cycles, no net allocation)
  - [ ] Unit test: acquire blocks when pool exhausted, unblocks on release
  - [ ] Unit test: acquire with timeout returns NULL on expiry
  - [ ] Unit test: pool-backed buffer unref returns buffer to pool
  - [ ] Unit test: pool flush frees all cached buffers
  - [ ] Integration test: `v4l2src → queue → filesink` with pool, verify zero
        calls to `malloc` after warm-up phase

## 8b — Clock  (from `wiki/future.md`)  (✅ done)
- [x] `zst_clock_t` interface: `get_time`, `wait`
- [x] System clock wrapping `CLOCK_MONOTONIC`
- [x] Pipeline-level master clock selection
- [x] Clock slaving for A/V sync — see [`wiki/clock-slaving.md`](clock-slaving.md)
  for detailed design and task breakdown

## 8c — Other Advanced Features

### Element Bin (composite sub-pipeline)

A container element that groups multiple elements into a single logical element with a clean external interface — analogous to GStreamer's `bin`. Enables reusable pipeline components, complex element composition, and hierarchical pipeline structures.

- [ ] `zst_bin_t` as a `zst_element` subclass — state machine delegates to children
- [ ] Ghost pads: `zst_ghost_pad_t` proxies an internal element's pad to the bin's external interface
- [ ] Child management: `zst_bin_add()` / `zst_bin_remove()` with automatic state synchronisation
- [ ] State propagation: `NULL→READY→PAUSED→PLAYING` cascade to all children
- [ ] Error aggregation: child errors bubble up through the bin's bus
- [ ] EOS passthrough: bin converges EOS from all sink-pad branches before signalling src
- [ ] Use case: package `v4l2src → queue → h264enc` as a reusable "capture" bin
- [ ] Use case: create custom muxer bins with internal format conversion
- [ ] Use case: isolate a sub-pipeline for separate threading / scheduling

### Pad Blocking / Probes (buffer interception)

Intercept data flowing through a pad without modifying the element's logic. Analogous to GStreamer's pad probes — enables frame-by-frame inspection, dynamic filtering, and pipeline debugging without element modification.

- [ ] `zst_pad_add_probe(pad, callback, user_data)` — attach a probe callback to a pad
- [ ] Probe types: `PRE_BUFFER` (before element process), `POST_BUFFER` (after process), `PRE_EVENT`, `POST_EVENT`
- [ ] Return values: `PROBE_OK` (passthrough), `PROBE_DROP` (discard buffer), `PROBE_BLOCK` (pause data flow)
- [ ] Pad blocking: `zst_pad_block(pad)` — block data flow at a pad, resume with `zst_pad_unblock()`
- [ ] Block callback: fire on first blocked buffer, return `PROBE_OK` to unblock or `PROBE_REBLOCK` to keep blocking
- [ ] Use case: frame-by-frame stepping through a pipeline (debugger pattern)
- [ ] Use case: dynamic buffer dropping for bandwidth / QoS management
- [ ] Use case: tap into pipeline data for parallel analysis (e.g. recording + preview)
- [ ] Use case: insert custom processing at any pad boundary without writing an element

### Segment Seeking (timestamp-based clipping)

Enable playback of a specific time range within a stream — clip in, clip out, seeking, and looping. Unlike frame-accurate VCR-style seeking, this focuses on segment-based clipping for live recording and on-demand playback.

- [ ] `zst_segment_t` data structure: `start`, `stop`, `rate`, `base`, `position` (floating-point seconds)
- [ ] Segment event: `ZST_EVENT_SEGMENT` propagated downstream from source elements
- [ ] Source element seeking: `zst_element_seek(element, rate, segment)` → element jumps to new position
- [ ] Sink element clipping: apply `start`/`stop` segment bounds — discard buffers outside the window
- [ ] `SEEK` flag in caps for format-specific seek support (seekable files, RTSP PLAY with Range header)
- [ ] Use case: clip a recording to a specific time range (start=30.0, stop=120.0)
- [ ] Use case: loop playback of a segment for stress testing
- [ ] Use case: seek to a specific position in a recorded file source
- [ ] Use case: pause/resume from last position (stop position as resumption point)
