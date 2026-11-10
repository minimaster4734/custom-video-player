#!/usr/bin/env bash
# Makes the freshly built shared libraries loadable on Android.
#
# Problem: FFmpeg, dav1d and fdk-aac are built as libfoo.so.N (versioned
# SONAME, other libs list "libfoo.so.N" as a dependency). Android's package
# installer only extracts files named lib*.so, and its dynamic linker looks
# dependencies up by those exact names -- so the versioned names crash at
# startup ("library libavcodec.so.61 not found").
#
# Fix, per library: keep one real file named libfoo.so, set its SONAME to
# "libfoo.so", and rewrite every "libbar.so.N" dependency entry to
# "libbar.so". Also strips debug info to keep the APK small.
set -euo pipefail

DIR=${1:?usage: normalize_sonames.sh <dir with the .so files>}
: "${ANDROID_NDK_HOME:?ANDROID_NDK_HOME must be set}"
STRIP="${STRIP_BIN:-${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip}"

cd "$DIR"

# 1. Turn symlinks into real files, then drop every versioned name.
for f in lib*.so; do
  [ -e "$f" ] || continue
  if [ -L "$f" ]; then
    cp --remove-destination "$(readlink -f "$f")" "$f"
  fi
done
rm -f lib*.so.*

# 2. Strip, set SONAME, fix dependency names.
for f in lib*.so; do
  "$STRIP" --strip-unneeded "$f"
  patchelf --set-soname "$f" "$f"
  for needed in $(patchelf --print-needed "$f"); do
    case "$needed" in
      lib*.so.*)
        plain="${needed%%.so.*}.so"
        # only our own libraries; system ones (libc, libm, libz...) stay as they are
        [ -e "$plain" ] && patchelf --replace-needed "$needed" "$plain" "$f" ;;
    esac
  done
done

echo "Normalized: $(ls lib*.so | tr '\n' ' ')"
for f in lib*.so; do
  echo "  $f needs: $(patchelf --print-needed "$f" | tr '\n' ' ')"
done
