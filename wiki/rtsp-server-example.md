# RTSP Server Multi-Session Example

The `rtsp_server` element serves multiple RTSP streams on a single port, each
with its own mount point (URI path). Each mount point creates a named pair of
sink pads (video + audio).

## Architecture

```
Pipeline:
  v4l2src → queue → h264enc ───────────────────────┐
                                                      │
  alsasrc → queue → aacenc ───────────────────────┐  │
                                                    │  │
                                                    ▼  ▼
                                              rtsp_server
                                               (port 8554)
                                                    │
                    ┌───────────────────────────────┼───────────────────┐
                    ▼                               ▼                   ▼
          Client A                          Client B              Client C
          rtsp://host:8554/cam0             rtsp://host:8554/cam0   rtsp://host:8554/cam1
```

## C API Usage

```c
#include "zst_rtsp_server.h"

// Create the RTSP server element
zst_element_t* server = zst_rtsp_server_create();

// Add mount points — each creates <name>_video and <name>_audio sink pads
zst_rtsp_server_add_session(server, "cam0");
zst_rtsp_server_add_session(server, "cam1");

// Configure port (default 8554)
zst_element_set_property(server, "listen_port", "8554");

// Build pipeline
zst_pipeline_t* pipe = zst_pipeline_create();
zst_pipeline_add(pipe, v4l2src);
zst_pipeline_add(pipe, encoder);
zst_pipeline_add(pipe, server);

// Link sources to session pads
zst_pad_link(v4l2src_video_out, zst_element_get_pad(server, "cam0_video"));
zst_pad_link(encoder_audio_out, zst_element_get_pad(server, "cam0_audio"));

// Start it
zst_pipeline_set_state(pipe, ZST_STATE_PLAYING);
```

## Client URLs

| URL | Stream |
|------|--------|
| `rtsp://192.168.1.100:8554/cam0` | Camera 0 video + audio |
| `rtsp://192.168.1.100:8554/cam1` | Camera 1 video + audio |

Test with:

```bash
ffplay -rtsp_transport tcp rtsp://192.168.1.100:8554/cam0
vlc rtsp://192.168.1.100:8554/cam1
```

## Implementation Details

- **Transport**: RTP over RTSP (TCP interleaved) per RFC 2326 §10.12
- **Video**: H.264 RTP packetization per RFC 3984 (single NAL + FU-A)
- **Audio**: AAC RTP packetization per RFC 3640 (MPEG4-Generic)
- **RTCP**: Sender Reports every 5 seconds with NTP/RTP timestamp correlation
- **Threading**: One listen thread + one thread per connected client
- **Port**: Configurable via `listen_port` property
- **Sessions**: Up to 16, each with independent RTP state (SSRC, seq, timestamps)
