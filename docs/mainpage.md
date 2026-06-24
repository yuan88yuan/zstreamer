# zstreamer Documentation

## Introduction

zstreamer is a lightweight, modular multimedia streaming and pipeline framework written in **C11**.
It implements a **GStreamer-inspired** architecture: elements connected via pads,
data flowing as reference-counted buffers through thread-safe queues, driven by a
configurable scheduler.

## Key Features

- **GStreamer-like pipeline model** — elements, pads, buffers, queues, caps negotiation
- **Thread-safe design** — bounded queues, atomic ref-counting, multi-threaded scheduler
- **Dynamic plugins** — `dlopen`-based element loading at runtime
- **Async event bus** — error, EOS, state-change, and warning notifications
- **Clock & A/V sync** — system clock, QoS dropping, clock slaving
- **Adaptive Stream Demuxing** — dynamic pads, stream info queries, and safe runtime graph reconfiguration
- **Lightweight logging** — compile-time level filtering with custom handler support

For more details on how to use zstreamer, please refer to the [Getting Started](@ref tutorials_getting_started) guide and the [Architecture](@ref architecture_architecture) documentation.
