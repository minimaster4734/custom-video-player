#!/usr/bin/env bash
# Cross-compiles a trimmed, SHARED FFmpeg for local video+audio playback:
# AV1 via dav1d, H.264/HEVC/VP9 via FFmpeg's own native decoders, AAC
# (including xHE-AAC) via libfdk-aac, common video-container audio codecs,
# and MP4/MKV/WebM/AVI demuxing. Deliberately local-file only for phase 1
# (--enable-protocol=file only) -- see README for the network-streaming
# phase-2 plan.
set -euo pipefail

: "${ANDROID_NDK_HOME:?ANDROID_NDK_HOME must be set}"
ABI=$1
API=$2
INSTALL=$3

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PROJECT_ROOT=$(cd "$ROOT/.." && pwd)
JNI_LIBS="$PROJECT_ROOT/app/src/main/jniLibs/${ABI}"
HEADERS="$PROJECT_ROOT/app/src/main/cpp/prebuilt/include"

HOST_TAG="linux-x86_64"
TOOLCHAIN="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/${HOST_TAG}"

case "$ABI" in
  armeabi-v7a)
    TARGET_TRIPLE="armv7a-linux-androideabi"
    FFMPEG_ARCH="arm"
    CPU="armv7-a"
    EXTRA_CONFIGURE_FLAGS="--enable-neon"
    ;;
  *)
    echo "Unsupported ABI: $ABI" >&2
    exit 1
    ;;
esac

export CC="${TOOLCHAIN}/bin/${TARGET_TRIPLE}${API}-clang"
export CXX="${TOOLCHAIN}/bin/${TARGET_TRIPLE}${API}-clang++"
export AR="${TOOLCHAIN}/bin/llvm-ar"
export NM="${TOOLCHAIN}/bin/llvm-nm"
export RANLIB="${TOOLCHAIN}/bin/llvm-ranlib"
export STRIP="${TOOLCHAIN}/bin/llvm-strip"

# dav1d is located via pkg-config with no manual-path fallback in FFmpeg's
# configure script, so this has to be exact.
export PKG_CONFIG_LIBDIR="${INSTALL}/lib/pkgconfig"
export PKG_CONFIG_PATH="${INSTALL}/lib/pkgconfig"

cd "$ROOT/src/ffmpeg"
git clean -xdf >/dev/null 2>&1 || true

./configure \
    --target-os=android \
    --arch="${FFMPEG_ARCH}" \
    --cpu="${CPU}" \
    --enable-cross-compile \
    --cc="$CC" \
    --cxx="$CXX" \
    --ar="$AR" \
    --nm="$NM" \
    --ranlib="$RANLIB" \
    --strip="$STRIP" \
    --sysroot="${TOOLCHAIN}/sysroot" \
    --prefix="$INSTALL" \
    --pkg-config=pkg-config \
    --pkg-config-flags="--static" \
    --extra-cflags="-I${INSTALL}/include -fPIC" \
    --extra-ldflags="-L${INSTALL}/lib" \
    --enable-shared \
    --disable-static \
    --disable-programs \
    --disable-doc \
    --disable-avdevice \
    --disable-avfilter \
    --disable-everything \
    --enable-avformat \
    --enable-avcodec \
    --enable-avutil \
    --enable-swresample \
    --enable-swscale \
    --enable-demuxer=mov,matroska,webm,avi \
    --enable-protocol=file \
    --enable-decoder=libdav1d,h264,hevc,vp9,vvc \
    --enable-decoder=libfdk_aac,aac,pcm_s16le,opus,vorbis,mp3float,ac3,eac3 \
    --enable-parser=h264,hevc,vp9,av1,vvc,aac,opus,vorbis,mpegaudio \
    --enable-libdav1d \
    --enable-libfdk-aac \
    --enable-gpl \
    --enable-version3 \
    --enable-nonfree \
    ${EXTRA_CONFIGURE_FLAGS}

make -j"$(nproc)"
make install

mkdir -p "$HEADERS" "$JNI_LIBS"
cp "${INSTALL}"/lib/libavformat.so* "$JNI_LIBS/"
cp "${INSTALL}"/lib/libavcodec.so* "$JNI_LIBS/"
cp "${INSTALL}"/lib/libavutil.so* "$JNI_LIBS/"
cp "${INSTALL}"/lib/libswresample.so* "$JNI_LIBS/"
cp "${INSTALL}"/lib/libswscale.so* "$JNI_LIBS/"
cp -r "${INSTALL}"/include/* "$HEADERS/"

echo "FFmpeg (shared, dav1d + libfdk-aac) built for ${ABI} -> ${JNI_LIBS}/"
