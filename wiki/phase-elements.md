# Element Implementations — Phase 4  (✅ 4a-4h, 📝 4i-4n)

Eight elements are fully implemented with real hardware/codec integration and synthetic fallbacks for headless environments.
Six more are planned: file/network I/O for stream ingestion, test sources for headless benchmarking, and a fake sink for pipeline debugging.
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

Receives media data over TCP, Unix sockets, or higher-level streaming protocols (SRT, RTMP) and feeds buffers into the pipeline.

- [ ] `net_source` element with 1+ src pads
- [ ] TCP client mode: connect to remote host:port, read stream into buffers
- [ ] TCP server mode: accept incoming connections, read from first connected client
- [ ] Unix socket support for local IPC
- [ ] Configurable `host`, `port`, `protocol` (tcp-client, tcp-server, unix)
- [ ] Reconnection with exponential back-off on connection loss
- [ ] Buffer size / read timeout configuration
- [ ] EOS on clean disconnect; error recovery on unexpected disconnect
- [ ] Caps negotiation based on stream content type

### 4k — Network Sink  (📝 Planned)

Sends pipeline output data over TCP, Unix sockets, or streaming protocols. Enables live streaming to ingest endpoints (RTMP servers, SRT targets) or local IPC consumption.

- [ ] `net_sink` element with 1 sink pad
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

> **Video / Audio distinction?** 一個 fake sink 就夠了 — 不管是 video 還是 audio buffer，行為都一樣：把 buffer unref 丟棄（回 pool 或 free）。GStreamer 也是單一 `fakesink`。如果之後需要區分統計（如 video fps vs audio latency），可以在 element 內部依 `buffer->type` 分別計數，不需要拆成兩個 element。

- [ ] `fake_sink` element with 1 sink pad
- [ ] Accept any caps — passthrough negotiation, no format restrictions
- [ ] On `sink_push`: immediately `zst_buffer_unref()` the buffer (returns it to pool or frees it)
- [ ] EOS passthrough: count and acknowledge
- [ ] Optional stats: total buffers received, bytes processed, buffer rate (per second by media type)
- [ ] Optional `drop-probability` setting: randomly drop packets to simulate packet loss
- [ ] Zero-copy path: buffer is released without touching payload memory
