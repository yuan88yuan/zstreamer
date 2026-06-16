# Caps Negotiation

Caps (Capabilities) negotiation in zstreamer is the process by which two linked pads agree on the format of the data that will flow between them. This process ensures that the source pad produces data that the sink pad can consume, and vice versa.

## Caps Structure

In zstreamer, capabilities are represented by the `zst_caps_t` structure. A `zst_caps_t` contains a list of `zst_caps_struct_t` elements, each describing a specific media format.

A `zst_caps_struct_t` includes:
- `media_type`: A string representing the broad category and format (e.g., `video/x-raw`, `audio/x-raw`, `video/x-h264`).
- A generic dictionary or properties list for format-specific fields:
  - For video: `format` (e.g., RGB, YUV), `width`, `height`, `framerate`.
  - For audio: `format` (e.g., S16LE, F32LE), `rate` (sample rate), `channels`.

Elements can expose multiple structures in their caps if they support multiple formats.

## Format Intersection

When two pads are linked, or when an element wants to start producing data, it needs to find a format that both pads support. This is done through format intersection.

The `zst_caps_intersect()` function takes two `zst_caps_t` objects and returns a new `zst_caps_t` containing only the structures that are mutually compatible.

The intersection rules are:
1.  **Media Type Match:** The `media_type` strings must match exactly.
2.  **Field Match:** For each field present in both structures, the values must match or be compatible (e.g., one specifies a range that includes the other's fixed value). If a field is missing in one structure, it's typically considered an "any" match, meaning it accepts any value the other structure specifies for that field.

If the intersection is empty, the pads cannot agree on a format, and the negotiation fails.

## Dynamic Reconfiguration

Caps negotiation can happen dynamically during pipeline execution. This is useful for scenarios where the source changes its format (e.g., a file demuxer encounters a new stream with a different resolution) or the sink needs a different format.

When a source pad needs to change its format, it typically calls `zst_pad_push_event()` with a Caps event containing the new `zst_caps_t`.

1.  **Event Flow:** The Caps event flows downstream to the peer sink pad.
2.  **Element Handling:** The receiving element intercepts the Caps event in its `event_handler`. It checks if it can support the new caps.
3.  **Reconfiguration:** If the element supports the new caps, it reconfigures its internal state (e.g., re-initializes a decoder or resampler) to handle the new format. It then updates its own sink pad's caps using `zst_pad_set_caps()`.
4.  **Downstream Propagation:** If the element modifies the data format (e.g., a decoder), it negotiates new caps with its downstream peers by sending a new Caps event from its source pad.

This dynamic reconfiguration allows zstreamer pipelines to adapt to changing media properties without needing to stop and rebuild the pipeline.
