package com.leandro.customplayer

import android.content.Context
import android.os.Build
import androidx.appcompat.app.AppCompatDelegate

/**
 * App-wide settings: theme and folder-picker mode. Theme is applied via
 * AppCompatDelegate.setDefaultNightMode(), which gives deterministic
 * light/dark control independent of the OS -- important because Android 6
 * has no system-wide dark mode at all, so relying on automatic day/night
 * detection there is unreliable (some OEM skins fake it inconsistently).
 * Explicit control here is what actually fixes mismatched text/icon colors.
 */
class AppSettings(context: Context) {
    private val prefs = context.getSharedPreferences("app_settings", Context.MODE_PRIVATE)

    companion object {
        const val THEME_LIGHT = "light"
        const val THEME_DARK = "dark"
        const val THEME_SYSTEM = "system"

        const val PICKER_LEGACY = "legacy"
        const val PICKER_SAF = "saf"

        fun applyTheme(mode: String) {
            AppCompatDelegate.setDefaultNightMode(
                when (mode) {
                    THEME_LIGHT -> AppCompatDelegate.MODE_NIGHT_NO
                    THEME_DARK -> AppCompatDelegate.MODE_NIGHT_YES
                    else -> AppCompatDelegate.MODE_NIGHT_FOLLOW_SYSTEM
                }
            )
        }
    }

    fun getTheme(): String = prefs.getString("theme", THEME_LIGHT) ?: THEME_LIGHT

    fun setTheme(mode: String) {
        prefs.edit().putString("theme", mode).apply()
        applyTheme(mode)
    }

    /** Legacy (java.io.File) browsing only works reliably pre-scoped-storage
     *  (Android 9 / API 28 and below), which is exactly this app's target
     *  device -- so that's the default there. On Android 10+ it defaults to
     *  the system picker instead, since scoped storage would otherwise make
     *  the legacy browser see mostly-empty folders. Either can be picked
     *  explicitly in Settings regardless. */
    fun getPickerMode(): String {
        val default = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) PICKER_SAF else PICKER_LEGACY
        return prefs.getString("picker_mode", default) ?: default
    }

    fun setPickerMode(mode: String) {
        prefs.edit().putString("picker_mode", mode).apply()
    }

    fun getLastLegacyPath(): String? = prefs.getString("last_legacy_path", null)

    fun setLastLegacyPath(path: String) {
        prefs.edit().putString("last_legacy_path", path).apply()
    }
}
