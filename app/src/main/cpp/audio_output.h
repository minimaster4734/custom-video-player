#ifndef CUSTOMPLAYER_AUDIO_OUTPUT_H
#define CUSTOMPLAYER_AUDIO_OUTPUT_H

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <atomic>
#include <cstdint>
#include <deque>
#include <pthread.h>

/* OpenSL ES output (not AAudio -- unavailable on API 24, this project's
 * minSdk). Takes resampled S16 interleaved PCM from the audio decode
 * thread via write(), chunks it into fixed-size buffers, and keeps
 * OpenSL ES's own buffer queue topped up via its completion callback.
 * Also IS the A/V sync master clock: framesWritten() tracks how much
 * audio has actually been handed off to OpenSL (not just decoded), which
 * is what Sync compares video PTS against. */
class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();

    bool open(int sample_rate, int channels);
    void close();

    /* Blocks (backpressure) if too many buffers are already queued/in
     * flight -- this is what paces the audio decode thread to roughly
     * real-time rather than racing ahead and burning memory. */
    void write(const int16_t *data, int num_frames);

    /* Frames actually handed to OpenSL so far. Combined with sampleRate(),
     * this is the audio master clock: framesWritten() * 1000 / sampleRate()
     * gives the current audio position in ms. */
    int64_t framesWritten() const { return frames_written_.load(); }

    /* Smoothed clock: framesWritten() only moves when a whole 4096-frame
     * buffer (~90 ms) finishes, which makes video frames bunch up and get
     * dropped. This adds the time elapsed inside the buffer that is playing
     * right now (capped at that buffer's length, frozen while paused). */
    int64_t framesPlayedSmooth() const;
    int sampleRate() const { return sample_rate_; }

    /* Drops any buffered-but-unplayed audio -- call on seek. */
    void flush();

    void setPaused(bool paused);

    /* End-of-stream: hands the final partially-filled chunk to OpenSL (write()
     * only ever enqueues full chunks) and blocks until everything queued has
     * been played, or timeout_ms passes, or flush()/close() is called. */
    void finishAndDrain(int timeout_ms);
    bool isDrained();

private:
    static void bufferQueueCallbackTrampoline(SLAndroidSimpleBufferQueueItf bq, void *context);
    void onBufferConsumed();
    void enqueueReadyLocked();
    void updateFrontLocked();
    static int64_t monoNs();

    struct Chunk {
        int16_t *data;
        int frames; // valid frames in this chunk (<= kBufferFrames)
    };

    SLObjectItf engine_object_ = nullptr;
    SLEngineItf engine_engine_ = nullptr;
    SLObjectItf output_mix_object_ = nullptr;
    SLObjectItf player_object_ = nullptr;
    SLPlayItf player_play_ = nullptr;
    SLAndroidSimpleBufferQueueItf player_bufq_ = nullptr;

    static constexpr int kBufferFrames = 4096;
    static constexpr int kMaxBuffered = 6; // ready_queue_ + in_flight_ combined cap

    std::deque<Chunk> ready_queue_;  // built, waiting to be handed to OpenSL
    std::deque<Chunk> in_flight_;    // currently sitting in OpenSL's own queue
    int outstanding_ = 0;            // == in_flight_.size(), tracked separately for clarity

    Chunk partial_{nullptr, 0};      // being filled by write() until it's full

    std::atomic<int64_t> frames_written_{0};
    std::atomic<int> front_frames_{0};          // length of the buffer now playing (0 = none)
    std::atomic<int64_t> front_start_ns_{0};    // when it (effectively) started playing
    std::atomic<bool> paused_{false};
    std::atomic<int64_t> frozen_extra_ns_{0};   // elapsed-in-buffer captured at pause
    int sample_rate_ = 44100;
    int channels_ = 2;
    bool opened_ = false;

    pthread_mutex_t mutex_;
    pthread_cond_t cond_;
};

#endif /* CUSTOMPLAYER_AUDIO_OUTPUT_H */
