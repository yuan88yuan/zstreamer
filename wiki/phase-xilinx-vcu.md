# Xilinx VCU Integration Plan

This document outlines the planned integration of the Xilinx VCU (Video Codec Unit) hardware encoder and decoder into the zstreamer framework.

The integration will be based on the [Xilinx vcu-ctrl-sw](https://github.com/Xilinx/vcu-ctrl-sw) repository, which provides the control software for the Allegro DVT IP.

## Elements to Implement

### 1. Xilinx VCU Encoder
- **Description:** Hardware-accelerated video encoder using the Xilinx VCU.
- **Backend:** `lib_encode` from `vcu-ctrl-sw`.
- **Supported Codecs:** HEVC (H.265), VP9.
- **Features:**
  - Hardware accelerated encoding.
  - Multi-channel support.
  - Rate control configuration (CBR, VBR, etc.).

### 2. Xilinx VCU Decoder
- **Description:** Hardware-accelerated video decoder using the Xilinx VCU.
- **Backend:** `lib_decode` from `vcu-ctrl-sw`.
- **Supported Codecs:** HEVC (H.265), VP9.
- **Features:**
  - Hardware accelerated decoding.
  - Multi-channel support.
  - Error concealment and resilience.

## Implementation Details
- The integration will link against `lib_encode` and `lib_decode` provided by the `vcu-ctrl-sw` library.
- It will wrap the `AL_Encoder_Create` / `AL_Decoder_Create` C API for pipeline integration.
- Hardware requirements: Xilinx device with VCU (e.g., Zynq UltraScale+ EV).
