#===============================================================================
#  zstreamer — Docker development environment
#
#  Build:  docker build -t zstreamer .
#  Run:    docker run --rm -it zstreamer
#  Test:   docker run --rm zstreamer ctest --test-dir build
#
#  For device access (V4L2 cameras) add:
#    --device /dev/video0
#===============================================================================

FROM ubuntu:24.04 AS base

ENV DEBIAN_FRONTEND=noninteractive

# ── Build dependencies ──────────────────────────────────────────────────
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

# ── Multimedia libraries (for element plugins) ──────────────────────────
# libv4l2       — V4L2 source element
# libx264-dev   — H.264 software encoder
# libavformat / libavcodec — MP4 muxer (optional FFmpeg backend)
RUN apt-get update && apt-get install -y --no-install-recommends \
    libv4l-dev \
    libx264-dev \
    libavformat-dev \
    libavcodec-dev \
    libavutil-dev \
    && rm -rf /var/lib/apt/lists/*

# ── Debugging / profiling tools ─────────────────────────────────────────
RUN apt-get update && apt-get install -y --no-install-recommends \
    gdb \
    valgrind \
    strace \
    && rm -rf /var/lib/apt/lists/*

# ── Copy source & build ─────────────────────────────────────────────────
WORKDIR /workspace
COPY . .

RUN mkdir -p build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON && \
    make -j$(nproc)

# ── Default entry: build & test ─────────────────────────────────────────
FROM base AS ci
WORKDIR /workspace/build
CMD ["ctest", "--output-on-failure"]

# ── Interactive development ─────────────────────────────────────────────
FROM base AS dev
WORKDIR /workspace
CMD ["/bin/bash"]
