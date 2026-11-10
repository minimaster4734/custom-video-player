# Custom Player (Android video player, armv7)

Personal-use video player for old Android phones: **armeabi-v7a, Android 7+
(API 24)**, 100 % software decoding, no MediaCodec. Built from the same
ideas as the xHE-AAC audio player (FFmpeg + libfdk-aac, built on GitHub
Actions, driven from Termux).

| Piece | What it does |
|---|---|
| Container | FFmpeg demuxers: **mp4/mov/m4v/3gp, mkv/webm, avi** (local files) |
| Video | **AV1** (dav1d), **H.264**, **H.265/HEVC**, **VP9**, **VVC/H.266** (FFmpeg's built-in decoder) |
| Audio | **xHE-AAC / AAC** (libfdk-aac), Opus, Vorbis, MP3, AC-3/E-AC-3 |
| Output | OpenGL ES 2.0 (video), OpenSL ES (audio) |
| Sync | Audio is the master clock; late video frames are dropped |

## Getting the APK

Same routine as the audio player. From Termux, inside the project folder:

```
git add -A && git commit -m "Initial commit" && git push -u origin main
```

Then on GitHub: **Actions -> Build Custom Player APK -> Run workflow** (it
also runs on every push). When it finishes, download the artifact
`custom-player-debug-armeabi-v7a` and install the APK.

The **first** build compiles FFmpeg, dav1d and fdk-aac and takes a good
while. Those results are cached, so later builds (only app code changed)
are fast. The cache is rebuilt automatically whenever
`third_party/versions.json` or `third_party/build_scripts/` change.

## Using it

* Top left: **settings** (theme, which folder picker to use).
* Top right: **folder** icon (and "Open file" in its overflow menu).
* Tap a video to play. Tap the picture to show/hide the controls.
* Controls: seek bar, -10 s / +10 s, play/pause, speed (0.5x-2x), loop,
  and **Stats** (decoder, fps, dropped frames, CPU, A/V offset in ms).
* Playback position is remembered per file; the app can also be chosen as
  the opener for video files from a file manager.

**Speed note:** speed changes are done by resampling the audio, so the
pitch rises/falls with speed (no time-stretching). Sync stays correct.

## Layout

```
third_party/
  versions.json            <- the ONLY place dependency versions live
  build_scripts/           <- build_all.sh drives everything from versions.json
app/src/main/cpp/          <- native player (C++17)
  native_player.cpp        demux / decode / render threads, clock, JNI
  decoder.cpp              chooses a decoder by name (see below)
  renderer.cpp             EGL + GLES2 YUV420 -> RGB (limited/full range, 601/709)
  audio_output.cpp         OpenSL ES buffer queue; also drives the clock
app/src/main/java/...      Kotlin UI (library list, player screen, settings)
```

### JNI contract (frozen)

Kotlin `object com.leandro.customplayer.nativebridge.NativePlayer`:
`nativeInit, nativeSetSurface, nativeSetDataSource, nativePrepare,
nativeStart, nativePause, nativeSeekTo, nativeRelease, nativeGetDuration,
nativeGetCurrentPosition, nativeIsPlaying, nativeSetSpeed,
nativeSetLooping, nativeGetVideoWidth, nativeGetVideoHeight,
nativeGetStats` (JSON: `fps, dropped, cpu, decoder, av_offset_ms`).
One player per process. (The package says `nativebridge`, not `native`,
because `native` is a Java keyword.)

## Upgrading a dependency

1. Edit the version in `third_party/versions.json`
   (`ref` = branch or tag; set `pin` to a commit hash to freeze exactly).
2. Push. The workflow notices the change and rebuilds the native libraries.
3. If the build breaks, the error is almost always an FFmpeg `configure`
   flag that was renamed or removed in the new version -- that is in
   `third_party/build_scripts/build_ffmpeg.sh` (see "Known history").

A weekly workflow (**Check dependency updates**) opens one GitHub issue
when newer FFmpeg / dav1d / fdk-aac versions exist. It never changes the
build by itself.

The app code was tested against FFmpeg **7.1**. Newer branches (8.x, 9.x)
should work, but have not been tried with this player.

## About VVC

FFmpeg has **no wrapper for Fraunhofer's VVdeC** in any version (checked
7.1 through 9.1 and master), so VVC plays through FFmpeg's own built-in
decoder, which is included in the first build. VVC is very heavy: on this
hardware expect small files (144p-480p) to be watchable, big ones not.
If a faster VVC decoder ever lands in FFmpeg, only the name list in
`decoder.cpp` and one flag in `build_ffmpeg.sh` need to change.
(Hooking VVdeC up directly, outside FFmpeg, is possible but is a separate
piece of work.)

## Not in this version

* **Network / m3u8** (needs the INTERNET permission and a TLS library).
* Subtitles, multiple audio tracks, picture rotation metadata.
* 10-bit and 4:2:2/4:4:4 video is converted to 8-bit 4:2:0 in software
  first, which costs CPU on a slow phone.

## Troubleshooting

* **Crash at start / "library ... not found":** the native libraries did
  not get their names normalised. `build_all.sh` ends with a
  "Normalized: ..." list; every library should depend only on plain
  `lib*.so` names.
* **"Cannot play this file":** the container or codec is not compiled in
  (see the table above), or the file is damaged. `adb logcat -s customplayer ffmpeg customplayer_decoder`
  shows the reason.
* **Black picture but sound:** a decoder exists but the surface was not
  ready; also check the stats overlay for the decoder name and fps.
* **Many dropped frames:** the file is too heavy for the phone's CPU; the
  stats overlay shows CPU use and the dropped count.

## Known history (why the scripts look like this)

* Android only loads `lib*.so`; FFmpeg/dav1d/fdk-aac build versioned names
  (`libavcodec.so.61`). `normalize_sonames.sh` fixes that.
* `--disable-postproc` was removed in new FFmpeg versions, so it is not
  used. XML comments must not contain `--`.
