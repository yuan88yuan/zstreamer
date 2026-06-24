# Adaptive Stream Demuxing Plan

This document describes the planned API and implementation work needed for demuxers whose output stream set is not fixed at element creation time. Examples include MPEG-TS services with changing PMT/PAT, live broadcast inputs with signal loss/restoration, RTSP/SDP sessions with changing media descriptions, and generic container demuxers that discover tracks after receiving enough bytes.

## Goal

Support demuxers that can reflect the live status of an input media signal:

```text
input signal / byte stream / container
        ↓
 dynamic demuxer
        ├── video_0 appears
        ├── audio_0 appears later
        ├── audio_1 disappears
        ├── video_0 caps change: 1920x1080 → 1280x720
        └── signal lost / signal restored
```

The framework should expose these changes through first-class stream metadata, dynamic pads, bus notifications, in-band pad events, and safe graph reconfiguration APIs.

## Resolved Gaps

Initially, the project had basic pieces of this model:

- `zst_element_add_pad()` / `zst_element_remove_pad()` exist.
- `sdpdemux` creates source pads from parsed SDP tracks.
- `glcompsink` and `audiomixer` use dynamic/request pads.
- `tsdemux` and `mp4demux` discover tracks using FFmpeg.
- The bus supports EOS/error/warning/state/segment events.

However, stronger core support was required, and the following features have now been fully implemented:

- **Pad presence**: Pad templates now model `always` / `sometimes` / `request` presence (Phase B/F).
- **Bus notifications**: Dynamic pad add/remove has a public bus notification contract (Phase C).
- **Deferred destruction**: Pad refcounting (`zst_pad_ref` / `zst_pad_unref`) ensures safety during dynamic pad removal under concurrent scheduler execution (Phase B).
- **In-band sticky events**: Downstream elements receive sticky `STREAM_START`, `CAPS`, and `SEGMENT` events replayed after late linking (Phase C).
- **Enhanced caps metadata**: `zst_caps_t` supports generic key/value fields like `codec_data`, `profile`, `stream-format`, `alignment`, and `language` (Phase E).
- **Stream query API**: A standard stream table query API (`zst_element_get_stream_count`, `zst_element_get_stream_info`, `zst_element_get_stream_pad`) is implemented (Phase A).
- **Flexible process model**: Support for `chain()` callback routing outputs to arbitrary pads bypassing the single-output `process()` assumption (Phase A/F).

---

## 1. Add a first-class stream model

Create a new public header:

```text
include/zst_stream.h
```

Proposed API:

```c
typedef uint64_t zst_stream_id_t;

typedef enum {
    ZST_MEDIA_UNKNOWN,
    ZST_MEDIA_VIDEO,
    ZST_MEDIA_AUDIO,
    ZST_MEDIA_TEXT,
    ZST_MEDIA_DATA
} zst_media_kind_t;

typedef enum {
    ZST_STREAM_STATUS_PRESENT,
    ZST_STREAM_STATUS_LOST,
    ZST_STREAM_STATUS_REMOVED,
    ZST_STREAM_STATUS_CHANGED
} zst_stream_status_t;

typedef struct {
    size_t struct_size;

    zst_stream_id_t id;
    uint32_t program_id;
    uint32_t index;

    zst_media_kind_t kind;
    zst_stream_status_t status;

    const char* name;
    const char* language;

    zst_caps_t* caps;

    zst_time_t first_pts;
    zst_time_t last_seen_pts;

    uint32_t flags;
} zst_stream_info_t;
```

Add element stream query APIs:

```c
uint32_t zst_element_get_stream_count(zst_element_t* el);

zst_result_t zst_element_get_stream_info(
    zst_element_t* el,
    uint32_t index,
    zst_stream_info_t* info_out);

zst_pad_t* zst_element_get_stream_pad(
    zst_element_t* el,
    zst_stream_id_t stream_id);
```

Demuxers maintain an internal stream table and expose snapshots through these APIs.

---

## 2. Extend pad templates with pad presence

Current pad templates only describe name, direction, and caps. Extend them to model dynamic source pads and request pads:

```c
typedef enum {
    ZST_PAD_ALWAYS,
    ZST_PAD_SOMETIMES,
    ZST_PAD_REQUEST
} zst_pad_presence_t;

typedef struct {
    const char* name_template;       /* "sink", "video_%u", "audio_%u" */
    zst_pad_direction_t direction;
    zst_pad_presence_t presence;
    const char* caps;
} zst_pad_template_t;
```

Example descriptor for a floating demuxer:

```c
static const zst_pad_template_t g_demux_pads[] = {
    { "sink",     ZST_PAD_SINK, ZST_PAD_ALWAYS,    "video/mpegts;video/quicktime;ANY" },
    { "video_%u", ZST_PAD_SRC,  ZST_PAD_SOMETIMES, "video/x-h264;video/x-h265;video/x-raw;ANY" },
    { "audio_%u", ZST_PAD_SRC,  ZST_PAD_SOMETIMES, "audio/aac;audio/x-raw;ANY" },
    { "text_%u",  ZST_PAD_SRC,  ZST_PAD_SOMETIMES, "text/x-raw;application/x-subtitle;ANY" }
};
```

Compatibility rule: old descriptors without `presence` should be treated as `ZST_PAD_ALWAYS`.

---

## 3. Make dynamic pad lifecycle safe while PLAYING

Add explicit dynamic-pad helpers:

```c
zst_result_t zst_element_add_dynamic_pad(
    zst_element_t* el,
    zst_pad_t* pad,
    const zst_stream_info_t* stream_info);

zst_result_t zst_element_remove_dynamic_pad(
    zst_element_t* el,
    zst_pad_t* pad);
```

Required behavior:

- Adding a pad in `READY`, `PAUSED`, or `PLAYING` is allowed.
- Removing a pad automatically unlinks its peer.
- Pipeline topology and buffer-pool sizing are marked dirty.
- If the element belongs to a running pipeline, downstream graph ranks are updated.
- Pad destruction is safe even if another scheduler worker is pushing/pulling.

Add pad references or deferred destruction:

```c
zst_pad_t* zst_pad_ref(zst_pad_t* pad);
void zst_pad_unref(zst_pad_t* pad);
```

Add snapshot iteration APIs to avoid iterating arrays that may mutate:

```c
zst_result_t zst_element_snapshot_src_pads(
    zst_element_t* el,
    zst_pad_t*** pads_out,
    uint32_t* count_out);

zst_result_t zst_element_snapshot_sink_pads(
    zst_element_t* el,
    zst_pad_t*** pads_out,
    uint32_t* count_out);

void zst_element_pad_snapshot_free(
    zst_pad_t** pads,
    uint32_t count);
```

Scheduler and default pad code should use snapshots in places where concurrent dynamic graph changes are possible.

---

## 4. Add bus events for dynamic media changes

Extend `zst_event_type_t`:

```c
typedef enum {
    ZST_EVENT_EOS,
    ZST_EVENT_ERROR,
    ZST_EVENT_STATE_CHANGED,
    ZST_EVENT_WARNING,
    ZST_EVENT_SEGMENT,

    ZST_EVENT_PAD_ADDED,
    ZST_EVENT_PAD_REMOVED,
    ZST_EVENT_STREAM_ADDED,
    ZST_EVENT_STREAM_REMOVED,
    ZST_EVENT_STREAM_CHANGED,
    ZST_EVENT_STREAM_STATUS,
    ZST_EVENT_CAPS_CHANGED,
    ZST_EVENT_NO_MORE_PADS,

    ZST_EVENT_SIGNAL_PRESENT,
    ZST_EVENT_SIGNAL_LOST
} zst_event_type_t;
```

Add payloads to `struct zst_event`:

```c
struct {
    zst_pad_t* pad;
    zst_stream_info_t stream;
} pad_added;

struct {
    zst_pad_t* pad;
    zst_stream_id_t stream_id;
} pad_removed;

struct {
    zst_pad_t* pad;
    zst_caps_t* old_caps;
    zst_caps_t* new_caps;
} caps_changed;

struct {
    zst_stream_info_t stream;
} stream_status;
```

Add constructors:

```c
zst_event_t* zst_event_new_pad_added(
    zst_element_t* src,
    zst_pad_t* pad,
    const zst_stream_info_t* stream);

zst_event_t* zst_event_new_pad_removed(
    zst_element_t* src,
    zst_pad_t* pad,
    zst_stream_id_t stream_id);

zst_event_t* zst_event_new_caps_changed(
    zst_element_t* src,
    zst_pad_t* pad,
    const zst_caps_t* old_caps,
    const zst_caps_t* new_caps);

zst_event_t* zst_event_new_signal_lost(zst_element_t* src);
zst_event_t* zst_event_new_signal_present(zst_element_t* src);
```

Applications can use `ZST_EVENT_PAD_ADDED` to dynamically create and link downstream branches.

---

## 5. Add in-band pad events and sticky events

Bus events notify applications, but downstream elements also need stream state changes in the data path.

Create:

```text
include/zst_pad_event.h
src/zst_pad_event.c
```

Proposed API:

```c
typedef enum {
    ZST_PAD_EVENT_STREAM_START,
    ZST_PAD_EVENT_CAPS,
    ZST_PAD_EVENT_SEGMENT,
    ZST_PAD_EVENT_EOS,
    ZST_PAD_EVENT_FLUSH_START,
    ZST_PAD_EVENT_FLUSH_STOP,
    ZST_PAD_EVENT_GAP,
    ZST_PAD_EVENT_DISCONT
} zst_pad_event_type_t;

typedef struct zst_pad_event zst_pad_event_t;

zst_pad_event_t* zst_pad_event_new_stream_start(zst_stream_id_t stream_id);
zst_pad_event_t* zst_pad_event_new_caps(const zst_caps_t* caps);
zst_pad_event_t* zst_pad_event_new_eos(void);

zst_result_t zst_pad_push_event(zst_pad_t* src, zst_pad_event_t* event);
```

Sticky events:

- `STREAM_START`
- `CAPS`
- `SEGMENT`

When a source pad is linked after it appears, the latest sticky events should be replayed to the new peer before the first buffer.

---

## 6. Improve caps for dynamic formats

Keep the current video/audio helpers, but add generic key/value fields for codec and transport metadata:

```c
typedef enum {
    ZST_CAPS_FIELD_INT,
    ZST_CAPS_FIELD_UINT,
    ZST_CAPS_FIELD_DOUBLE,
    ZST_CAPS_FIELD_STRING,
    ZST_CAPS_FIELD_FRACTION,
    ZST_CAPS_FIELD_BUFFER
} zst_caps_field_type_t;

zst_caps_t* zst_caps_new_simple(const char* media_type);

zst_result_t zst_caps_set_string(zst_caps_t* caps, const char* key, const char* value);
zst_result_t zst_caps_set_int(zst_caps_t* caps, const char* key, int value);
zst_result_t zst_caps_set_buffer(zst_caps_t* caps, const char* key, const void* data, size_t size);
```

Example H.264 caps:

```c
zst_caps_t* caps = zst_caps_new_simple("video/x-h264");
zst_caps_set_string(caps, "stream-format", "avc");
zst_caps_set_string(caps, "alignment", "au");
zst_caps_set_int(caps, "width", 1920);
zst_caps_set_int(caps, "height", 1080);
zst_caps_set_buffer(caps, "codec_data", extradata, extradata_size);
```

Fields needed by dynamic demuxers:

- `codec_data` / extradata
- `stream-format` (`avc`, `byte-stream`, `hvc1`, `hev1`, ADTS, LATM)
- `alignment` (`au`, `nal`, `frame`)
- `profile`, `level`, `tier`
- `program-id`, `pid`, `stream-id`
- `language`
- `channel-layout`
- subtitle format metadata

---

## 7. Add safe graph reconfiguration APIs

Applications need to link branches while the scheduler is running.

Add transaction helpers:

```c
zst_result_t zst_pipeline_reconfigure_begin(zst_pipeline_t* pipe);
zst_result_t zst_pipeline_reconfigure_end(zst_pipeline_t* pipe);

zst_result_t zst_pipeline_link_pads_dynamic(
    zst_pipeline_t* pipe,
    zst_pad_t* src,
    zst_pad_t* sink);
```

Behavior:

1. Acquire the pipeline write lock.
2. Optionally block affected pads.
3. Link/unlink pads.
4. Update ranks from the changed element.
5. Mark buffer-pool sizing dirty.
6. Wake the scheduler.
7. Replay sticky events and unblock pads.

Add element helpers:

```c
zst_result_t zst_pipeline_add_element_dynamic(zst_pipeline_t* pipe, zst_element_t* el);
zst_result_t zst_pipeline_remove_element_dynamic(zst_pipeline_t* pipe, zst_element_t* el);
```

If the pipeline is already `PLAYING`, newly-added elements should be moved to the pipeline's current state automatically.

---

## 8. Add unlinked-pad policy

A floating demuxer may produce a pad before the application links it. Add policy control:

```c
typedef enum {
    ZST_PAD_UNLINKED_ERROR,
    ZST_PAD_UNLINKED_DROP,
    ZST_PAD_UNLINKED_BLOCK,
    ZST_PAD_UNLINKED_QUEUE
} zst_pad_unlinked_policy_t;

zst_result_t zst_pad_set_unlinked_policy(
    zst_pad_t* pad,
    zst_pad_unlinked_policy_t policy,
    uint32_t max_queued_buffers);
```

Recommended defaults:

- Dynamic demuxer source pads: `ZST_PAD_UNLINKED_DROP` for live inputs.
- File/on-demand demuxers: `ZST_PAD_UNLINKED_BLOCK` or `ZST_PAD_UNLINKED_QUEUE` when lossless late linking is desired.

---

## 9. Add pad-aware element processing

The current `zst_element_ops_t::process()` path returns one output buffer and default code pushes it to `src_pads[0]`. Demuxers need to route packets to arbitrary pads.

Add optional pad-aware callbacks:

```c
typedef struct {
    const char* name;

    zst_result_t (*open)(zst_element_t* el);
    zst_result_t (*close)(zst_element_t* el);
    zst_result_t (*start)(zst_element_t* el);
    zst_result_t (*stop)(zst_element_t* el);

    zst_result_t (*process)(
        zst_element_t* el,
        zst_buffer_t* in,
        zst_buffer_t** out);

    zst_result_t (*chain)(
        zst_element_t* el,
        zst_pad_t* sink_pad,
        zst_buffer_t* in);

    zst_result_t (*request_pad)(
        zst_element_t* el,
        const char* template_name,
        zst_pad_t** pad_out);

    zst_result_t (*release_pad)(
        zst_element_t* el,
        zst_pad_t* pad);

    /* existing callbacks remain */
} zst_element_ops_t;
```

Rules:

- If `chain()` exists, default sink pad push calls `chain()`.
- `chain()` may push to any source pad.
- Existing `process()` remains supported for simple filters and sources.
- Dynamic demuxers should use `chain()`.

---

## 10. Dynamic demuxer behavior contract

A demuxer should maintain a stream table similar to:

```c
typedef struct {
    zst_stream_id_t id;
    int backend_stream_index;
    uint32_t program_id;

    zst_media_kind_t kind;
    zst_stream_status_t status;

    zst_caps_t* caps;
    zst_pad_t* pad;

    uint64_t generation;
    zst_time_t last_seen;
} demux_stream_t;
```

### New stream discovered

1. Build `zst_stream_info_t`.
2. Create pad name: `video_0`, `audio_0`, `text_0`, etc.
3. Create source pad.
4. Set pad template caps and active caps.
5. Add dynamic pad.
6. Post `ZST_EVENT_STREAM_ADDED` and `ZST_EVENT_PAD_ADDED`.
7. Push sticky `STREAM_START`, `CAPS`, and `SEGMENT` events.
8. Push buffers on the new pad.

### Caps changed

1. Compare old and new caps.
2. If compatible, push a new `CAPS` event and continue.
3. If incompatible, push `FLUSH_START`, `CAPS`, `FLUSH_STOP`, and `DISCONT` as needed.
4. Post `ZST_EVENT_CAPS_CHANGED` / `ZST_EVENT_STREAM_CHANGED`.

### Signal lost

1. Post `ZST_EVENT_SIGNAL_LOST`.
2. Mark streams as `ZST_STREAM_STATUS_LOST`.
3. Optionally push EOS on each active pad.
4. Keep pads alive for a configurable timeout so transient signal loss does not destroy downstream branches immediately.

### Signal restored

1. Post `ZST_EVENT_SIGNAL_PRESENT`.
2. Revalidate the stream table.
3. Existing stream with same ID: send `DISCONT` and continue.
4. New stream: create a new dynamic pad.
5. Missing stream: remove its pad after timeout.

---

## 11. Example application flow

```c
static void on_bus(zst_bus_t* bus, zst_event_t* ev, void* user_data)
{
    zst_pipeline_t* pipe = user_data;

    if (ev->type == ZST_EVENT_PAD_ADDED) {
        zst_pad_t* src = ev->as.pad_added.pad;
        zst_stream_info_t* stream = &ev->as.pad_added.stream;

        if (stream->kind == ZST_MEDIA_VIDEO) {
            zst_element_t* dec = zst_element_factory_make("h264dec");
            zst_element_t* sink = zst_element_factory_make("glsink");

            zst_pipeline_reconfigure_begin(pipe);

            zst_pipeline_add_element_dynamic(pipe, dec);
            zst_pipeline_add_element_dynamic(pipe, sink);

            zst_pad_link(src, zst_element_get_pad(dec, "sink"));
            zst_pad_link(zst_element_get_pad(dec, "src"),
                         zst_element_get_pad(sink, "sink"));

            zst_pipeline_reconfigure_end(pipe);
        }
    }

    if (ev->type == ZST_EVENT_PAD_REMOVED) {
        /* Unlink/remove downstream branch. */
    }

    if (ev->type == ZST_EVENT_CAPS_CHANGED) {
        /* Reconfigure decoder/scaler/sink branch if necessary. */
    }
}
```

---

## 12. Implementation phases

### Phase A — Stream metadata

Files:

```text
include/zst_stream.h
include/zst_types.h
include/zst_element.h
src/zst_element.c
```

Tasks:

- [x] Add `zst_stream_id_t`, `zst_media_kind_t`, `zst_stream_status_t`, and `zst_stream_info_t`.
- [x] Add element stream query APIs.
- [x] Define ownership rules for copied `zst_stream_info_t` and embedded caps.

### Phase B — Dynamic pad safety

Files:

```text
include/zst_pad.h
include/zst_element.h
src/zst_pad.c
src/zst_element.c
src/zst_scheduler.c
```

Tasks:

- [x] Add pad refcounting or deferred destruction.
- [x] Add dynamic pad add/remove helpers.
- [x] Add source/sink pad snapshot APIs.
- [x] Update scheduler and default push/pull paths to tolerate dynamic pad mutation.

### Phase C — Bus and in-band pad events

Files:

```text
include/zst_bus.h
src/zst_bus.c
include/zst_pad_event.h
src/zst_pad_event.c
src/zst_pad.c
```

Tasks:

- [x] Add pad/stream/caps/signal bus events.
- [x] Add event constructors and destruction/copy logic.
- [x] Add pad event propagation.
- [x] Add sticky event storage/replay on source pads.

### Phase D — Graph reconfiguration

Files:

```text
include/zst_pipeline.h
src/zst_pipeline.c
src/zst_scheduler.c
```

Tasks:

- [x] Add reconfiguration transaction APIs.
- [x] Add dynamic element add/remove helpers.
- [x] Wake scheduler after topology changes.
- [x] Recalculate ranks and buffer-pool sizing after dynamic link/unlink.

### Phase E — Caps v2

Files:

```text
include/zst_caps.h
src/zst_caps.c
```

Tasks:

- [x] Add generic key/value caps fields.
- [x] Add copy/intersect/fixate support for generic fields.
- [x] Preserve compatibility with existing video/audio helpers.
- [x] Add codec metadata fields used by demuxers and decoders.

### Phase F — Refactor demuxers

Initial target:

```text
src/mpegts_demuxer.c
include/zstreamer/elements/zst_mpegts_demuxer.h
```

Follow-up targets:

```text
src/mp4_demuxer.c
src/sdp_demuxer.c
```

Pending follow-up (future work):

```text
src/rtmp_source.c
src/rtsp_source.c
```

Tasks:

- [x] Replace fixed `video` / `audio` pads with `video_%u`, `audio_%u`, `text_%u`, `data_%u` dynamic pads where appropriate.
- [x] Maintain stream tables keyed by stable stream IDs.
- [x] Emit bus and pad events on stream add/remove/change.
- [x] Handle signal loss/restoration and transient disappearance.
- [x] Support unlinked-pad policy.

### Phase G — Tests

Add tests for:

- [x] Pad added while `PLAYING`. (Tested in `tests/test_core.c` via TS/MP4 demuxer elements)
- [x] Pad removed while `PLAYING`. (Tested in `tests/test_dynamic.c`)
- [x] Caps changed while `PLAYING`. (Tested in `tests/test_dynamic.c`)
- [x] Signal lost / signal present events. (Tested in `tests/test_dynamic.c`)
- [x] Application links a pad after it appears. (Tested in `tests/test_core.c` via `test_mpegts_elements`)
- [x] Sticky caps/stream/segment events are replayed after late linking. (Tested in `tests/test_dynamic.c`)
- [x] Multi-thread scheduler does not crash during dynamic pad removal. (Tested in `tests/test_dynamic.c`)
- [x] Unlinked pad policies: drop, block, queue. (Tested in `tests/test_dynamic.c`)
- [x] MPEG-TS PMT changes and stream count changes. (Tested in `tests/test_dynamic.c`)
- [x] MP4 fragmented-stream late track discovery. (Tested in `tests/test_dynamic.c`)

---

## Recommended direction

Do not model dynamic demuxers as fixed `video` / `audio` pads.

Use this framework-level contract instead:

```text
stream discovered → dynamic pad created → bus event posted → app/autoplugger links → data flows
stream changed    → caps event + bus event
stream lost       → status event, optional EOS, pad retained temporarily
stream removed    → pad removed safely
```

This gives zstreamer a GStreamer-like dynamic pad model while preserving the lightweight C11 architecture.
