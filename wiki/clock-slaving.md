# Clock Slaving for A/V Sync

## Current State (Phase 8b)

`zst_clock_t` is implemented with a basic interface:

| API | What it does |
|-----|-------------|
| `zst_clock_get_time()` | Returns current time in nanoseconds |
| `zst_clock_wait(duration)` | Sleeps for a relative duration in ns |
| `zst_pipeline_set_clock()` | Stores a clock in the pipeline |
| `zst_pipeline_get_clock()` | Retrieves the pipeline's clock |
| System clock | Wraps `CLOCK_MONOTONIC` — see `src/zst_clock.c` |

**Critical gap:** The scheduler (`src/zst_scheduler.c`) doesn't use the clock at all.
The worker loop just calls `process()` on elements in a tight loop with a fixed
1 ms `nanosleep` when idle — no PTS awareness, no timing, no sync.

---

## The Problem: Drift

Consider a live recording pipeline:

```
v4l2src (30fps) → queue → x264enc → queue → mp4mux → queue → filesink
alsasrc (44100Hz)→ queue → aacenc ────────────────────────┘
```

Two independent clocks govern the sources:

- **Camera clock**: ~30 fps, driven by USB vsync intervals
- **Audio clock**: 44100 Hz, driven by the audio interface's crystal oscillator
- **System clock** (`CLOCK_MONOTONIC`): driven by the CPU's TSC

These clocks all run at **slightly different rates**. A 0.01% drift between audio
and system clocks means audio is ~4.4 samples/second off. After 5 minutes,
that's ~1300 samples of drift — visibly out of sync.

`clock_gettime(CLOCK_MONOTONIC)` alone can't solve this — it drifts relative
to the hardware clocks that actually drive the media.

---

## What Clock Slaving Does

Clock slaving makes one clock "follow" another clock, compensating for drift
in real-time.

```
┌─────────────────────────────────────────────────────────────┐
│                         Pipeline                              │
│                                                               │
│  ┌──────────────────┐     ┌────────────────────────────┐    │
│  │   Master Clock   │◄────│   Clock Provider           │    │
│  │   (e.g. audio    │     │   (e.g. ALSA source        │    │
│  │   sample clock)  │     │    reports its counter)    │    │
│  └────────┬─────────┘     └────────────────────────────┘    │
│           │                                                   │
│  ┌────────▼─────────┐                                        │
│  │   Slave Clock    │  ← adjusts rate to match master         │
│  │                  │     using periodic drift estimation      │
│  │  get_time() =    │                                        │
│  │  ref_time · α    │  (α = drift ratio, continuously         │
│  │  + β             │   updated)                             │
│  └────────┬─────────┘                                        │
│           │                                                   │
│  ┌────────▼─────────┐                                        │
│  │   Scheduler      │  ← all elements use slave clock        │
│  │   uses clock to  │     for PTS-based timing               │
│  │   wait/skip      │                                        │
│  └──────────────────┘                                        │
└───────────────────────────────────────────────────────────────┘
```

### Step-by-step

**1. Master clock selection**

The pipeline picks the best clock provider. Audio is typically preferred
because audio hardware clocks are the most accurate — crystal oscillators
with very low ppm (parts-per-million) drift. The master clock is the
"truth" for the entire pipeline.

Elements that can provide a clock implement an optional `get_clock()` op
in their vtable. The pipeline surveys all elements at startup and selects
the best clock (e.g. ALSA > V4L2 > system).

**2. The slave clock measures drift**

Periodically (e.g. every second), it samples both the master clock and
the reference clock (`CLOCK_MONOTONIC`):

```
t0:  master=1000 ns          ref=1000 ns
t1:  master=1,000,100,000 ns ref=1,000,000,000 ns
```

The master advanced by 1,000,100,000 ns while the reference advanced only
1,000,000,000 ns. The slave computes:

- **Instantaneous ratio**: `1,000,100,000 / 1,000,000,000 = 1.0001`
- **Running average** (to filter noise):
  `α = α_prev · 0.9 + latest_ratio · 0.1`
- **Now**: `slave_get_time() = CLOCK_MONOTONIC · α + β`
  where β is a phase offset to align the initial time.

**3. Elements wait on the clock**

Instead of the scheduler sleeping for a fixed 1 ms, it uses the clock for
PTS-based timing:

```
current = zst_clock_get_time(pipeline->clock)

if (buf->pts > current + EARLY_THRESHOLD)
    // too early — block until PTS
    zst_clock_wait(pipeline->clock, buf->pts - current)
    deliver(buf)

else if (buf->pts < current - LATE_THRESHOLD)
    // too late — drop (QoS)
    zst_buffer_unref(buf)

else
    // on time — deliver immediately
    deliver(buf)
```

---

## What Needs to Be Built

| Component | Description |
|-----------|-------------|
| **Clock provider interface** | Element vtable callback (`get_clock()`) — elements with a hardware clock (ALSA source, V4L2 source) return a clock instance. The pipeline auto-selects the best one. |
| **Slave clock implementation** | A `zst_clock_t` that slaves to a master clock. Its `get_time()` applies a continuously-updated drift ratio + offset. Its `wait()` can sleep on either the slave or master clock. |
| **Drift estimator** | A timer/thread that periodically samples master vs reference, filters noise with exponential moving average, and computes ratio + offset. |
| **Scheduler clock integration** | The worker loop in `src/zst_scheduler.c` uses the pipeline clock instead of a fixed 1 ms `nanosleep`. It waits until the next buffer's PTS before delivering. |
| **Element PTS tracking** | All elements set `buf->pts` relative to the pipeline clock (source elements timestamp at capture time). |
| **QoS (Quality of Service)** | Measure how far behind each element is running. If frames are consistently late, tell upstream to drop frames (`ZST_BUFFER_FLAG_DROP`) or reduce data rate. |

### Pipeline Flow with Clock Slaving

```
zst_pipeline_set_state(PLAYING)
  │
  ├── Pipeline auto-selects master clock
  │   (e.g. ALSA source provides the most stable clock)
  │
  ├── Creates slave clock, slaves it to master
  │
  ├── Scheduler run loop:
  │   while running:
  │     for each source element:
  │       buf = process()
  │       buf->pts = zst_clock_get_time(pipeline->clock)
  │       push downstream
  │
  │     for each sink element:
  │       buf = pop from queue
  │       wait_until(pipeline->clock, buf->pts)
  │       render(buf)
  │
  └── Clock slave continuously adjusts α
```

---

## Why It Matters

Without clock slaving, a 5‑minute recording from `v4l2src + alsasrc → mp4mux`
produces a file where audio and video drift apart — audio finishes before
video, or vice versa. The muxer timestamps are inconsistent, and players
show visible desync.

With clock slaving:

- Audio and video timestamps are expressed in the **same clock domain**
- Drift is corrected in real-time, not after the fact
- The output file has coherent PTS values
- The scheduler makes intelligent decisions about dropping vs. delivering
  frames (QoS)

---

## Related

- [Clock Sync Bug](clock-sync-debug.md) — Debugging journey for the broken
  clock sync comparison that caused ffplay to drop RTP packets.

## References

- GStreamer clock design: [gstreamer.freedesktop.org/documentation/additional/design/clock.html](https://gstreamer.freedesktop.org/documentation/additional/design/clock.html)
- Linux `CLOCK_MONOTONIC` vs `CLOCK_REALTIME`: `man clock_gettime`
- Audio clock slaving in PipeWire: [pipewire.org](https://pipewire.org)
