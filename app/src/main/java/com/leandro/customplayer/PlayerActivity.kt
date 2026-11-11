package com.leandro.customplayer

import android.content.pm.ActivityInfo
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.SurfaceHolder
import android.view.View
import android.view.WindowManager
import android.widget.SeekBar
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import com.leandro.customplayer.databinding.ActivityPlayerBinding
import com.leandro.customplayer.nativebridge.NativePlayer
import org.json.JSONObject
import java.util.Locale

/**
 * Full-screen playback. All decoding/rendering/sync lives in native code;
 * this class only wires the surface, the controls and the lifecycle.
 *
 * Native contract notes: exactly one player exists per process, calls are
 * made from the UI thread, except prepare() which blocks on file I/O and so
 * runs on a worker thread (the surface may be attached before or after it
 * finishes -- native code handles both).
 */
class PlayerActivity : AppCompatActivity(), SurfaceHolder.Callback {

    private lateinit var binding: ActivityPlayerBinding
    private lateinit var prefs: PlaybackPrefs
    private val ui = Handler(Looper.getMainLooper())

    private var source: VideoFiles.Source? = null
    private var worker: Thread? = null
    @Volatile private var prepared = false
    private var released = false
    private var posKey = ""
    private var durationMs = 0L

    private val speeds = floatArrayOf(0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f)
    private var speedIdx = 2
    private var looping = false
    private var userSeeking = false
    private var controlsVisible = true
    private var statsVisible = false
    private var resumeAfterStop = false
    private var lastSaveMs = 0L

    private val ticker = object : Runnable {
        override fun run() {
            if (released) return
            if (prepared) updateUi()
            ui.postDelayed(this, 250)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityPlayerBinding.inflate(layoutInflater)
        setContentView(binding.root)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        enterImmersive()

        prefs = PlaybackPrefs(this)
        val uri = intent.data
        if (uri == null) {
            finish()
            return
        }
        posKey = uri.toString()

        // The native player must exist before the surface callbacks fire.
        NativePlayer.nativeInit()
        binding.playerView.holder.addCallback(this)

        binding.playerView.setOnClickListener { setControlsVisible(!controlsVisible) }
        binding.btnPlay.setOnClickListener { togglePlay() }
        binding.btnBack.setOnClickListener { seekBy(-10_000) }
        binding.btnForward.setOnClickListener { seekBy(10_000) }
        binding.btnSpeed.setOnClickListener {
            speedIdx = (speedIdx + 1) % speeds.size
            NativePlayer.nativeSetSpeed(speeds[speedIdx])
            binding.btnSpeed.text = String.format(Locale.US, "%.2fx", speeds[speedIdx]).replace(".00x", ".0x")
        }
        binding.btnLoop.setOnClickListener {
            looping = !looping
            NativePlayer.nativeSetLooping(looping)
            binding.btnLoop.setText(if (looping) R.string.loop_on else R.string.loop_off)
        }
        // Some phones ignore the system rotation setting, so rotate explicitly.
        binding.btnRotate.setOnClickListener {
            val landscape = resources.configuration.orientation ==
                android.content.res.Configuration.ORIENTATION_LANDSCAPE
            requestedOrientation = if (landscape) ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
                                   else ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE
        }
        binding.btnStats.setOnClickListener {
            statsVisible = !statsVisible
            binding.statsText.visibility = if (statsVisible) View.VISIBLE else View.GONE
        }
        binding.seekBar.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(sb: SeekBar?, progress: Int, fromUser: Boolean) {
                if (fromUser) binding.positionText.text = formatTime(progress.toLong())
            }
            override fun onStartTrackingTouch(sb: SeekBar?) { userSeeking = true }
            override fun onStopTrackingTouch(sb: SeekBar?) {
                userSeeking = false
                if (prepared) NativePlayer.nativeSeekTo((sb?.progress ?: 0).toLong())
            }
        })

        openAsync(uri)
    }

    private fun openAsync(uri: android.net.Uri) {
        worker = Thread {
            val src = VideoFiles.resolve(this, uri)
            var ok = false
            if (src != null) {
                source = src
                ok = NativePlayer.nativeSetDataSource(src.path) && NativePlayer.nativePrepare()
            }
            ui.post { if (!released) onPrepareResult(ok) }
        }.also { it.start() }
    }

    private fun onPrepareResult(ok: Boolean) {
        if (!ok) {
            Toast.makeText(this, R.string.cannot_play, Toast.LENGTH_LONG).show()
            finish()
            return
        }
        prepared = true
        durationMs = NativePlayer.nativeGetDuration()
        binding.playerView.setVideoSize(
            NativePlayer.nativeGetVideoWidth(), NativePlayer.nativeGetVideoHeight()
        )
        // Wide video: start in landscape (the Rotate button can undo this).
        if (NativePlayer.nativeGetVideoWidth() > NativePlayer.nativeGetVideoHeight()) {
            requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
        }
        binding.seekBar.max = durationMs.toInt()
        binding.durationText.text = formatTime(durationMs)

        val saved = prefs.getPosition(posKey)
        if (saved > 5_000 && saved < durationMs - 5_000) NativePlayer.nativeSeekTo(saved)
        NativePlayer.nativeStart()
        ui.post(ticker)
        ui.postDelayed({ setControlsVisible(false) }, 3000)
    }

    private fun updateUi() {
        val pos = NativePlayer.nativeGetCurrentPosition()
        val playing = NativePlayer.nativeIsPlaying()
        if (!userSeeking) {
            binding.seekBar.progress = pos.toInt()
            binding.positionText.text = formatTime(pos)
        }
        binding.btnPlay.setImageResource(if (playing) R.drawable.ic_pause else R.drawable.ic_play)
        binding.btnPlay.contentDescription = getString(if (playing) R.string.pause else R.string.play)

        if (statsVisible) {
            try {
                val j = JSONObject(NativePlayer.nativeGetStats())
                binding.statsText.text = String.format(
                    Locale.US,
                    "%s  %.1f fps\ndropped %d  cpu %.0f%%\nA/V %+d ms\n%s %s  cores %d\ndecode %.1f ms  convert %.1f ms\ndraw %.1f ms  frames queued %d  packets queued %d\nworst: decode %.0f ms  draw %.0f ms  late-drop %d ms",
                    j.optString("decoder"), j.optDouble("fps"), j.optInt("dropped"),
                    j.optDouble("cpu"), j.optInt("av_offset_ms"),
                    j.optString("res"), j.optString("pix"), j.optInt("cores"),
                    j.optDouble("dec_ms"), j.optDouble("conv_ms"),
                    j.optDouble("rend_ms"), j.optInt("vq"), j.optInt("pq"),
                    j.optDouble("dmax_ms"), j.optDouble("rmax_ms"), j.optInt("late_ms")
                )
            } catch (e: Exception) { }
        }

        val now = System.currentTimeMillis()
        if (now - lastSaveMs > 5_000) {
            lastSaveMs = now
            savePosition(pos, playing)
        }
    }

    private fun savePosition(pos: Long, playing: Boolean) {
        // Reached the end: next time start from the beginning.
        if (durationMs > 0 && pos >= durationMs - 1_000 && !playing) prefs.clearPosition(posKey)
        else if (pos > 0) prefs.setPosition(posKey, pos)
    }

    private fun togglePlay() {
        if (!prepared) return
        if (NativePlayer.nativeIsPlaying()) NativePlayer.nativePause() else NativePlayer.nativeStart()
    }

    private fun seekBy(deltaMs: Long) {
        if (!prepared) return
        val target = (NativePlayer.nativeGetCurrentPosition() + deltaMs).coerceIn(0, durationMs)
        NativePlayer.nativeSeekTo(target)
    }

    private fun setControlsVisible(visible: Boolean) {
        controlsVisible = visible
        binding.controls.visibility = if (visible) View.VISIBLE else View.GONE
    }

    @Suppress("DEPRECATION")
    private fun enterImmersive() {
        window.decorView.systemUiVisibility = (View.SYSTEM_UI_FLAG_FULLSCREEN
                or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                or View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                or View.SYSTEM_UI_FLAG_LAYOUT_STABLE)
    }

    private fun formatTime(ms: Long): String {
        val total = ms / 1000
        val h = total / 3600
        val m = (total % 3600) / 60
        val s = total % 60
        return if (h > 0) String.format(Locale.US, "%d:%02d:%02d", h, m, s)
        else String.format(Locale.US, "%d:%02d", m, s)
    }

    // ---- surface

    override fun surfaceCreated(holder: SurfaceHolder) {
        NativePlayer.nativeSetSurface(holder.surface)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        // Blocks briefly until the render thread has let go of the surface.
        NativePlayer.nativeSetSurface(null)
    }

    // ---- lifecycle

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) enterImmersive()
    }

    override fun onStop() {
        super.onStop()
        if (prepared) {
            resumeAfterStop = NativePlayer.nativeIsPlaying()
            NativePlayer.nativePause()
            savePosition(NativePlayer.nativeGetCurrentPosition(), false)
        }
    }

    override fun onStart() {
        super.onStart()
        if (prepared && resumeAfterStop) {
            resumeAfterStop = false
            NativePlayer.nativeStart()
        }
    }

    override fun onDestroy() {
        released = true
        ui.removeCallbacksAndMessages(null)
        // prepare() may still be running on the worker; native release must
        // not race it.
        try { worker?.join() } catch (e: InterruptedException) { }
        if (prepared) savePosition(NativePlayer.nativeGetCurrentPosition(), false)
        NativePlayer.nativeRelease()
        try { source?.pfd?.close() } catch (e: Exception) { }
        super.onDestroy()
    }
}
