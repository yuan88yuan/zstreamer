# Cross-Compilation for ARM64 (Petalinux / Xilinx SC6f0)

This document describes how to cross-compile the `zstreamer` framework for ARM64 embedded platforms (specifically targeting Petalinux / Xilinx SC6f0 environments) using the cross-compilation toolchain.

## Cross-Compilation Toolchain

- **Base Docker Image:** `yuan88yuan/xlnk2_arm64:v1` (containing an `amd64` development host with an `aarch64` cross-compiler SDK).
- **Environment Initialization:** Sourced via `/opt/qcap-dev-init` to set up paths to cross-compiler binaries (e.g., `aarch64-xilinx-linux-gcc`) and the target sysroot.
- **Dockerfile:** `Dockerfile.xlnk2_arm64`

## Handling Optional Dependencies

Because the embedded sysroot lacks common multimedia and rendering libraries (such as FFmpeg, x264, x265, Freetype2, and SRT), the CMake build system and unit tests have been adapted to make these dependencies optional.

### 1. Compile Guards and Defines

The build system automatically detects the availability of these packages via `pkg-config` and sets corresponding C preprocessor defines:

- `HAS_FFMPEG`: Set if FFmpeg libraries (libavformat, libavcodec, libavutil, libswscale, libswresample) are found.
- `HAS_X264`: Set if `x264` is found.
- `HAS_X265`: Set if `x265` is found.
- `HAS_ALSA`: Set if ALSA (`alsa`) is found.
- `HAS_V4L2`: Set if `libv4l2` is found.
- `HAS_FREETYPE`: Set if Freetype2 (`freetype2`) is found.
- `HAS_SRT`: Set if SRT (`srt` or `srt-gnutls`) is found.

### 2. Conditional Plugin Compilation

In `CMakeLists.txt`, plugin targets and source listings are dynamically appended only if their dependencies are met. Similarly, `src/zst_builtins.c` conditionally registers the element descriptors under the corresponding preprocessor guards.

### 3. Test Suite Adaptation

To ensure unit tests can compile and run on target configurations with missing libraries, unit tests in `tests/test_core.c` are guarded and skipped appropriately:

- **Optional elements integration:** Tests for optional plugins (e.g., MPEG-TS, MP4 demuxer, RTMP, RTSP, SRT, text overlay) are wrapped in their respective defines.
- **Skipped logs:** When dependencies are missing, the test suite runner outputs `[SKIP]` for the corresponding test groups.
- **Conditional binary building:** Binary examples (like `example_record` and `demo_colorbar_mp4`) are only built if all their required dependencies are available.

## How to Build

Run the following command to build the cross-compilation docker image:

```bash
docker build -f Dockerfile.xlnk2_arm64 -t zstreamer-xlnk2-arm64 .
```

This builds the `aarch64` target libraries and plugins under the cross-compiler environment inside `/workspace/build/` of the container.

## How to Run Tests on Target Host

Since target binaries cannot be run directly inside the `amd64` container hosting the cross-compiler, they must be copied to the arm64 target device, or executed via `qemu-user-static` configuration on the host.
