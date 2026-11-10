#ifndef CUSTOMPLAYER_SYNC_H
#define CUSTOMPLAYER_SYNC_H

#include <atomic>
#include <cstdint>

/* Audio is the master clock (per spec Section 14): video frames are
 * scheduled against it, not the other way around, because audio glitches
 * are far more noticeable to a listener than an occasionally-dropped or
 * delayed video frame. */
class Sync {
public:
    /* audio_clock_ms: current audio playback position, as reported by
     * AudioOutput::framesWritten() / sample_rate. */
    void setAudioClockMs(int64_t ms) { audio_clock_ms_.store(ms); }
    int64_t audioClockMs() const { return audio_clock_ms_.load(); }

    /* Called from the render thread for each decoded video frame. Returns
     * how many milliseconds to sleep before displaying it (0 if it's
     * already due), or kDropFrame if it's far enough behind that
     * displaying it at all would just add latency -- the caller should
     * skip straight to the next frame instead. */
    static constexpr int64_t kDropFrame = -1;
    int64_t delayForVideoPtsMs(int64_t video_pts_ms) const {
        int64_t diff = video_pts_ms - audioClockMs();
        if (diff < kDropThresholdMs) return kDropFrame;
        if (diff <= 0) return 0;
        if (diff > kMaxDelayMs) return kMaxDelayMs; // clamp: don't stall on a bad timestamp
        return diff;
    }

    void reset() { audio_clock_ms_.store(0); }

private:
    std::atomic<int64_t> audio_clock_ms_{0};

    /* A frame more than 100ms behind the audio clock is dropped rather
     * than shown late -- matches the spec's -0.1s drop threshold. */
    static constexpr int64_t kDropThresholdMs = -100;
    /* Never sleep more than 1s for a single frame -- guards against a
     * corrupt/outlier timestamp freezing playback. */
    static constexpr int64_t kMaxDelayMs = 1000;
};

#endif /* CUSTOMPLAYER_SYNC_H */
