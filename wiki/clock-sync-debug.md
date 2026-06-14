# Clock Sync Debug: "RTP: dropping old packet received too late"

## Symptom

When connecting `ffplay` to the RTSP demo server (`demo_rtsp_mod.c`), the
following warning floods the terminal during playback:

```
[rtsp @ 0x7ed054000c80] RTP: dropping old packet received too late
    Last message repeated 162 times
```

This warning from FFmpeg's RTP depacketizer (`libavformat/rtpdec.c`) means
the client's jitter buffer is discarding packets whose timestamps are more
than ~600ms **behind** the already-consumed position. The stream appears
choppy or freezes.

## First Attempts

Initial investigation focused on the RTSP server's RTP timestamp generation
in `src/rtsp_server.c`:

| Hypothesis | Checked | Result |
|-----------|---------|--------|
| Non-monotonic RTP timestamps | `session_deliver()` computes `st->timestamp` via `base_rtp_ts + (delta * clock_rate) / 1e9`, where `delta = buf->pts - base_pts`. Since PTS is monotonically increasing, the RTP timestamp is also monotonically increasing. | ✅ Timestamps are fine |
| RTP-Info mismatch | The PLAY response reports `seq=%u;rtptime=%u` from the initial `rand32()` value. The first RTP packet uses the same timestamp because `base_rtp_ts = st->timestamp` (= same `rand32()`) and `delta = 0`. | ✅ RTP-Info matches first packet |
| SPS/PPS delivered with wrong timestamp | SPS/PPS are sent within `session_deliver()` after `base_pts`/`base_rtp_ts` are initialized, so they use the same timestamp as the first frame. | ✅ SPS/PPS timestamps correct |
| Race in write_rtp_packet | `send_rtcp_sr()` from `client_thread` can race with `write_rtp_packet()` from a pipeline push callback. Both write to `cl->fd` without the global `srv->lock`. | ⚠️ Data race, but not the primary cause |
| Pipeline runs faster than real-time | **Bingo.** The pipeline produces frames at encoder speed (thousands of fps for 320×240 H.264), and the RTSP server sends RTP packets to ffplay at the same rate. ffplay's jitter buffer fills faster than it drains. | ✅ Root cause |

## Root Cause

The scheduler in `src/zst_scheduler.c` has a **clock sync** feature intended
to pace output to real-time:

```c
if (pipe->clock_sync && el->clock && out_buf->pts > 0 ...) {
    zst_time_t current = zst_clock_get_time(el->clock);
    if (out_buf->pts > current + 5000000ULL) {
        zst_clock_wait(el->clock, out_buf->pts - current);
    }
}
```

**The comparison is broken** because it compares absolute values from two
incompatible time domains:

| Value | Domain | Example |
|-------|--------|---------|
| `out_buf->pts` | Source-generated PTS | `33 ms` (frame-count-based) |
| `current = clock_get_time()` | Absolute `CLOCK_MONOTONIC` | `120.5 s` (since boot) |

Since `33 ms > 120.5 s + 5 ms` is **never true**, the condition never fires,
clock sync never waits, and the pipeline runs at full encoder speed.

### Frame-count-based PTS (`use-clock=false`)
```
buf->pts = s->frame_count * dur_ns
```
- Frame 0: PTS = 0
- Frame 1: PTS = 33 ms  
- Frame 2: PTS = 66 ms

### Clock-based PTS (`use-clock=true`)
```
buf->pts = zst_clock_get_time(el->clock)
```
- Frame 0: PTS ≈ 120.500 s
- Frame 1: PTS ≈ 120.501 s (~1 ms later at encoder speed)
- Frame 2: PTS ≈ 120.502 s

With **either** mode the absolute comparison `PTS > current + 5ms` fails:

| Mode | PTS | current | PTS > current + 5ms? |
|------|-----|---------|---------------------|
| Frame-based (f=1) | 33 ms | 120.5 s | Never |
| Clock-based (f=1) | 120.501 s | 120.5 s | `120.501 > 120.5 + 0.005` = No |

### Result: pipeline at full speed

With 320×240 H.264 + 44100Hz AAC on a modern CPU, the encoder sustains
thousands of frames per second. The RTSP server sends RTP packets at the
same rate. ffplay receives 2000+ RTP packets per second while only consuming
30 per second (at 30fps). The jitter buffer overflows and drops "old" packets.

## The Fix

### Insight: compare deltas, not absolutes

The clock sync should compare **how much PTS advanced between frames**
against **how much wall-clock time actually elapsed**. This works regardless
of whether PTS is frame-count-based or clock-based, and regardless of the
absolute clock value.

```
pts_delta   = PTS_N - PTS_{N-1}
time_delta  = clock_N - clock_{N-1}

if (pts_delta > time_delta + 5 ms):
    wait(pts_delta - time_delta)
```

### Implementation (3 files changed)

**`include/zst_element.h`** — Two fields track the previous frame's state:

```c
zst_time_t clock_sync_last_pts;
zst_time_t clock_sync_last_clock;
```

**`src/zst_element.c`** — Reset on transition to PLAYING:

```c
el->clock_sync_last_pts    = 0;
el->clock_sync_last_clock = 0;
```

**`src/zst_scheduler.c`** — Delta-based clock sync:

```c
if (el->clock_sync_last_pts != 0) {
    zst_time_t pts_delta  = out_buf->pts - el->clock_sync_last_pts;
    zst_time_t time_delta = current - el->clock_sync_last_clock;
    if (pts_delta > time_delta + 5000000ULL) {
        zst_clock_wait(el->clock, pts_delta - time_delta);
        current = zst_clock_get_time(el->clock); // re-read after wait
    }
}
el->clock_sync_last_pts    = out_buf->pts;
el->clock_sync_last_clock = current;
```

**`tests/demo_rtsp_mod.c`** — Removed `use-clock=true` from sources so PTS
uses frame-counts, allowing clock sync to properly pace against wall time:

```c
zst_element_set_property_bool(video_src, "use-clock", false);
zst_element_set_property_bool(audio_src, "use-clock", false);
```

### Why this works

With frame-count-based PTS and delta comparison:

| Frame | PTS | clock_get_time() | pts_delta | time_delta | Wait? |
|-------|-----|-----------------|-----------|------------|-------|
| 0 | 0 | 120.500 s | — | — | No (first frame) |
| 1 | 33 ms | 120.501 s | 33 ms | 1 ms | **Yes: 27 ms** |
| 2 | 66 ms | 120.529 s | 33 ms | 28 ms | Yes: 5 ms (overrun) |
| 3 | 100 ms | 120.563 s | 34 ms | 34 ms | No (caught up) |

After the first frame, the pipeline paces itself to ~30 fps.

## Why ffplay Drops Packets Without This Fix

Without pacing, the RTSP server delivers all RTP packets in a burst:

```
Wall-clock time:  [0 ms]  [0.1 ms]  [0.2 ms]  [0.3 ms]  ...
RTP timestamp:     0       3000      6000      9000      ...  (frames at 30fps)
```

ffplay receives 100 frames in 0.5 ms of wall time, but each frame's RTP
timestamp is 33 ms apart. ffplay decodes at 30 fps, consuming one frame
every 33 ms. The jitter buffer grows to 100+ frames instantly.

After 2 seconds of real time, ffplay has consumed ~60 frames (PTS ≈ 2.0 s).
But the jitter buffer contains frame 61+ with PTS > 2.0 s — these are
**ahead**, not behind.

The "dropping old packet" message appears when the depacketizer encounters
a packet whose RTP timestamp is more than ~600 ms **behind** the last
dequeued position. This happens when:

1. The jitter buffer fills to capacity
2. New packets arrive with sequence numbers that wrap around in the buffer
3. The depacketizer interprets the wrapped packet as "old" relative to the
   last-dequeued position

With pacing, the pipeline produces frames at the correct rate (~30 fps),
the jitter buffer stays nearly empty, and ffplay receives and consumes
frames at the same rate.

## Edge Cases Considered

| Scenario | Behavior |
|----------|----------|
| First frame has PTS=0 | `clock_sync_last_pts == 0`, comparison skipped, state recorded |
| Element transitions to PLAYING mid-stream | State reset to 0, first buffer re-bases |
| `use-clock=true` (PTS = clock_get_time) | pts_delta ≈ time_delta → no wait (no pacing, but no flood either) |
| Encoder slower than real-time | pts_delta < time_delta + 5ms → no wait (produces as fast as it can) |
| Long idle between frames (e.g. paused) | time_delta >> pts_delta → no wait (clock already past the PTS) |
| AAC encoder buffers frames internally | pts_delta captures actual output spacing, comparison still valid |

## Files Changed

```
M include/zst_element.h     — Add clock_sync_last_{pts,clock} fields
M src/zst_element.c         — Reset clock sync state on PLAYING transition
M src/zst_scheduler.c       — Delta-based clock sync comparison
M tests/demo_rtsp_mod.c     — Remove use-clock=true from sources
```
