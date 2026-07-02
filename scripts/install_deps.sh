#!/usr/bin/env bash
#===============================================================================
#  zstreamer — Dependency Installation Script
#  Installs all required build, development, and runtime library dependencies.
#  Works on Debian/Ubuntu-based systems.
#===============================================================================
set -e

# Detect if running as root
SUDO=""
if [ "$EUID" -ne 0 ]; then
    if command -v sudo >/dev/null 2>&1; then
        SUDO="sudo"
    else
        echo "Warning: Not running as root and 'sudo' not found. Trying to run apt-get directly..."
    fi
fi

echo "--> Updating package lists..."
$SUDO apt-get update

echo "--> Installing compilation tools..."
$SUDO apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    pkg-config \
    zip \
    curl \
    ca-certificates

echo "--> Installing core and element multimedia library dependencies..."
$SUDO apt-get install -y --no-install-recommends \
    libv4l-dev \
    libx264-dev \
    libx265-dev \
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
    libx11-dev \
    libxext-dev \
    libgl1-mesa-dev \
    libglu1-mesa-dev \
    mesa-common-dev

echo "--> Installation completed successfully!"
