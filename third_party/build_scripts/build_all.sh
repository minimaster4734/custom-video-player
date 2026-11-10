#!/usr/bin/env bash
# Single entry point for (re)building every native dependency, driven
# entirely by third_party/versions.json. Bump a version there and re-run
# this -- no other file needs to change for a routine dependency upgrade.
#
# Needs: ANDROID_NDK_HOME, jq, meson, ninja, cmake, git, make, nasm-free
# (ARM build), pkg-config, patchelf.
set -euo pipefail

: "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME}"
API=24
ABI=armeabi-v7a
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PROJECT_ROOT=$(cd "$ROOT/.." && pwd)
SRC="$ROOT/src"
INSTALL="$ROOT/$ABI"
MANIFEST="$ROOT/versions.json"

mkdir -p "$SRC" "$INSTALL"

jqget() { jq -r "$1" "$MANIFEST"; }

# Fetches one dependency at exactly the version versions.json names. A stamp
# file records what was fetched, so an unchanged manifest reuses the existing
# checkout and a changed one cleanly re-clones (no half-updated trees).
fetch_repo() {
  local name=$1 url=$2 ref=$3 ref_type=$4 pin=$5
  local dir="$SRC/$name"
  local stamp="$url|$ref|$ref_type|$pin"

  if [ -d "$dir/.git" ] && [ "$(cat "$dir/.fetched-stamp" 2>/dev/null)" = "$stamp" ]; then
    echo "[$name] already at $ref ${pin:+(pin $pin)}"
    return
  fi
  rm -rf "$dir"
  echo "[$name] fetching $ref ${pin:+(pin $pin)}"
  if [ -n "$pin" ] && [ "$pin" != "null" ]; then
    # An exact commit overrides the ref (needs full history to find it).
    git clone "$url" "$dir"
    git -C "$dir" checkout "$pin"
  else
    # Branch or tag: shallow clone (FFmpeg's full history is huge).
    git clone --depth 1 --branch "$ref" "$url" "$dir"
  fi
  echo "$stamp" > "$dir/.fetched-stamp"
}

for dep in fdk_aac dav1d ffmpeg; do
  enabled=$(jqget ".\"$dep\".enabled // true")
  [ "$enabled" = "false" ] && continue
  fetch_repo "$dep" \
    "$(jqget ".\"$dep\".url")" \
    "$(jqget ".\"$dep\".ref")" \
    "$(jqget ".\"$dep\".ref_type")" \
    "$(jqget ".\"$dep\".pin")"
done

# Order matters: fdk-aac and dav1d must be installed before FFmpeg's
# configure step, since FFmpeg links against them.
"$ROOT/build_scripts/build_fdk_aac.sh" "$ABI" "$API" "$INSTALL"
"$ROOT/build_scripts/build_dav1d.sh" "$ABI" "$API" "$INSTALL"
"$ROOT/build_scripts/build_ffmpeg.sh" "$ABI" "$API" "$INSTALL"

# Android only loads libraries whose file names end in plain ".so", but the
# builds above produce versioned names (libavcodec.so.61, ...). Fix that.
"$ROOT/build_scripts/normalize_sonames.sh" "$PROJECT_ROOT/app/src/main/jniLibs/$ABI"

echo "All native dependencies built for ${ABI}."
