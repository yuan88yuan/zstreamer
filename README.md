# zstreamer

`zstreamer` is a lightweight, modular multimedia streaming and pipeline framework written in C11. It implements a GStreamer-like architecture with elements connected by pads, data carried in reference-counted buffers, and a scheduler that drives pipeline execution.

## Features

- Core pipeline framework with elements, pads, buffers, queueing, caps negotiation, and async event bus
- Thread-safe bounded queue and first-class queue element
- Video/audio conversion and streaming building blocks
- Dynamic plugin loading via `dlopen` plugins
- Built-in support for real multimedia components: V4L2, ALSA, x264, FFmpeg, libswscale, libswresample, FreeType
- RTSP source/sink and multi-session RTSP server support
- Unit tests and example apps included

## Repository Layout

- `include/` — Public API headers
- `src/` — Core framework and element implementations
- `tests/` — Unit tests and examples
- `wiki/` — Architecture docs and implementation plan
- `CMakeLists.txt` — Build configuration
- `Dockerfile` — Ubuntu 24.04 dev/test container

## Build

### Native

```bash
mkdir -p build && cd build
cmake .. -DBUILD_TESTS=ON
make -j$(nproc)
ctest --output-on-failure
```

### Docker

```bash
docker build -t zstreamer .
docker run --rm zstreamer
```

Verbose test output:

```bash
docker run --rm --entrypoint bash zstreamer \
    -c "/workspace/build/ctest -V"
```

Interactive container shell:

```bash
docker run --rm -it zstreamer bash
# then inside container:
cd /workspace/build && ctest -V
```

Live code mount (edit on host, rebuild in container):

```bash
docker run --rm -it \
    -v $(pwd):/workspace \
    zstreamer bash
```

## CMake Options

- `BUILD_TESTS` (default `ON`) — Build test programs
- `BUILD_SHARED` (default `OFF`) — Build core library as shared instead of static
- `ENABLE_PLUGINS` (default `ON`) — Enable dlopen-based plugin loading

## Supported Elements

Built-in elements include:

- `v4l2_source`
- `alsa_source`
- `h264_encoder`
- `h265_encoder`
- `aac_encoder`
- `h264_decoder`
- `h265_decoder`
- `aac_decoder`
- `mp4_muxer`
- `file_sink`
- `file_source`
- `fake_sink`
- `video_scaler`
- `audio_resampler`
- `video_test_src`
- `audio_test_src`
- `text_overlay`
- `text_source`
- `srt_parser`
- `net_source`
- `net_sink`
- `rtsp_source`
- `rtsp_sink`
- `rtsp_server`

## Architecture Overview

The framework is organized around:

- `zst_pipeline` — pipeline container and state propagation
- `zst_element` — processing node with source/sink pads
- `zst_pad` — peer-to-peer connections between elements
- `zst_buffer` — reference-counted data carrier with typed memory
- `zst_queue` — thread-safe bounded queue
- `zst_scheduler` — pipeline driver (single-thread or multi-thread)
- `zst_caps` — media caps negotiation
- `zst_bus` — async event notifications
- `zst_plugin` — dynamic plugin loading support

## Docs

See `wiki/` for design documentation and implementation plans:

- `wiki/architecture.md`
- `wiki/implementation-plan.md`
- `wiki/pipeline-flow.md`
- `wiki/future.md`
- `wiki/rtsp-server-example.md`

## Development Notes

- Language standard: C11
- Public API prefix: `zst_`
- Error handling: `zst_result_t` with `ZST_OK` (0) on success
- Core modules are not thread-safe; thread safety is provided through queue primitives and scheduler design

## License

License information is not included in the repository. Add a LICENSE file if needed.
