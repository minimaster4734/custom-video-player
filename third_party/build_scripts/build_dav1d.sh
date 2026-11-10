#!/usr/bin/env bash
# Cross-compiles dav1d (AV1 software decoder, chosen for speed on armv7
# over libgav1 / FFmpeg's own av1 decoder) via meson+ninja, per dav1d's
# own documented Android cross-compilation approach.
set -euo pipefail

: "${ANDROID_NDK_HOME:?ANDROID_NDK_HOME must be set}"
ABI=$1
API=$2
INSTALL=$3

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PROJECT_ROOT=$(cd "$ROOT/.." && pwd)
JNI_LIBS="$PROJECT_ROOT/app/src/main/jniLibs/${ABI}"
HEADERS="$PROJECT_ROOT/app/src/main/cpp/prebuilt/include"

# cross-armv7.txt references bare tool names (armv7a-linux-androideabi24-clang
# etc.) rather than absolute paths, so the NDK's own toolchain dir has to be
# on PATH for meson to find them.
export PATH="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/linux-x86_64/bin:${PATH}"

cd "$ROOT/src/dav1d"
rm -rf build

# --libdir lib keeps the installed .pc file at a predictable lib/pkgconfig
# path (meson's default can be arch-tripled, e.g. lib/arm-linux-gnueabihf,
# which would break ffmpeg's configure step later -- it locates dav1d via
# pkg-config with no manual-path fallback).
meson setup build \
    --cross-file "$ROOT/build_scripts/cross-armv7.txt" \
    --buildtype release \
    --default-library shared \
    -Denable_tools=false \
    -Denable_tests=false \
    --libdir lib \
    --prefix "$INSTALL"
ninja -C build
ninja -C build install

mkdir -p "$HEADERS" "$JNI_LIBS"
cp "${INSTALL}/lib/"libdav1d.so* "$JNI_LIBS/"
cp -r "${INSTALL}/include/"* "$HEADERS/" 2>/dev/null || true

echo "dav1d (shared) built for ${ABI} -> ${JNI_LIBS}/"
