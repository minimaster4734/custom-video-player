#!/usr/bin/env bash
# Cross-compiles libfdk-aac as a SHARED library (this project bundles
# prebuilt .so files into the APK rather than static-linking everything
# into one native library -- see README's "Shared vs static" note).
# Provides AAC decode including the full xHE-AAC/USAC path -- same
# approach already proven out in the xheaac-player sibling project,
# because FFmpeg's own native AAC decoder is known-incomplete for the
# USAC speech-coding (ACELP/TCX) path at low bitrates.
set -euo pipefail

: "${ANDROID_NDK_HOME:?ANDROID_NDK_HOME must be set}"
ABI=$1
API=$2
INSTALL=$3

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PROJECT_ROOT=$(cd "$ROOT/.." && pwd)
JNI_LIBS="$PROJECT_ROOT/app/src/main/jniLibs/${ABI}"
HEADERS="$PROJECT_ROOT/app/src/main/cpp/prebuilt/include"

cd "$ROOT/src/fdk_aac"
rm -rf "build-${ABI}"
mkdir "build-${ABI}"
cd "build-${ABI}"

# fdk-aac includes an Android-internal header (log/log.h) when __ANDROID__ is set;
# the NDK does not ship it, so provide a stub that makes the call a no-op.
STUB="$PWD/stub-include"
mkdir -p "$STUB/log"
echo "#define android_errorWriteLog(tag, subTag) ((void)0)" > "$STUB/log/log.h"

cmake .. \
    -DCMAKE_TOOLCHAIN_FILE="${ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="${ABI}" \
    -DANDROID_PLATFORM="android-${API}" \
    -DANDROID_STL=c++_shared \
    -DBUILD_SHARED_LIBS=ON \
    -DCMAKE_CXX_FLAGS="-I$STUB" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${INSTALL}"

cmake --build . --target install -- -j"$(nproc)"

mkdir -p "$HEADERS" "$JNI_LIBS"
cp "${INSTALL}/lib/"libfdk-aac.so* "$JNI_LIBS/"
cp -r "${INSTALL}/include/"* "$HEADERS/" 2>/dev/null || true

echo "fdk-aac (shared) built for ${ABI} -> ${JNI_LIBS}/"
