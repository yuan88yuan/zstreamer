# Xilinx VCU Integration Plan

This document outlines the planned integration of the Xilinx VCU (Video Codec Unit) hardware encoder and decoder into the zstreamer framework.

The integration will be based on the [Xilinx vcu-ctrl-sw](https://github.com/Xilinx/vcu-ctrl-sw) repository, which provides the control software for the Allegro DVT IP.

## Elements to Implement

### 1. Xilinx VCU Encoder
- **Description:** Hardware-accelerated video encoder using the Xilinx VCU.
- **Backend:** `lib_encode` from `vcu-ctrl-sw`.
- **Supported Codecs:** H.264 (AVC), HEVC (H.265), VP9.
- **Features:**
  - Hardware accelerated encoding.
  - Multi-channel support.
  - Rate control configuration (CBR, VBR, etc.).

### 2. Xilinx VCU Decoder
- **Description:** Hardware-accelerated video decoder using the Xilinx VCU.
- **Backend:** `lib_decode` from `vcu-ctrl-sw`.
- **Supported Codecs:** H.264 (AVC), HEVC (H.265), VP9.
- **Features:**
  - Hardware accelerated decoding.
  - Multi-channel support.
  - Error concealment and resilience.

## Implementation Details
- The integration will link against `lib_encode` and `lib_decode` provided by the `vcu-ctrl-sw` library.
- It will wrap the `AL_Encoder_Create` / `AL_Decoder_Create` C API for pipeline integration.
- Hardware requirements: Xilinx device with VCU (e.g., Zynq UltraScale+ EV).

### Buffer Management and Manipulation
Proper buffer management is critical for performance to avoid unnecessary memory copies. The Xilinx `vcu-ctrl-sw` uses its own buffer representation (`AL_TBuffer` and `AL_TAllocator`). The integration must map these to `zstreamer`'s `zst_buffer_t` and `zst_allocator_t`.

1. **Allocator Integration (`zst_allocator_t` to `AL_TAllocator`)**
   - The VCU requires contiguous memory, often accessible via DMA. The integration will leverage `zstreamer`'s DMABUF allocator fallback (`zst_allocator_dmabuf_create`).
   - A custom `AL_TAllocator` wrapper will be implemented to act as an adapter, translating `vcu-ctrl-sw` allocation requests (`Alloc`, `Free`) into `zstreamer`'s allocator calls (`zst_allocator_alloc`, `zst_allocator_free`).
   - This ensures that buffers allocated by the VCU control software are compatible with the rest of the `zstreamer` pipeline, enabling zero-copy sharing via DMABUFs where supported.

2. **Buffer Mapping (`zst_buffer_t` to `AL_TBuffer`)**
   - **Encoder Input:** Incoming `zst_buffer_t` frames (containing raw video like NV12) will be wrapped in an `AL_TBuffer`. The integration will use `AL_Buffer_Create_And_Allocate` (or a custom wrapper using the DMA allocator) to manage the memory. The `AL_TBuffer` will point to the `zst_buffer_t`'s data. If the incoming buffer is already a DMABUF, it will be imported directly to avoid copies.
   - **Encoder Output / Decoder Input:** Encoded bitstream buffers will also be mapped. For encoding, `vcu-ctrl-sw` pushes bitstream packets via callbacks. These will be wrapped into `zst_buffer_t` packets, preserving PTS and setting appropriate `buffer->type`.
   - **Decoder Output:** Decoded raw frames will be returned by the VCU as `AL_TBuffer`s. The integration will wrap these in `zst_buffer_t` with the `ZST_BUFFER_VIDEO_FRAME` type. `vcu-ctrl-sw` provides pixel plane metadata (`AL_TPixMapMetaData`, `AL_Plane_GetBufferPixelPlanes`), which will be mapped to `zst_video_frame_t` payloads to correctly configure stride and plane offsets for downstream elements.

3. **Zero-Copy and DMABUF**
   - The goal is an end-to-end zero-copy path. If an upstream element (e.g., a V4L2 source or another hardware element) provides a DMABUF, the VCU encoder will directly map that `fd` using `AL_Allocator_Import` (if supported by the specific `vcu-ctrl-sw` adapter) instead of copying.
   - For decoding, the VCU will decode directly into DMABUFs allocated by `zstreamer`'s pool, allowing downstream sinks (like V4L2 sink or DRM sinks) to display them without CPU copies.
