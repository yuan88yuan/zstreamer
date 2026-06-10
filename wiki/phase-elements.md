# Element Implementations — Phase 4  (✅ 4a-4h, 📝 4i-4r)

Eight elements are fully implemented with real hardware/codec integration and synthetic fallbacks for headless environments.
Ten more are planned: file/network I/O for stream ingestion, RTSP/RTMP for live streaming, test sources for headless benchmarking, and a fake sink for pipeline debugging.
Two more handle format conversion (scaling, resampling) — essential once caps negotiation (Phase 5) requires automatic conversion between mismatched formats.

### 4a — V4L2 Source  (✅ done)
- [x] Open `/dev/video0` with O_RDWR | O_NONBLOCK
- [x] Format negotiation: `VIDIOC_S_FMT` (YUYV, 640×480)
- [x] MMAP buffer setup: `VIDIOC_REQBUFS` / `QUERYBUF` / `QBUF`
- [x] `VIDIOC_STREAMON` / `VIDIOC_STREAMOFF`
- [x] poll-based non-blocking capture with timeout
- [x] YUYV → YUV420P colour space conversion
- [x] **Synthetic fallback** when no camera: moving vertical bar pattern, 30 fps

**Dependencies:** `libv4l-dev` (in Docker)

### 4b — H.264 Encoder  (✅ done)
- [x] x264 integration: `x264_param_default_preset("ultrafast", "zerolatency")`
- [x] CRF rate control (23)
- [x] Accept I420 YUV planes from `zst_video_frame_t` payload
- [x] NAL unit concatenation into `zst_buffer` packets
- [x] PTS passthrough
- [x] EOS passthrough
- [x] Lazy initialization on first frame (handles dynamic resolution)

**Dependencies:** `libx264-dev` (in Docker)

### 4c — MP4 Muxer  (✅ done)
- [x] FFmpeg `libavformat` integration
- [x] Custom AVIO write callback pushes buffers downstream (not to file)
- [x] Video stream (H.264) + audio stream (AAC)
- [x] Fragmented MP4: `frag_keyframe+empty_moov+default_base_moof`
- [x] Per-stream EOS tracking: muxer waits for both video + audio EOS before propagating
- [x] Proper `av_write_trailer()` on stop

**Dependencies:** `libavformat-dev`, `libavcodec-dev`, `libavutil-dev` (in Docker)

### 4d — File Sink  (✅ done)
- [x] FILE* writer: `fopen`, `fwrite`, `fclose`
- [x] Writes buffer memory data to file
- [x] Proper `close` lifecycle hook

### 4e — ALSA Audio Source  (✅ done)
- [x] `snd_pcm_open("default", SND_PCM_STREAM_CAPTURE)`
- [x] Parameter setup: S16_LE, 44100Hz, stereo, 0.5s latency
- [x] `snd_pcm_readi()` for capture
- [x] Underrun / xrun recovery (`-EPIPE` → `snd_pcm_prepare`)
- [x] **Synthetic fallback**: 440Hz square wave, 44100Hz timing with nanosleep

**Dependencies:** `libasound2-dev`

### 4f — AAC Encoder  (✅ done)
- [x] FFmpeg `libavcodec` AAC encoder: `avcodec_find_encoder(AV_CODEC_ID_AAC)`
- [x] S16LE interleaved → FLTP float planar conversion
- [x] `avcodec_send_frame()` / `avcodec_receive_packet()` API
- [x] 128kbps bitrate
- [x] EOS passthrough

**Dependencies:** `libavcodec-dev` (in Docker)

### 4g — Video Scaler  (✅ done)

A conversion element that scales video frames and converts pixel formats. Deployed
when a source's output caps (e.g. 1080p NV12) don't match the next element's input
caps (e.g. 720p I420).

- [x] **Interface**: single sink pad, single src pad — accepts raw video, outputs raw video
- [x] **Backend**: `libswscale` from FFmpeg (`sws_getContext` / `sws_scale`)
- [x] **Auto-configuration**: on first frame, allocate the SWS context based on input resolution/format and configured output resolution/format
- [x] Configurable target: `width`, `height`, `pixel_format` — or passthrough if formats match
- [x] **Synthetic fallback**: naive nearest-neighbour scaling if `libswscale` unavailable
- [x] EOS passthrough

**Dependencies:** `libswscale-dev` (in Docker)

### 4h — Audio Resampler  (✅ done)

Converts audio sample rate and format. Needed when source sample rate (e.g. ALSA
at 48000Hz) differs from what the encoder expects (e.g. AAC at 44100Hz), or when
format mismatches (S16LE ↔ F32LE).

- [x] **Interface**: single sink pad, single src pad — accepts raw audio, outputs raw audio
- [x] **Backend**: `libswresample` from FFmpeg (`swr_alloc_set_opts` / `swr_convert`)
- [x] **Auto-configuration**: on first frame, allocate SWR context from input/output params
- [x] Configurable: `sample_rate`, `sample_format`, `channels` — passthrough if matching
- [x] **Synthetic fallback**: linear interpolation resampling if `libswresample` unavailable
- [x] EOS passthrough

**Dependencies:** `libswresample-dev` (in Docker)

---

### 4i — File Source  (📝 Planned)

Reads raw or containerised media data from a local file and pushes it into the pipeline as a sequence of buffers. Analogous to GStreamer's `filesrc`.

- [ ] `file_source` element with 1 src pad — configurable `path` property
- [ ] Open file with `fopen`/`open` (O_RDONLY) on state transition to READY
- [ ] Read chunks into `zst_buffer` pool, push to src pad
- [ ] Send `EOS` when `feof()` / `read()` returns 0
- [ ] Configurable `chunk_size` and `loop` (restart from beginning on EOF)
- [ ] Support for `offset` / `length` to read a subset of the file
- [ ] Caps negotiation: advertise `text/plain`, `video/x-h264`, `audio/aac`, etc. based on file extension or probe

### 4j — Network Source  (📝 Planned)

Receives raw byte streams over TCP or Unix sockets and feeds them into the pipeline as buffers.

> **Protocol layering note:** This is a **raw transport** element. It reads bytes from a socket without understanding any application-layer protocol (RTSP, RTMP, HTTP, SRT, etc.). It outputs opaque byte buffers — not demuxed video/audio pads. For protocol-aware streaming with automatic demuxing and caps negotiation, use **4o (RTSP Source)** or **4q (RTMP Source)** instead.

- [ ] `net_source` element with 1 src pad — outputs raw byte buffers
- [ ] TCP client mode: connect to remote host:port, read stream into buffers
- [ ] TCP server mode: accept incoming connections, read from first connected client
- [ ] Unix socket support for local IPC
- [ ] Configurable `host`, `port`, `protocol` (tcp-client, tcp-server, unix)
- [ ] Reconnection with exponential back-off on connection loss
- [ ] Buffer size / read timeout configuration
- [ ] EOS on clean disconnect; error recovery on unexpected disconnect
- [ ] Caps negotiation: fixed `text/plain` caps or none (passthrough)

### 4k — Network Sink  (📝 Planned)

Sends raw byte buffers over TCP or Unix sockets. Enables local IPC and custom binary protocol output.

> **Protocol layering note:** This is a **raw transport** element. It writes bytes to a socket without understanding any application-layer protocol (RTSP, RTMP, HTTP, SRT, etc.). It accepts a single raw byte buffer on its sink pad — not demuxed video/audio streams. For protocol-aware streaming output with proper muxing, use **4p (RTSP Sink)** or **4r (RTMP Sink)** instead.

- [ ] `net_sink` element with 1 sink pad — accepts raw byte buffers
- [ ] TCP client mode: connect to remote host:port and write buffers
- [ ] TCP server mode: listen, accept, and stream to connected clients
- [ ] Unix socket support for local IPC
- [ ] Configurable `host`, `port`, `protocol` (tcp-client, tcp-server, unix)
- [ ] Reconnection with exponential back-off on connection loss
- [ ] Write timeout and buffer drain on disconnect
- [ ] EOS passthrough: flush remaining data before closing connection

---

### 4l — Video Test Source  (📝 Planned)

Generates synthetic video test patterns without any real hardware input. Useful for pipeline testing, benchmarking, and demo scenarios where no camera is available.

- [ ] `video_test_src` element with 1 src pad
- [ ] Configurable resolution (`width` x `height`), framerate, pixel format
- [ ] Test pattern options: colour bars (SMPTE/EBU), moving gradients, checkerboard, white noise, black/silent
- [ ] Timestamp generation: `pts` set from pipeline clock at capture rate
- [ ] EOS on `stop` state transition or configurable frame limit
- [ ] Caps negotiation: advertise `video/x-raw` with configurable resolution/formats
- [ ] Optional YUV420P → NV12 / RGB conversion in software
- [ ] Loop mode: restart pattern sequence on frame limit or EOS

### 4m — Audio Test Source  (📝 Planned)

Generates synthetic audio test signals without any real hardware input. Useful for pipeline testing, latency measurement, and audio chain verification.

- [ ] `audio_test_src` element with 1 src pad
- [ ] Configurable sample rate, channels, sample format (S16LE, F32LE)
- [ ] Signal options: sine wave (configurable frequency), square wave, pink/white noise, silence
- [ ] Timestamp generation: `pts` set from pipeline clock based on `nb_samples`
- [ ] EOS on `stop` or configurable sample limit
- [ ] Caps negotiation: advertise `audio/x-raw` with configurable format/channels/rate
- [ ] Loop mode: restart signal sequence on limit or EOS

### 4n — Fake Sink  (📝 Planned)

Consumes and immediately discards incoming buffers without any I/O or processing. Used for headless profiling, throughput testing, and pipeline termination without a real output.

> **Video / Audio distinction?** A single fake sink is sufficient — both video and audio buffers behave identically: `zst_buffer_unref()` discards the buffer (returns it to the pool or frees it). GStreamer also uses a single `fakesink` for all media types. If per-type statistics are needed later (e.g. video fps vs audio latency), the element can count internally by `buffer->type` — no need for separate elements.

- [ ] `fake_sink` element with 1 sink pad
- [ ] Accept any caps — passthrough negotiation, no format restrictions
- [ ] On `sink_push`: immediately `zst_buffer_unref()` the buffer (returns it to pool or frees it)
- [ ] EOS passthrough: count and acknowledge
- [ ] Optional stats: total buffers received, bytes processed, buffer rate (per second by media type)
- [ ] Optional `drop-probability` setting: randomly drop packets to simulate packet loss
- [ ] Zero-copy path: buffer is released without touching payload memory

---

### 4o — RTSP Source  (📝 Planned)

Receives live or on-demand streaming media from an RTSP server (DESCRIBE/SETUP/PLAY), demuxes RTP streams into separate video/audio source pads, and feeds them into the pipeline. The de-facto standard for IP camera ingestion.

- [ ] `rtsp_source` element with 2+ src pads (video, audio, metadata)
- [ ] RTSP control: DESCRIBE (SDP parsing), SETUP (transport negotiation), PLAY/PAUSE/TEARDOWN
- [ ] RTP/RTCP transport: UDP (unicast + multicast), TCP interleaved mode
- [ ] SDP → caps negotiation: map payload types (PT) to `video/x-h264`, `audio/aac`, etc.
- [ ] RTSP authentication: Basic, Digest
- [ ] Reconnection: automatic re-SETUP on transport loss, exponential back-off
- [ ] NTP timestamp correlation: map RTP timestamps → pipeline clock via RTCP SR
- [ ] Configurable `rtsp_url`, `username`, `password`, `transport` (udp/tcp), `buffer_size`
- [ ] RTSP keep-alive: OPTIONS pings to prevent server timeout
- [ ] EOS on RTSP BYE or TEARDOWN

### 4p — RTSP Sink  (📝 Planned)

Acts as an RTSP server element that accepts incoming RTP streams and makes them available for RTSP clients to connect and consume (pull model). Enables live relay scenarios where zstreamer is the streaming source.

- [ ] `rtsp_sink` element with 2+ sink pads (video, audio)
- [ ] Built-in lightweight RTSP server: listen on configurable port, handle DESCRIBE/SETUP/PLAY
- [ ] SDP generation from input caps: generate SDP body from pad caps on all pads ready
- [ ] RTP/RTCP transport: UDP unicast per connected client, TCP interleaved fallback
- [ ] Multiple concurrent client support — each client gets its own RTP stream
- [ ] RTP packetisation: H.264 (RFC 3984), AAC (RFC 3640), generic payload wrapping
- [ ] RTCP sender reports: generate SR packets with NTP/RTP timestamps
- [ ] Configurable `listen_port`, `mount_point`, `max_clients`, `transport`

### 4q — RTMP Source  (📝 Planned)

Connects to an RTMP server (or receives RTMP pushes) and demuxes the FLV stream into video/audio buffers. Essential for consuming from live streaming platforms, OBS pushes, and legacy IP cameras.

- [ ] `rtmp_source` element with 2 src pads (video, audio)
- [ ] RTMP handshake + connect: `connect("rtmp://host/live/streamkey")`, `createStream`, `play`
- [ ] FLV demuxing: parse FLV tag headers, extract video (H.264/HEVC/AV1) and audio (AAC/MP3)
- [ ] AMF0/AMF3 metadata parsing: extract `onMetaData` (width, height, framerate, samplerate)
- [ ] Timestamp mapping: FLV timestamps → pipeline clock PTS
- [ ] Configurable `rtmp_url`, `live` (true/false for live vs VOD), `buffer_time`, `swf_url`
- [ ] Authentication: `rtmp://user:pass@host/app/streamkey`
- [ ] Reconnection: auto-reconnect on stream loss, exponential back-off
- [ ] EOS on RTMP stream end or `deleteStream`

### 4r — RTMP Sink  (📝 Planned)

Publishes pipeline output to an RTMP ingest endpoint — the standard way to push to YouTube Live, Twitch, Facebook Live, and most CDNs.

- [ ] `rtmp_sink` element with 2 sink pads (video, audio)
- [ ] RTMP handshake + publish: `connect(...)`, `publish("streamkey")`
- [ ] FLV muxing: wrap incoming H.264/AAC buffers into FLV tags, maintain correct tag boundaries
- [ ] AMF0 metadata injection: `@setDataFrame("onMetaData")` with `width`, `height`, `framerate`, `videocodecid`, `audiocodecid`, `duration`
- [ ] Timestamp generation: pipeline clock → FLV timestamps (milliseconds, monotonically increasing)
- [ ] Configurable `rtmp_url`, `live` (true = no buffer, low latency)
- [ ] Authentication: `rtmp://user:pass@host/app/streamkey`
- [ ] Reconnection: auto-reconnect on publish failure, exponential back-off
- [ ] EOS passthrough: send `FCUnpublish` on stream end, clean disconnect
