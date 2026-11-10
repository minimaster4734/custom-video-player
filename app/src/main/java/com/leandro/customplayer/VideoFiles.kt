package com.leandro.customplayer

import android.content.Context
import android.net.Uri
import android.os.ParcelFileDescriptor
import java.io.File

/** What the native side can open: the demuxers compiled into FFmpeg are
 *  mov (mp4/m4v/mov/3gp), matroska (mkv/webm) and avi. */
object VideoFiles {
    private val EXTENSIONS = setOf("mp4", "m4v", "mov", "3gp", "mkv", "webm", "avi")

    fun isVideoName(name: String): Boolean =
        EXTENSIONS.contains(name.substringAfterLast('.', "").lowercase())

    /** A path the native player can open, plus the descriptor (if any) that
     *  must stay open for as long as playback uses that path. */
    class Source(val path: String, val pfd: ParcelFileDescriptor?)

    /**
     * file:// URIs open directly. content:// URIs (system picker, file
     * managers) are opened as a descriptor and handed to FFmpeg as
     * /proc/self/fd/N, which its plain "file" protocol can read and seek --
     * no copying, and the native code needs no Android-specific I/O.
     */
    fun resolve(context: Context, uri: Uri): Source? {
        return when (uri.scheme) {
            "file" -> uri.path?.let { Source(it, null) }
            "content" -> {
                val pfd = try {
                    context.contentResolver.openFileDescriptor(uri, "r")
                } catch (e: Exception) {
                    null
                } ?: return null
                Source("/proc/self/fd/${pfd.fd}", pfd)
            }
            null -> Source(uri.toString(), null).takeIf { File(uri.toString()).exists() }
            else -> null
        }
    }
}
