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
  - [ ] Default pool sizing: `min_buffers` = number of queue elements in pipeline + 2
        (so there's always a spare buffer circulating)

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
- [ ] Clock slaving for A/V sync — see [`wiki/clock-slaving.md`](clock-slaving.md)
  for detailed design and task breakdown

## 8c — Other Advanced Features
- [ ] Element bin (composite sub-pipeline)
- [ ] Pad blocking / probes (buffer interception)
- [ ] Segment seeking (timestamp-based clipping)
