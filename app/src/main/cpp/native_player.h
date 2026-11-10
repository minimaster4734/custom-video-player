#ifndef CUSTOMPLAYER_NATIVE_PLAYER_H
#define CUSTOMPLAYER_NATIVE_PLAYER_H

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#include <android/native_window.h>
#include <atomic>
#include <pthread.h>
#include <string>
#include <time.h>

#include "packet_queue.h"
#include "frame_queue.h"
#include "audio_output.h"
#include "renderer.h"
#include "sync.h"

/* Owns demuxer, decoders, renderer, audio output, and the worker threads
 * (demux / video-decode / audio-decode+play / render). One instance per
 * app, created in nativeInit() and torn down in nativeRelease() -- see the
 * JNI section of native_player.cpp for why it's a single implicit instance
 * despite nativeInit() returning a handle.
 *
 * SEEK GENERATIONS. Every seek bumps seek_gen_. The demuxer stamps each
 * packet with the generation it was read under (AVPacket::opaque); decode
 * threads stamp each frame with the generation they decoded it under
 * (AVFrame::opaque). Anything whose stamp != seek_gen_ is stale and is
 * silently thrown away, which makes "seek while a thread is blocked
 * mid-push/mid-write" safe without any lock-step handshake between
 * threads. */
class NativePlayer {
public:
    NativePlayer();
    ~NativePlayer();

    /* May be called with window == nullptr (surface destroyed, e.g. app
     * backgrounded) -- rendering just pauses until a new one arrives.
     * Takes ownership of one ANativeWindow reference. */
    void setSurface(ANativeWindow *window);

    bool setDataSource(const std::string &path);
    bool prepare();
    void start();
    void pause();
    void seekTo(int64_t ms);
    void release();

    int64_t getDuration() const { return duration_ms_.load(); }
    int64_t getCurrentPosition();
    bool isPlaying() const { return state_.load() == State::Playing; }
    void setSpeed(float speed);
    void setLooping(bool loop) { looping_.store(loop); }
    int getVideoWidth() const { return video_width_.load(); }
    int getVideoHeight() const { return video_height_.load(); }
    std::string getStats();

private:
    enum class State { Idle, Prepared, Playing, Paused, Stopped };

    static void *demuxThreadTrampoline(void *arg);
    static void *videoDecodeThreadTrampoline(void *arg);
    static void *audioDecodeThreadTrampoline(void *arg);
    static void *renderThreadTrampoline(void *arg);

    void demuxLoop();
    void videoDecodeLoop();
    void audioDecodeLoop();
    void renderLoop();

    void requestSeek(int64_t ms);
    void onStreamFinished(bool video, uint32_t gen);
    int64_t ptsToMs(int64_t pts, AVRational tb) const;

    /* Master clock (see native_player.cpp "CLOCK" section). */
    int64_t clockPositionMs();
    int64_t clockPositionLocked();         // caller holds clock_mutex_
    void anchorAudioClock(uint32_t gen, int64_t pts_ms);
    void anchorWallClock(uint32_t gen, int64_t pts_ms);
    bool clockAnchoredFor(uint32_t gen);

    AVFormatContext *fmt_ctx_ = nullptr;
    AVCodecContext *video_ctx_ = nullptr;
    AVCodecContext *audio_ctx_ = nullptr;
    SwrContext *swr_ctx_ = nullptr;        // audio thread only
    SwsContext *sws_ctx_ = nullptr;        // video decode thread only
    int video_stream_index_ = -1;
    int audio_stream_index_ = -1;
    AVRational video_time_base_{1, 1000};
    AVRational audio_time_base_{1, 1000};
    int64_t start_ms_ = 0;                 // container start_time, subtracted from all PTS

    int audio_out_rate_ = 48000;           // rate/channels actually opened on OpenSL
    int audio_out_channels_ = 2;

    PacketQueue video_packets_;
    PacketQueue audio_packets_;
    FrameQueue video_frames_;

    AudioOutput audio_output_;
    Renderer renderer_;
    Sync sync_;

    pthread_t demux_thread_{};
    pthread_t video_decode_thread_{};
    pthread_t audio_decode_thread_{};
    pthread_t render_thread_{};
    bool demux_started_ = false;
    bool video_decode_started_ = false;
    bool audio_decode_started_ = false;
    bool render_started_ = false;

    std::string path_;
    std::atomic<State> state_{State::Idle};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> looping_{false};
    std::atomic<float> speed_{1.0f};

    /* Surface handoff: set from whatever thread calls nativeSetSurface
     * (normally the UI thread), consumed by the render thread, which is the
     * only thread allowed to touch the EGL context it creates from it. */
    pthread_mutex_t surface_mutex_;
    pthread_cond_t surface_cond_;
    ANativeWindow *pending_window_ = nullptr;
    bool window_changed_ = false;
    uint32_t surface_seq_ = 0;             // bumped by setSurface()
    uint32_t surface_applied_seq_ = 0;     // set by the render thread once applied

    /* Seek handoff: consumed by the demux thread at the top of its loop. */
    pthread_mutex_t seek_mutex_;
    pthread_cond_t seek_cond_;
    bool seek_pending_ = false;
    int64_t seek_target_ms_ = 0;
    std::atomic<uint32_t> seek_gen_{0};
    std::atomic<uint32_t> demux_gen_{0};         // generation the packet queues are valid for (set AFTER the post-seek flush)
    std::atomic<int64_t> discard_before_ms_{-1}; // drop decoded output earlier than this (post-seek)
    std::atomic<int64_t> pending_pos_ms_{0};     // position to report until the clock re-anchors

    /* End-of-stream bookkeeping (see onStreamFinished). */
    pthread_mutex_t end_mutex_;
    bool video_finished_ = true;
    bool audio_finished_ = true;
    bool has_video_ = false;
    bool has_audio_ = false;

    /* CLOCK -- protected by clock_mutex_. */
    pthread_mutex_t clock_mutex_;
    uint32_t clock_gen_ = 0xFFFFFFFFu;     // generation the anchor below belongs to
    bool clock_audio_mode_ = false;
    int64_t clock_base_ms_ = 0;
    int64_t clock_anchor_frames_ = 0;
    struct timespec clock_anchor_wall_{};
    float clock_rate_ = 1.0f;
    bool clock_running_ = false;           // wall mode only

    std::string video_decoder_name_;
    std::string audio_decoder_name_;
    std::atomic<int> video_width_{0};
    std::atomic<int> video_height_{0};
    std::atomic<int64_t> duration_ms_{0};

    /* Stats (getStats() is polled from the UI thread). */
    std::atomic<int> frames_rendered_since_stat_{0};
    std::atomic<int> frames_dropped_{0};
    std::atomic<int64_t> last_av_offset_ms_{0};
    std::atomic<int64_t> dec_us_{0};            // smoothed decode time per frame (microseconds)
    std::atomic<int64_t> conv_us_{0};           // smoothed pixel-format conversion time per frame
    std::atomic<int> src_pix_fmt_{-1};          // pixel format the decoder produces
    struct timespec last_stat_wall_{};
    int64_t last_stat_cpu_ns_ = 0;
    double last_fps_ = 0.0;
    double last_cpu_ = 0.0;
};

#endif /* CUSTOMPLAYER_NATIVE_PLAYER_H */
