package com.leandro.customplayer

import android.content.Context
import android.util.AttributeSet
import android.view.SurfaceView
import android.view.View

/** SurfaceView that letterboxes itself to the video's aspect ratio. The
 *  native renderer draws the full frame into whatever buffer it is given;
 *  the system compositor scales that buffer to this view's size, so getting
 *  the view's aspect right is all the "fit" logic needed. */
class PlayerView @JvmOverloads constructor(
    context: Context, attrs: AttributeSet? = null
) : SurfaceView(context, attrs) {

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
        var w = maxW
        var h = (maxW.toLong() * videoH / videoW).toInt()
        if (h > maxH) {
            h = maxH
            w = (maxH.toLong() * videoW / videoH).toInt()
        }
        setMeasuredDimension(w, h)
    }
}
