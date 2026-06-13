#===============================================================================
#  zstreamer — Docker development environment
#
#  Build:    docker build -t zstreamer .
#  Run:      docker run --rm zstreamer              # runs ctest (ci target)
#  Run dev:  docker run --rm -it zstreamer bash     # interactive shell
#
#  For device access (V4L2 cameras) add:
#    --device /dev/video0
#
#  Build for a specific stage:
#    docker build --target dev -t zstreamer-dev .
#===============================================================================

FROM ubuntu:24.04 AS base

ENV DEBIAN_FRONTEND=noninteractive

# ── Build dependencies ──────────────────────────────────────────────────
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    pkg-config \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*


# ── Multimedia libraries (for element plugins) ──────────────────────────
RUN apt-get update && apt-get install -y --no-install-recommends \
    libv4l-dev \
    libx264-dev \
    libavformat-dev \
    libavcodec-dev \
    libavutil-dev \
    libswscale-dev \
    libswresample-dev \
    libasound2-dev \
    libfreetype-dev \
    libsrt-gnutls-dev \
    libgnutls28-dev \
    nettle-dev \
    libvulkan-dev \
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

# ── Interactive development (inherits all of base) ──────────────────────
FROM base AS dev
WORKDIR /workspace
CMD ["/bin/bash"]

# ── CI / one-shot test (default target — last in Dockerfile) ────────────
FROM base AS ci
WORKDIR /workspace/build
CMD ["ctest", "--output-on-failure"]
