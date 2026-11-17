package com.leandro.customplayer

import android.content.Context

/** Remembers where playback stopped, per file, and the last folder. */
class PlaybackPrefs(context: Context) {
    private val prefs = context.getSharedPreferences("playback", Context.MODE_PRIVATE)

    fun getPosition(key: String): Long = prefs.getLong("pos:$key", 0L)

    fun setPosition(key: String, ms: Long) {
        prefs.edit().putLong("pos:$key", ms).apply()
    }

    fun clearPosition(key: String) {
        prefs.edit().remove("pos:$key").apply()
    }

    /** Either an absolute path (legacy browser) or a SAF tree Uri string. */
    var lastFolder: String?
        get() = prefs.getString("last_folder", null)
        set(value) { prefs.edit().putString("last_folder", value).apply() }

    /** 0 = fit, 1 = stretch, 2 = original size. */
    var scaleMode: Int
        get() = prefs.getInt("scale_mode", 0)
        set(value) { prefs.edit().putInt("scale_mode", value).apply() }
}
