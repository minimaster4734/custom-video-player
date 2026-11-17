package com.leandro.customplayer

import android.content.Context
import android.util.AttributeSet
import android.view.SurfaceView
import android.view.View

/** SurfaceView that sizes itself for the chosen scale mode. The native
 *  renderer draws the full frame into whatever buffer it is given; the system
 *  compositor scales that buffer to this view's size, so choosing this view's
 *  size is all the scaling logic needed.
 *   FIT      keeps the aspect ratio and fits inside the screen (black bars)
 *   STRETCH  fills the whole screen, ignoring the aspect ratio
 *   ORIGINAL shows the video at its own pixel size, centred (cropped by the
 *            screen if the video is larger than it) */
class PlayerView @JvmOverloads constructor(
    context: Context, attrs: AttributeSet? = null
) : SurfaceView(context, attrs) {

    enum class ScaleMode { FIT, STRETCH, ORIGINAL }

    var scaleMode: ScaleMode = ScaleMode.FIT
        set(value) {
            if (field == value) return
            field = value
            requestLayout()
        }

    private var videoW = 0
    private var videoH = 0

    fun setVideoSize(w: Int, h: Int) {
        if (w == videoW && h == videoH) return
        videoW = w
        videoH = h
        requestLayout()
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val maxW = View.MeasureSpec.getSize(widthMeasureSpec)
        val maxH = View.MeasureSpec.getSize(heightMeasureSpec)
        if (videoW <= 0 || videoH <= 0) {
            setMeasuredDimension(maxW, maxH)
            return
        }
        when (scaleMode) {
            ScaleMode.STRETCH -> { setMeasuredDimension(maxW, maxH); return }
            ScaleMode.ORIGINAL -> { setMeasuredDimension(videoW, videoH); return }
            ScaleMode.FIT -> {}
        }
        var w = maxW
        var h = (maxW.toLong() * videoH / videoW).toInt()
        if (h > maxH) {
            h = maxH
            w = (maxH.toLong() * videoW / videoH).toInt()
        }
        setMeasuredDimension(w, h)
    }
}
