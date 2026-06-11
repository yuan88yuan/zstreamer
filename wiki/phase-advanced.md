# Advanced Features — Phase 8

## 8a — Allocator API  (from `wiki/future.md`)  (✅ done)
- [x] `zst_allocator_t` interface: `alloc`, `free`, ref-counting
- [x] Default CPU allocator (malloc/free)
- [ ] DMABUF allocator (Linux dma-buf)
- [ ] CUDA / Vulkan device memory allocators
- [x] Buffer pools to eliminate per-frame allocation ✅

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
  - [x] **Default pool sizing** — topology-aware `min_buffers` adjustment

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
    - [x] Unit test: basic allocator create/alloc/free/destroy
    - [x] Unit test: nonblock acquire with `ZST_POOL_ACQUIRE_NONBLOCK` flag

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

## 8d — Element Public API and Plugin-First Feature Exposure

Expose the features of all official, dynamic, and future elements through a stable C API. The existing `dlopen` plugin/factory system should be the primary public interface; per-element C headers should be optional convenience wrappers, not the only supported way to use elements.

### Design Principles

- [ ] Treat `zst_element_factory_make()` as the primary user-facing element creation API
- [ ] Use one registry path for built-in official elements and dynamically loaded plugins
- [ ] Make elements self-describing: factory name, category, description, properties, pads, caps, and creation function
- [ ] Keep element private structs private; users interact through `zst_element_t`, pads, caps, properties, and introspection
- [ ] Preserve the current string property API for compatibility while adding typed helpers
- [ ] Provide optional official convenience headers for first-party elements only

### Plugin / Element Metadata

Extend the plugin ABI in a backward-compatible way using `abi_version` and `struct_size`. Keep the current `create_element(const char* name)` path during transition so existing plugins continue to load.

- [ ] Add `zst_property_type_t` for `STRING`, `INT`, `UINT`, `DOUBLE`, `BOOL`, and `ENUM`
- [ ] Add property flags: `READABLE`, `WRITABLE`, `RUNTIME`
- [ ] Add `zst_property_spec_t` with name, type, flags, default value, and description
- [ ] Add `zst_pad_template_t` with pad name, direction, and caps string
- [ ] Add `zst_element_desc_t` describing each element exported by a plugin
- [ ] Extend `zst_plugin_desc_t` to expose an array of `zst_element_desc_t`
- [ ] Support plugins that expose multiple element factories from one `.so`

Candidate public structures:

```c
typedef enum {
    ZST_PROPERTY_STRING,
    ZST_PROPERTY_INT,
    ZST_PROPERTY_UINT,
    ZST_PROPERTY_DOUBLE,
    ZST_PROPERTY_BOOL,
    ZST_PROPERTY_ENUM
} zst_property_type_t;

typedef enum {
    ZST_PROPERTY_READABLE = 1u << 0,
    ZST_PROPERTY_WRITABLE = 1u << 1,
    ZST_PROPERTY_RUNTIME  = 1u << 2
} zst_property_flags_t;

typedef struct {
    const char* name;
    zst_property_type_t type;
    uint32_t flags;
    const char* default_value;
    const char* description;
} zst_property_spec_t;

typedef struct {
    const char* name;
    zst_pad_direction_t direction;
    const char* caps;
} zst_pad_template_t;

typedef struct {
    const char* name;
    const char* long_name;
    const char* category;
    const char* description;
    const char* author;

    const zst_property_spec_t* properties;
    uint32_t nb_properties;

    const zst_pad_template_t* pads;
    uint32_t nb_pads;

    zst_element_t* (*create)(void);
} zst_element_desc_t;
```

### Factory Introspection APIs

Applications, CLIs, UIs, and tests should be able to discover available elements at runtime, including third-party plugin elements unknown at compile time.

- [ ] `zst_element_factory_list()` — list all registered element descriptors
- [ ] `zst_element_factory_get_desc(name)` — get metadata for one factory name
- [ ] Introspection should work for both built-in and plugin-backed elements
- [ ] Return property and pad metadata without requiring element instantiation where possible

Candidate API:

```c
uint32_t zst_element_factory_list(
    const zst_element_desc_t*** elements_out);

const zst_element_desc_t* zst_element_factory_get_desc(
    const char* name);
```

### Typed Property Helpers

Keep the current string API:

```c
zst_element_set_property(el, "chunk-size", "4096");
```

Add typed wrappers for safer user code:

- [ ] `zst_element_set_property_string()` / `zst_element_get_property_string()`
- [ ] `zst_element_set_property_int()` / `zst_element_get_property_int()`
- [ ] `zst_element_set_property_uint()` / `zst_element_get_property_uint()`
- [ ] `zst_element_set_property_double()` / `zst_element_get_property_double()`
- [ ] `zst_element_set_property_bool()` / `zst_element_get_property_bool()`
- [ ] Validate typed helper calls against descriptor metadata when available

Candidate API:

```c
zst_result_t zst_element_set_property_string(zst_element_t* el, const char* name, const char* value);
zst_result_t zst_element_set_property_int(zst_element_t* el, const char* name, int64_t value);
zst_result_t zst_element_set_property_uint(zst_element_t* el, const char* name, uint64_t value);
zst_result_t zst_element_set_property_double(zst_element_t* el, const char* name, double value);
zst_result_t zst_element_set_property_bool(zst_element_t* el, const char* name, bool value);

zst_result_t zst_element_get_property_string(zst_element_t* el, const char* name, char* value_out, size_t max_len);
zst_result_t zst_element_get_property_int(zst_element_t* el, const char* name, int64_t* value_out);
zst_result_t zst_element_get_property_uint(zst_element_t* el, const char* name, uint64_t* value_out);
zst_result_t zst_element_get_property_double(zst_element_t* el, const char* name, double* value_out);
zst_result_t zst_element_get_property_bool(zst_element_t* el, const char* name, bool* value_out);
```

### Unified Built-In and Dynamic Registration

The current dynamic plugin flow should remain valid:

```c
zst_plugin_registry_init();
zst_plugin_registry_scan(path);
zst_plugin_registry_scan_env();
zst_element_factory_make("filesrc");
```

Add built-in registration so official elements and dynamic plugins are available through the same factory/introspection API.

- [ ] Add `zst_register_builtin_elements()` or equivalent initialization hook
- [ ] Register built-in official elements into the same factory registry as dynamic plugins
- [ ] Ensure `zst_element_factory_make()` does not care whether an element is built-in or plugin-backed
- [ ] Install plugin `.so` files to a stable plugin directory and continue supporting `ZSTREAMER_PLUGIN_PATH`

Candidate usage:

```c
zst_plugin_registry_init();
zst_register_builtin_elements();
zst_plugin_registry_scan_env();

zst_element_t* src = zst_element_factory_make("filesrc");
zst_element_t* enc = zst_element_factory_make("h264encoder");
zst_element_t* sink = zst_element_factory_make("filesink");
```

### Metadata for Official Elements

Add `zst_element_desc_t` metadata for every official element:

- [ ] `filesrc`, `filesink`, `fakesink`
- [ ] `v4l2source`, `alsasource`
- [ ] `h264encoder`, `h264decoder`
- [ ] `h265encoder`, `h265decoder`
- [ ] `aacencoder`, `aacdecoder`
- [ ] `mp4muxer`
- [ ] `videoscaler`, `audioresampler`
- [ ] `videotestsrc`, `audiotestsrc`
- [ ] `textoverlay`, `textsource`, `srtparser`
- [ ] `netsrc`, `netsink`
- [ ] `rtspsource`, `rtspsink`, `rtspserver`
- [ ] future RTMP source/sink and other elements

Each descriptor should document:

- [ ] factory name
- [ ] long name, category, and description
- [ ] source and sink pad templates
- [ ] supported/static caps where known
- [ ] readable/writable properties and defaults
- [ ] read-only statistics where applicable

### Optional Official Convenience Headers

Install optional first-party headers under a stable namespace, for example:

```text
include/zstreamer/elements/zst_file_source.h
include/zstreamer/elements/zst_file_sink.h
include/zstreamer/elements/zst_fake_sink.h
include/zstreamer/elements/zst_h264_encoder.h
...
```

These headers may expose constructor convenience functions, property name macros, and optional config structs with `struct_size` for ABI extension.

- [ ] Add convenience headers for official elements where useful
- [ ] Add property name macros to avoid string literals in user code
- [ ] Use `struct_size` in config structs for forward-compatible extension
- [ ] Implement wrappers on top of the same element implementations; do not bypass the generic factory/property model

Example:

```c
#define ZST_FILE_SOURCE_PROP_PATH       "path"
#define ZST_FILE_SOURCE_PROP_CHUNK_SIZE "chunk-size"
#define ZST_FILE_SOURCE_PROP_LOOP       "loop"

zst_element_t* zst_file_source_create(const char* path);
```

### Library and Installation Layout

Recommended installable artifacts:

```text
libzstreamer.so              core framework and registry
libzstreamer-elements.so     official element implementations, if not linked into core
lib/zstreamer/plugins/*.so   dynamic plugins
include/zstreamer/...        public headers
```

- [ ] Decide whether official elements live in `libzstreamer`, `libzstreamer-elements`, plugins, or a supported combination
- [ ] Install public headers and optional convenience headers
- [ ] Install official plugin `.so` files to a stable plugin directory
- [ ] Add CMake/pkg-config metadata so users can link core and official elements cleanly

### Test Deliverables

- [ ] Existing plugin ABI loads during transition
- [ ] Registry lists built-in elements
- [ ] Registry lists dynamically loaded plugin elements
- [ ] Descriptors for official elements contain expected properties and pads
- [ ] `zst_element_factory_make()` creates the same element whether backed by built-in registration or plugin registration
- [ ] Typed property helpers set/get values correctly
- [ ] Typed helper validation rejects wrong property types where metadata is available
- [ ] Public convenience headers compile and link from an external-style test target
- [ ] Third-party test plugin descriptors are discoverable

### User-Facing Examples

Generic plugin-friendly API:

```c
zst_plugin_registry_init();
zst_register_builtin_elements();
zst_plugin_registry_scan_env();

zst_element_t* src = zst_element_factory_make("filesrc");
zst_element_set_property_string(src, "path", "input.h264");
zst_element_set_property_uint(src, "chunk-size", 4096);

zst_element_t* sink = zst_element_factory_make("filesink");
zst_element_set_property_string(sink, "path", "output.h264");
```

Optional official convenience API:

```c
#include "zstreamer/elements/zst_file_source.h"
#include "zstreamer/elements/zst_file_sink.h"

zst_element_t* src = zst_file_source_create("input.h264");
zst_element_t* sink = zst_file_sink_create("output.h264");
```
