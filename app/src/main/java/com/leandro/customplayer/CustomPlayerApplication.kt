package com.leandro.customplayer

import android.app.Application

/** Applies the saved theme before any Activity exists, so the right
 *  light/dark appearance is there from the first frame. */
class CustomPlayerApplication : Application() {
    override fun onCreate() {
        super.onCreate()
        AppSettings.applyTheme(AppSettings(this).getTheme())
    }
}
