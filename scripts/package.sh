#!/usr/bin/env bash
set -e

# Usage: ./package.sh <version>
VERSION="${1:-0.1.0}"
# Strip leading 'v' if present for debian package compatibility
DEB_VERSION="${VERSION#v}"

echo "=== Packaging zstreamer v${VERSION} (Debian version: ${DEB_VERSION}) ==="

# Directories
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_STATIC="${PROJECT_ROOT}/build-static"
BUILD_SHARED="${PROJECT_ROOT}/build-shared"
STAGE_DIR="${PROJECT_ROOT}/zstreamer-release-stage"
DEB_STAGE_DIR="${PROJECT_ROOT}/zstreamer-deb-stage"
OUTPUT_DIR="${PROJECT_ROOT}/dist"

# Clean previous build/dist artifacts
rm -rf "$BUILD_STATIC" "$BUILD_SHARED" "$STAGE_DIR" "$DEB_STAGE_DIR" "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

# 1. Build Static Libraries
echo "--> Configuring and building static libraries..."
cmake -B "$BUILD_STATIC" -S "$PROJECT_ROOT" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED=OFF \
    -DBUILD_TESTS=OFF \
    -DENABLE_PLUGINS=ON
cmake --build "$BUILD_STATIC" -j$(nproc)

# 2. Build Shared Libraries
echo "--> Configuring and building shared libraries..."
cmake -B "$BUILD_SHARED" -S "$PROJECT_ROOT" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED=ON \
    -DBUILD_TESTS=OFF \
    -DENABLE_PLUGINS=ON
cmake --build "$BUILD_SHARED" -j$(nproc)

# 3. Create Tarball/Zip Staging
echo "--> Creating staging directory for archives..."
mkdir -p "$STAGE_DIR/zstreamer"
# Install shared build to the stage directory
DESTDIR="" cmake --install "$BUILD_SHARED" --prefix "$STAGE_DIR/zstreamer"

# Copy static libraries to the same lib directory in stage
cp "$BUILD_STATIC/libzstreamer.a" "$STAGE_DIR/zstreamer/lib/"
cp "$BUILD_STATIC/libzstreamer-elements.a" "$STAGE_DIR/zstreamer/lib/"

# 4. Generate Tarball and Zip
echo "--> Generating archives..."
tar -czf "${OUTPUT_DIR}/zstreamer-${VERSION}-linux-x86_64.tar.gz" -C "$STAGE_DIR" zstreamer
if command -v zip >/dev/null 2>&1; then
    (cd "$STAGE_DIR" && zip -r "${OUTPUT_DIR}/zstreamer-${VERSION}-linux-x86_64.zip" zstreamer)
elif command -v python3 >/dev/null 2>&1; then
    echo "zip not found, using python3 to create zip archive..."
    python3 -c "import shutil; shutil.make_archive('${OUTPUT_DIR}/zstreamer-${VERSION}-linux-x86_64', 'zip', '$STAGE_DIR', 'zstreamer')"
else
    echo "Warning: zip command and python3 not found. Skipping zip archive generation."
fi

# 5. Create Debian Package Staging
echo "--> Creating staging directory for Debian package..."
mkdir -p "$DEB_STAGE_DIR/usr"
# Install shared build to usr
DESTDIR="" cmake --install "$BUILD_SHARED" --prefix "$DEB_STAGE_DIR/usr"

# Copy static libraries to usr/lib
cp "$BUILD_STATIC/libzstreamer.a" "$DEB_STAGE_DIR/usr/lib/"
cp "$BUILD_STATIC/libzstreamer-elements.a" "$DEB_STAGE_DIR/usr/lib/"

# Create DEBIAN control file
mkdir -p "$DEB_STAGE_DIR/DEBIAN"
cat << EOF > "$DEB_STAGE_DIR/DEBIAN/control"
Package: zstreamer-dev
Version: ${DEB_VERSION}
Section: devel
Priority: optional
Architecture: amd64
Maintainer: zzlee <zzlee@github.com>
Depends: libavformat-dev, libavcodec-dev, libavutil-dev, libx264-dev, libasound2-dev, libv4l-dev, libswscale-dev, libswresample-dev, libfreetype-dev, libsrt-gnutls-dev, libvulkan-dev
Description: Lightweight modular multimedia streaming framework
 zstreamer is a GStreamer-like C11 library featuring a pipeline architecture,
 elements connected via pads, data flowing as reference-counted buffers through
 thread-safe queues, driven by a configurable scheduler.
 This package contains development headers, static libraries, shared libraries, and plugins.
EOF

# Build Debian package
echo "--> Generating Debian package..."
dpkg-deb --build "$DEB_STAGE_DIR" "${OUTPUT_DIR}/zstreamer-dev_${DEB_VERSION}_amd64.deb"

echo "=== Packaging Completed! Output files in ${OUTPUT_DIR}: ==="
ls -lh "$OUTPUT_DIR"
