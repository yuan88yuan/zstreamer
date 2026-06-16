# Clock and A/V Sync Guide

Accurate timing and synchronization are fundamental to multimedia pipelines. zstreamer uses a unified clock system (`zst_clock_t`) to ensure audio and video streams stay aligned and play back at the correct speed.

## The zst_clock_t

A `zst_clock_t` is an object that provides a monotonically increasing time source, measured in nanoseconds. It abstracts the underlying timing mechanism (e.g., system monotonic clock, audio device clock, or an external network clock).

## Master/Slave Relationships

To maintain synchronization across different elements, a pipeline establishes a clock hierarchy.

-   **Master Clock:** One clock is chosen as the master reference for the entire pipeline. This is often the clock provided by the audio sink (since audio playback is highly sensitive to clock drift) or the system monotonic clock.
-   **Slave Clocks:** Other clocks in the pipeline act as slaves. They "slave" to the master clock by periodically comparing their local time to the master's time and adjusting their rate or offset to match.

### Clock Slaving Mechanics

Slaving is critical for long-running streams to prevent drift. If a video capture device runs slightly faster than the audio playback device, video buffers will accumulate.

zstreamer implements clock slaving to handle this. It avoids multiplying large absolute timestamps (which causes precision loss). Instead, it stores base anchor times (`base_master`, `base_ref`) and applies floating-point arithmetic only to the smaller time deltas.

1.  **Observation:** A slave clock periodically observes the time from the master clock.
2.  **Calculation:** It calculates the drift (difference between expected and actual time) and the rate difference.
3.  **Adjustment:** It updates its internal offset and rate multiplier so that subsequent calls to `zst_clock_get_time()` return a value synchronized with the master clock.

## Scheduling and Sync

The clock is actively used by the pipeline scheduler (`zst_scheduler_t`) to control the flow of data.

1.  **Presentation Timestamps (PTS):** Every buffer carries a PTS, indicating exactly when it should be presented (rendered or played).
2.  **Waiting:** Before a sink element renders a buffer, it checks the buffer's PTS against the pipeline's master clock.
3.  `zst_clock_wait(clock, duration)`: If the PTS is in the future, the sink calls `zst_clock_wait` to block the thread until the correct presentation time arrives. Note that `time` passed to `zst_clock_wait` is a relative duration, not an absolute timestamp.

## Handling Late Frames (ZST_FLOW_DROPPED)

In real-time systems, processing delays can cause a buffer to arrive at the sink *after* its scheduled PTS.

1.  **Lateness Check:** When the sink checks the PTS against the clock, it calculates the "lateness" (Clock Time - PTS).
2.  **QoS Threshold:** If the buffer is late beyond a certain threshold (Quality of Service - QoS threshold), rendering it is useless and might cause further delays.
3.  **Dropping:** The scheduler or the sink element will choose to drop the buffer. When a buffer is dropped, the operation returns `ZST_FLOW_DROPPED`.
4.  **Feedback:** This drop signal propagates upstream, allowing source elements (like decoders or capture devices) to adjust their behavior, perhaps by skipping frames to catch up.
