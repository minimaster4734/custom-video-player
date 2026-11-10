package com.leandro.customplayer.nativebridge

import android.view.Surface

/**
 * Frozen JNI contract to libcustomplayer.so. Thin declarations only; all
 * behaviour lives in native code. One player per process (the native side
 * keeps a single implicit instance; nativeInit() just creates it).
 *
 * The package is "nativebridge" rather than "native" because "native" is a
 * Java keyword and cannot be used as a package segment from Java tooling.
 */
object NativePlayer {
    init {
        System.loadLibrary("customplayer")
    }

    external fun nativeInit(): Long
    external fun nativeSetSurface(surface: Surface?)
    external fun nativeSetDataSource(path: String): Boolean
    external fun nativePrepare(): Boolean
    external fun nativeStart()
    external fun nativePause()
    external fun nativeSeekTo(ms: Long)
    external fun nativeRelease()
    external fun nativeGetDuration(): Long
    external fun nativeGetCurrentPosition(): Long
    external fun nativeIsPlaying(): Boolean
    external fun nativeSetSpeed(speed: Float)
    external fun nativeSetLooping(loop: Boolean)
    external fun nativeGetVideoWidth(): Int
    external fun nativeGetVideoHeight(): Int
    /** JSON: {"fps":..,"dropped":..,"cpu":..,"decoder":"..","av_offset_ms":..} */
    external fun nativeGetStats(): String
}
