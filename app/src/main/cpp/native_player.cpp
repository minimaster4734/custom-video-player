#include "native_player.h"
#include "decoder.h"

#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
}

#define LOG_TAG "customplayer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

/* Demux throttle: stop reading ahead once this much compressed data is
 * queued (counted across BOTH streams -- throttling per stream deadlocks
 * when one stream's queue is full waiting on the audio clock while the
 * other, starved, is what drives that clock). */
constexpr int64_t kMaxQueuedBytes = 24 * 1024 * 1024;
constexpr int kMaxQueuedPackets = 4000;

int64_t nowNs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int64_t tsToNs(const struct timespec &ts) {
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

void sleepMs(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, nullptr);
}

uint32_t frameTag(const AVFrame *f) { return (uint32_t)(uintptr_t)f->opaque; }
uint32_t packetTag(const AVPacket *p) { return (uint32_t)(uintptr_t)p->opaque; }

/* OpenSL ES only accepts a fixed set of sample rates; anything else (e.g.
 * the 38400 Hz an xHE-AAC file may report) is resampled to 48000. */
int pickOutputRate(int rate) {
    static const int kSupported[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
    for (int r : kSupported) if (r == rate) return rate;
    return 48000;
}

bool openDecoder(AVStream *st, int threads, AVCodecContext **out, std::string *name) {
    const AVCodec *codec = pickDecoder(st->codecpar->codec_id);
    if (!codec) {
        LOGE("No decoder available for codec id %d", (int)st->codecpar->codec_id);
        return false;
    }
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx) return false;
    if (avcodec_parameters_to_context(ctx, st->codecpar) < 0) {
        avcodec_free_context(&ctx);
        return false;
    }
    ctx->pkt_timebase = st->time_base;
    if (threads > 0) {
        ctx->thread_count = threads;
        ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    }
    int r = avcodec_open2(ctx, codec, nullptr);
    if (r < 0) {
        char err[128];
        av_strerror(r, err, sizeof(err));
        LOGE("avcodec_open2(%s) failed: %s", codec->name, err);
        avcodec_free_context(&ctx);
        return false;
    }
    *out = ctx;
    *name = codec->name;
    return true;
}

bool isYuv420(int fmt) {
    return fmt == AV_PIX_FMT_YUV420P || fmt == AV_PIX_FMT_YUVJ420P;
}

} // namespace

/* ======================================================================
 * Lifecycle
 * ==================================================================== */

NativePlayer::NativePlayer() : video_frames_(4) {
    pthread_mutex_init(&surface_mutex_, nullptr);
    pthread_cond_init(&surface_cond_, nullptr);
    pthread_mutex_init(&seek_mutex_, nullptr);
    pthread_cond_init(&seek_cond_, nullptr);
    pthread_mutex_init(&end_mutex_, nullptr);
    pthread_mutex_init(&clock_mutex_, nullptr);
}

NativePlayer::~NativePlayer() {
    release();
    pthread_mutex_destroy(&clock_mutex_);
    pthread_mutex_destroy(&end_mutex_);
    pthread_cond_destroy(&seek_cond_);
    pthread_mutex_destroy(&seek_mutex_);
    pthread_cond_destroy(&surface_cond_);
    pthread_mutex_destroy(&surface_mutex_);
}

bool NativePlayer::setDataSource(const std::string &path) {
    if (state_.load() != State::Idle) return false;
    path_ = path;
    return true;
}

bool NativePlayer::prepare() {
    if (state_.load() != State::Idle || path_.empty() || fmt_ctx_) return false;

    stop_requested_.store(false);

    fmt_ctx_ = avformat_alloc_context();
    if (!fmt_ctx_) return false;
    fmt_ctx_->interrupt_callback.opaque = this;
    fmt_ctx_->interrupt_callback.callback = [](void *p) -> int {
        return static_cast<NativePlayer *>(p)->stop_requested_.load() ? 1 : 0;
    };

    int r = avformat_open_input(&fmt_ctx_, path_.c_str(), nullptr, nullptr);
    if (r < 0) {
        char err[128];
        av_strerror(r, err, sizeof(err));
        LOGE("avformat_open_input(%s) failed: %s", path_.c_str(), err);
        fmt_ctx_ = nullptr; // freed by avformat_open_input on failure
        return false;
    }
    if (avformat_find_stream_info(fmt_ctx_, nullptr) < 0) {
        LOGE("avformat_find_stream_info failed");
        avformat_close_input(&fmt_ctx_);
        return false;
    }

    int v = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (v >= 0 && (fmt_ctx_->streams[v]->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
        v = -1; // cover art in an audio file, not a video stream
    }
    int a = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_AUDIO, -1, v, nullptr, 0);
    if (v < 0 && a < 0) {
        LOGE("No playable audio or video stream in %s", path_.c_str());
        avformat_close_input(&fmt_ctx_);
        return false;
    }

    start_ms_ = (fmt_ctx_->start_time != AV_NOPTS_VALUE) ? fmt_ctx_->start_time / 1000 : 0;
    duration_ms_.store(fmt_ctx_->duration != AV_NOPTS_VALUE ? fmt_ctx_->duration / 1000 : 0);

    if (v >= 0) {
        AVStream *st = fmt_ctx_->streams[v];
        long cores = sysconf(_SC_NPROCESSORS_ONLN);
        int threads = (int)std::max(1L, std::min(4L, cores));
        if (!openDecoder(st, threads, &video_ctx_, &video_decoder_name_)) {
            avformat_close_input(&fmt_ctx_);
            return false;
        }
        video_stream_index_ = v;
        video_time_base_ = st->time_base;
        video_width_.store(video_ctx_->width);
        video_height_.store(video_ctx_->height);
        has_video_ = true;
    }

    if (a >= 0) {
        AVStream *st = fmt_ctx_->streams[a];
        if (!openDecoder(st, 0, &audio_ctx_, &audio_decoder_name_)) {
            LOGW("Audio decoder unavailable -- continuing without audio");
        } else {
            audio_stream_index_ = a;
            audio_time_base_ = st->time_base;
            int ch = audio_ctx_->ch_layout.nb_channels;
            audio_out_channels_ = ch >= 2 ? 2 : 1;
            audio_out_rate_ = pickOutputRate(audio_ctx_->sample_rate);
            if (!audio_output_.open(audio_out_rate_, audio_out_channels_)) {
                LOGW("OpenSL ES open failed -- continuing without audio");
                avcodec_free_context(&audio_ctx_);
                audio_stream_index_ = -1;
            } else {
                audio_output_.setPaused(true); // starts on start()
                has_audio_ = true;
            }
        }
    }
    if (!has_video_ && !has_audio_) {
        avformat_close_input(&fmt_ctx_);
        return false;
    }

    video_finished_ = !has_video_;
    audio_finished_ = !has_audio_;
    pending_pos_ms_.store(0);
    state_.store(State::Prepared);

    pthread_create(&demux_thread_, nullptr, demuxThreadTrampoline, this);
    demux_started_ = true;
    if (has_video_) {
        pthread_create(&video_decode_thread_, nullptr, videoDecodeThreadTrampoline, this);
        video_decode_started_ = true;
        pthread_create(&render_thread_, nullptr, renderThreadTrampoline, this);
        render_started_ = true;
    }
    if (has_audio_) {
        pthread_create(&audio_decode_thread_, nullptr, audioDecodeThreadTrampoline, this);
        audio_decode_started_ = true;
    }

    LOGI("Prepared: video=%s %dx%d audio=%s %d Hz->%d Hz %d ch, duration=%lld ms",
         has_video_ ? video_decoder_name_.c_str() : "none", video_width_.load(), video_height_.load(),
         has_audio_ ? audio_decoder_name_.c_str() : "none",
         audio_ctx_ ? audio_ctx_->sample_rate : 0, audio_out_rate_, audio_out_channels_,
         (long long)duration_ms_.load());
    return true;
}

void NativePlayer::release() {
    stop_requested_.store(true);

    video_packets_.abort();
    audio_packets_.abort();
    video_frames_.abort();
    pthread_mutex_lock(&seek_mutex_);
    pthread_cond_broadcast(&seek_cond_);
    pthread_mutex_unlock(&seek_mutex_);
    pthread_mutex_lock(&surface_mutex_);
    pthread_cond_broadcast(&surface_cond_);
    pthread_mutex_unlock(&surface_mutex_);

    // close() also wakes a decode thread blocked in AudioOutput::write().
    audio_output_.close();

    if (demux_started_) pthread_join(demux_thread_, nullptr);
    if (video_decode_started_) pthread_join(video_decode_thread_, nullptr);
    if (audio_decode_started_) pthread_join(audio_decode_thread_, nullptr);
    if (render_started_) pthread_join(render_thread_, nullptr);
    demux_started_ = video_decode_started_ = audio_decode_started_ = render_started_ = false;

    renderer_.release();

    if (swr_ctx_) swr_free(&swr_ctx_);
    if (sws_ctx_) { sws_freeContext(sws_ctx_); sws_ctx_ = nullptr; }
    if (video_ctx_) avcodec_free_context(&video_ctx_);
    if (audio_ctx_) avcodec_free_context(&audio_ctx_);
    if (fmt_ctx_) avformat_close_input(&fmt_ctx_);

    video_packets_.flush();
    audio_packets_.flush();
    video_frames_.flush();

    pthread_mutex_lock(&surface_mutex_);
    if (pending_window_) {
        ANativeWindow_release(pending_window_);
        pending_window_ = nullptr;
    }
    window_changed_ = false;
    pthread_mutex_unlock(&surface_mutex_);

    state_.store(State::Idle);
}

/* ======================================================================
 * Control surface
 * ==================================================================== */

void NativePlayer::setSurface(ANativeWindow *window) {
    pthread_mutex_lock(&surface_mutex_);
    if (pending_window_) ANativeWindow_release(pending_window_);
    pending_window_ = window;
    window_changed_ = true;
    uint32_t seq = ++surface_seq_;

    // Wait (briefly) until the render thread has actually let go of the old
    // surface -- Android expects that to have happened by the time
    // surfaceDestroyed() returns.
    if (render_started_ && !stop_requested_.load()) {
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_nsec += 500L * 1000000L;
        if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
        while (surface_applied_seq_ < seq && render_started_ && !stop_requested_.load()) {
            if (pthread_cond_timedwait(&surface_cond_, &surface_mutex_, &dl) != 0) break;
        }
    }
    pthread_mutex_unlock(&surface_mutex_);
}

void NativePlayer::start() {
    State s = state_.load();
    if (s == State::Idle) return;
    if (s == State::Stopped) requestSeek(0); // replay after the end

    state_.store(State::Playing);
    if (has_audio_) audio_output_.setPaused(false);

    pthread_mutex_lock(&clock_mutex_);
    if (!clock_audio_mode_ && clock_gen_ == seek_gen_.load()) {
        clock_anchor_wall_ = {};
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        clock_anchor_wall_ = ts;
        clock_running_ = true;
    }
    pthread_mutex_unlock(&clock_mutex_);
}

void NativePlayer::pause() {
    State s = state_.load();
    if (s != State::Playing) return;
    state_.store(State::Paused);
    if (has_audio_) audio_output_.setPaused(true);

    pthread_mutex_lock(&clock_mutex_);
    if (!clock_audio_mode_ && clock_running_ && clock_gen_ == seek_gen_.load()) {
        clock_base_ms_ = clockPositionLocked();
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        clock_anchor_wall_ = ts;
        clock_running_ = false;
    }
    pthread_mutex_unlock(&clock_mutex_);
}

void NativePlayer::seekTo(int64_t ms) {
    State s = state_.load();
    if (s == State::Idle) return;
    int64_t dur = duration_ms_.load();
    if (ms < 0) ms = 0;
    if (dur > 0 && ms > dur) ms = dur;
    if (s == State::Stopped) {
        // Seeking after the end leaves us paused on the new position.
        state_.store(State::Paused);
        if (has_audio_) audio_output_.setPaused(true);
    }
    requestSeek(ms);
}

void NativePlayer::setSpeed(float speed) {
    if (speed < 0.25f) speed = 0.25f;
    if (speed > 4.0f) speed = 4.0f;
    if (std::fabs(speed - speed_.load()) < 0.001f) return;
    speed_.store(speed);
    if (state_.load() != State::Idle) {
        // Speed is baked into the audio resampler, which is rebuilt on every
        // seek generation -- so a speed change is just a seek to "here".
        State s = state_.load();
        int64_t pos = getCurrentPosition();
        if (s == State::Stopped) return;
        requestSeek(pos);
    }
}

void NativePlayer::requestSeek(int64_t ms) {
    pthread_mutex_lock(&seek_mutex_);
    seek_pending_ = true;
    seek_target_ms_ = ms;
    pending_pos_ms_.store(ms);
    seek_gen_.fetch_add(1);
    pthread_cond_broadcast(&seek_cond_);
    pthread_mutex_unlock(&seek_mutex_);
}

int64_t NativePlayer::getCurrentPosition() {
    State s = state_.load();
    if (s == State::Idle) return 0;
    int64_t dur = duration_ms_.load();
    if (s == State::Stopped) return dur;
    int64_t pos = clockPositionMs();
    if (pos < 0) pos = 0;
    if (dur > 0 && pos > dur) pos = dur;
    return pos;
}

int64_t NativePlayer::ptsToMs(int64_t pts, AVRational tb) const {
    return av_rescale_q(pts, tb, AVRational{1, 1000}) - start_ms_;
}

/* ======================================================================
 * CLOCK
 *
 * Normal case ("audio mode"): media position = base + audio frames the
 * sound card has actually consumed since the anchor, scaled by playback
 * speed. Pausing OpenSL freezes it for free.
 *
 * "Wall mode" is used when there is no audio to follow (video-only file, or
 * the audio stream ended before the video): position = base + elapsed
 * monotonic time * speed, frozen while paused.
 *
 * Both are tagged with the seek generation they belong to; until a clock
 * for the current generation exists, the position being reported is just
 * the seek target (pending_pos_ms_).
 * ==================================================================== */

int64_t NativePlayer::clockPositionLocked() {
    if (clock_gen_ != seek_gen_.load()) return pending_pos_ms_.load();
    if (clock_audio_mode_) {
        int64_t frames = audio_output_.framesWritten() - clock_anchor_frames_;
        if (frames < 0) frames = 0;
        return clock_base_ms_ + (int64_t)((double)frames * 1000.0 * clock_rate_ / audio_out_rate_);
    }
    if (!clock_running_) return clock_base_ms_;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t elapsed_ns = tsToNs(ts) - tsToNs(clock_anchor_wall_);
    return clock_base_ms_ + (int64_t)((double)elapsed_ns / 1e6 * clock_rate_);
}

int64_t NativePlayer::clockPositionMs() {
    pthread_mutex_lock(&clock_mutex_);
    int64_t p = clockPositionLocked();
    pthread_mutex_unlock(&clock_mutex_);
    return p;
}

void NativePlayer::anchorAudioClock(uint32_t gen, int64_t pts_ms) {
    pthread_mutex_lock(&clock_mutex_);
    if (gen == seek_gen_.load()) {
        clock_gen_ = gen;
        clock_audio_mode_ = true;
        clock_base_ms_ = pts_ms;
        clock_anchor_frames_ = audio_output_.framesWritten();
        clock_rate_ = speed_.load();
    }
    pthread_mutex_unlock(&clock_mutex_);
}

void NativePlayer::anchorWallClock(uint32_t gen, int64_t pts_ms) {
    pthread_mutex_lock(&clock_mutex_);
    if (gen == seek_gen_.load()) {
        clock_gen_ = gen;
        clock_audio_mode_ = false;
        clock_base_ms_ = pts_ms;
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        clock_anchor_wall_ = ts;
        clock_rate_ = speed_.load();
        clock_running_ = (state_.load() == State::Playing);
    }
    pthread_mutex_unlock(&clock_mutex_);
}

bool NativePlayer::clockAnchoredFor(uint32_t gen) {
    pthread_mutex_lock(&clock_mutex_);
    bool ok = (clock_gen_ == gen);
    pthread_mutex_unlock(&clock_mutex_);
    return ok;
}

/* ======================================================================
 * End of stream
 *
 * Playback is over when every present stream has reported its end for the
 * CURRENT seek generation. The last one to report decides: loop (seek to
 * 0, keep playing) or stop.
 * ==================================================================== */

void NativePlayer::onStreamFinished(bool video, uint32_t gen) {
    bool both = false;
    bool audio_ended_first = false;

    pthread_mutex_lock(&end_mutex_);
    if (gen != seek_gen_.load()) { // stale report from before a seek
        pthread_mutex_unlock(&end_mutex_);
        return;
    }
    if (video) {
        video_finished_ = true;
    } else {
        audio_finished_ = true;
        audio_ended_first = has_video_ && !video_finished_;
    }
    both = video_finished_ && audio_finished_;
    pthread_mutex_unlock(&end_mutex_);

    if (audio_ended_first) {
        // The audio clock stops advancing once audio runs out; carry on
        // pacing the remaining video from the wall clock.
        anchorWallClock(gen, clockPositionMs());
    }

    if (both) {
        if (looping_.load()) {
            LOGI("End of stream -- looping");
            requestSeek(0);
        } else {
            LOGI("End of stream");
            State s = state_.load();
            if (s == State::Playing || s == State::Paused) {
                state_.store(State::Stopped);
                if (has_audio_) audio_output_.setPaused(true); // everything has played out
            }
        }
    }
}

/* ======================================================================
 * Demux thread
 * ==================================================================== */

void *NativePlayer::demuxThreadTrampoline(void *arg) {
    static_cast<NativePlayer *>(arg)->demuxLoop();
    return nullptr;
}

void NativePlayer::demuxLoop() {
    uint32_t demux_gen = 0;
    bool eof = false;

    while (!stop_requested_.load()) {
        // ---- seek handling (also parks the thread at EOF until a seek or stop)
        pthread_mutex_lock(&seek_mutex_);
        while (eof && !seek_pending_ && !stop_requested_.load()) {
            pthread_cond_wait(&seek_cond_, &seek_mutex_);
        }
        bool do_seek = seek_pending_;
        int64_t target_ms = seek_target_ms_;
        seek_pending_ = false;
        uint32_t gen = seek_gen_.load();
        pthread_mutex_unlock(&seek_mutex_);
        if (stop_requested_.load()) break;

        if (do_seek) {
            int64_t ts = (target_ms + start_ms_) * 1000; // AV_TIME_BASE units
            int r = av_seek_frame(fmt_ctx_, -1, ts, AVSEEK_FLAG_BACKWARD);
            if (r < 0) r = av_seek_frame(fmt_ctx_, -1, ts, 0);
            if (r < 0) LOGW("Seek to %lld ms failed (%d)", (long long)target_ms, r);

            // Seeking lands on the keyframe BEFORE the target; decoders drop
            // everything earlier than the exact target (see discard_before_ms_).
            discard_before_ms_.store((r >= 0 && target_ms > 0) ? target_ms : -1);

            video_packets_.flush();
            audio_packets_.flush();
            video_frames_.flush();
            audio_output_.flush();

            pthread_mutex_lock(&end_mutex_);
            video_finished_ = !has_video_;
            audio_finished_ = !has_audio_;
            pthread_mutex_unlock(&end_mutex_);

            demux_gen = gen;
            demux_gen_.store(gen); // only now are the queues valid for `gen`
            eof = false;
        }

        // ---- read-ahead throttle
        if (video_packets_.bytes() + audio_packets_.bytes() > kMaxQueuedBytes ||
            video_packets_.size() + audio_packets_.size() > kMaxQueuedPackets) {
            sleepMs(10);
            continue;
        }

        AVPacket *pkt = av_packet_alloc();
        if (!pkt) { sleepMs(10); continue; }
        int r = av_read_frame(fmt_ctx_, pkt);
        if (r < 0) {
            av_packet_free(&pkt);
            if (stop_requested_.load()) break;
            if (r != AVERROR_EOF) {
                char err[128];
                av_strerror(r, err, sizeof(err));
                LOGW("av_read_frame error: %s -- treating as end of stream", err);
            }
            eof = true;
            video_packets_.signalEof();
            audio_packets_.signalEof();
            continue;
        }

        pkt->opaque = (void *)(uintptr_t)demux_gen;
        if (pkt->stream_index == video_stream_index_) {
            video_packets_.push(pkt);
        } else if (pkt->stream_index == audio_stream_index_) {
            audio_packets_.push(pkt);
        } else {
            av_packet_free(&pkt);
        }
    }
}

/* ======================================================================
 * Video decode thread
 * ==================================================================== */

void *NativePlayer::videoDecodeThreadTrampoline(void *arg) {
    static_cast<NativePlayer *>(arg)->videoDecodeLoop();
    return nullptr;
}

void NativePlayer::videoDecodeLoop() {
    AVFrame *frame = av_frame_alloc();
    uint32_t local_gen = 0;
    uint32_t eof_done_gen = 0xFFFFFFFFu;
    int64_t last_pts_ms = 0;
    int64_t frame_dur_ms = 33;
    int64_t send_ns = 0; // time spent in avcodec_send_packet since the last frame came out
    {
        AVRational fr = fmt_ctx_->streams[video_stream_index_]->avg_frame_rate;
        if (fr.num > 0 && fr.den > 0) frame_dur_ms = std::max<int64_t>(1, 1000LL * fr.den / fr.num);
    }

    // Converts (if needed) and queues every frame the decoder currently has.
    // Returns false if the queue was aborted (shutdown).
    auto drainFrames = [&]() -> bool {
        while (!stop_requested_.load()) {
            int64_t t_recv = nowNs();
            int r = avcodec_receive_frame(video_ctx_, frame);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return true;
            if (r < 0) {
                LOGW("avcodec_receive_frame (video) error %d", r);
                return true;
            }

            {   // smoothed per-frame decode cost: time in send_packet + receive_frame
                int64_t us = (nowNs() - t_recv + send_ns) / 1000;
                send_ns = 0;
                dec_us_.store((dec_us_.load() * 9 + us) / 10);
                src_pix_fmt_.store(frame->format);
            }
            int64_t pts = frame->best_effort_timestamp;
            if (pts == AV_NOPTS_VALUE) pts = frame->pts;
            int64_t pts_ms = (pts == AV_NOPTS_VALUE) ? last_pts_ms + frame_dur_ms
                                                     : ptsToMs(pts, video_time_base_);
            last_pts_ms = pts_ms;

            int64_t dbs = discard_before_ms_.load();
            if (local_gen != seek_gen_.load() || (dbs >= 0 && pts_ms < dbs)) {
                av_frame_unref(frame);
                continue;
            }

            AVFrame *out = av_frame_alloc();
            if (!out) { av_frame_unref(frame); continue; }
            if (isYuv420(frame->format)) {
                av_frame_move_ref(out, frame);
            } else {
                // 10-bit / 4:2:2 / 4:4:4 etc: the renderer only takes 8-bit
                // 4:2:0, so convert here (off the render thread).
                sws_ctx_ = sws_getCachedContext(sws_ctx_, frame->width, frame->height,
                                                (AVPixelFormat)frame->format,
                                                frame->width, frame->height, AV_PIX_FMT_YUV420P,
                                                SWS_BILINEAR, nullptr, nullptr, nullptr);
                out->format = AV_PIX_FMT_YUV420P;
                out->width = frame->width;
                out->height = frame->height;
                out->colorspace = frame->colorspace;   // renderer picks matrix/range from these
                out->color_range = frame->color_range;
                if (!sws_ctx_ || av_frame_get_buffer(out, 32) < 0) {
                    LOGW("Pixel format conversion setup failed (fmt %d)", frame->format);
                    av_frame_free(&out);
                    av_frame_unref(frame);
                    continue;
                }
                int64_t t_conv = nowNs();
                sws_scale(sws_ctx_, frame->data, frame->linesize, 0, frame->height,
                          out->data, out->linesize);
                conv_us_.store((conv_us_.load() * 9 + (nowNs() - t_conv) / 1000) / 10);
                av_frame_unref(frame);
            }

            out->pts = pts_ms; // from here on, frame->pts is in milliseconds
            out->opaque = (void *)(uintptr_t)local_gen;
            if (!video_frames_.push(out)) {
                av_frame_free(&out);
                return false;
            }
        }
        return true;
    };

    while (!stop_requested_.load()) {
        AVPacket *pkt = nullptr;
        bool eof = false;
        if (!video_packets_.pop(&pkt, &eof)) {
            if (stop_requested_.load() || !eof) break; // aborted

            // End of stream. Only meaningful if the demuxer has finished its
            // post-seek flush for the current generation and we haven't
            // already handled this EOF.
            uint32_t cur = seek_gen_.load();
            if (demux_gen_.load() != cur || eof_done_gen == cur) {
                sleepMs(3);
                continue;
            }
            if (local_gen != cur) {
                avcodec_flush_buffers(video_ctx_);
                local_gen = cur;
            }
            avcodec_send_packet(video_ctx_, nullptr); // drain
            if (!drainFrames()) break;

            // In-band end-of-stream marker, stamped with the generation so a
            // stale one can never end a newer generation.
            AVFrame *marker = av_frame_alloc();
            if (marker) {
                marker->format = -1;
                marker->pts = AV_NOPTS_VALUE;
                marker->opaque = (void *)(uintptr_t)local_gen;
                if (!video_frames_.push(marker)) { av_frame_free(&marker); break; }
            }
            eof_done_gen = cur;
            continue;
        }

        uint32_t tag = packetTag(pkt);
        if (tag != seek_gen_.load()) { // read before the latest seek
            av_packet_free(&pkt);
            continue;
        }
        if (tag != local_gen) {
            avcodec_flush_buffers(video_ctx_);
            local_gen = tag;
        }

        int64_t t_send = nowNs();
        int r = avcodec_send_packet(video_ctx_, pkt);
        send_ns += nowNs() - t_send;
        av_packet_free(&pkt);
        if (r < 0 && r != AVERROR(EAGAIN) && r != AVERROR_EOF) {
            LOGW("avcodec_send_packet (video) error %d", r);
            continue;
        }
        if (!drainFrames()) break;
    }

    av_frame_free(&frame);
}

/* ======================================================================
 * Audio decode thread
 * ==================================================================== */

void *NativePlayer::audioDecodeThreadTrampoline(void *arg) {
    static_cast<NativePlayer *>(arg)->audioDecodeLoop();
    return nullptr;
}

void NativePlayer::audioDecodeLoop() {
    AVFrame *frame = av_frame_alloc();
    uint32_t local_gen = 0;
    uint32_t eof_done_gen = 0xFFFFFFFFu;
    uint32_t anchored_gen = 0xFFFFFFFFu;
    double next_pts_ms = 0.0;
    std::vector<int16_t> outbuf;

    int swr_in_fmt = -1, swr_in_rate = 0, swr_in_ch = 0;

    auto resetForNewGeneration = [&](uint32_t gen) {
        avcodec_flush_buffers(audio_ctx_);
        if (swr_ctx_) swr_free(&swr_ctx_);
        audio_output_.flush(); // drop anything the previous generation left queued
        local_gen = gen;
    };

    auto buildSwr = [&](const AVFrame *f) -> bool {
        if (swr_ctx_) swr_free(&swr_ctx_);
        AVChannelLayout in_layout, out_layout;
        memset(&in_layout, 0, sizeof(in_layout));
        memset(&out_layout, 0, sizeof(out_layout));
        if (f->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || f->ch_layout.nb_channels == 0) {
            av_channel_layout_default(&in_layout, std::max(1, f->ch_layout.nb_channels));
        } else {
            av_channel_layout_copy(&in_layout, &f->ch_layout);
        }
        av_channel_layout_default(&out_layout, audio_out_channels_);

        // Playback speed without a time-stretcher: resample to a LOWER output
        // rate than OpenSL is opened at, so the same audio plays in less
        // time (pitch rises with speed). The audio clock accounts for it via
        // clock_rate_, so A/V sync holds at any speed.
        float speed = speed_.load();
        int eff_rate = std::max(1000, (int)std::lround(audio_out_rate_ / speed));

        int r = swr_alloc_set_opts2(&swr_ctx_, &out_layout, AV_SAMPLE_FMT_S16, eff_rate,
                                    &in_layout, (AVSampleFormat)f->format, f->sample_rate, 0, nullptr);
        av_channel_layout_uninit(&in_layout);
        av_channel_layout_uninit(&out_layout);
        if (r < 0 || !swr_ctx_ || swr_init(swr_ctx_) < 0) {
            LOGE("swr init failed");
            if (swr_ctx_) swr_free(&swr_ctx_);
            return false;
        }
        swr_in_fmt = f->format;
        swr_in_rate = f->sample_rate;
        swr_in_ch = f->ch_layout.nb_channels;
        return true;
    };

    auto handleFrame = [&](AVFrame *f) {
        if (local_gen != seek_gen_.load()) return;
        if (f->sample_rate <= 0 || f->nb_samples <= 0) return;

        double dur_ms = f->nb_samples * 1000.0 / f->sample_rate;
        double pts_ms;
        int64_t pts = f->best_effort_timestamp;
        if (pts == AV_NOPTS_VALUE) pts = f->pts;
        pts_ms = (pts == AV_NOPTS_VALUE) ? next_pts_ms : (double)ptsToMs(pts, audio_time_base_);
        next_pts_ms = pts_ms + dur_ms;

        int64_t dbs = discard_before_ms_.load();
        if (dbs >= 0 && pts_ms + dur_ms <= (double)dbs) return; // entirely before the seek target

        if (!swr_ctx_ || swr_in_fmt != f->format || swr_in_rate != f->sample_rate ||
            swr_in_ch != f->ch_layout.nb_channels) {
            if (!buildSwr(f)) return;
        }

        float speed = speed_.load();
        int eff_rate = std::max(1000, (int)std::lround(audio_out_rate_ / speed));
        int max_out = (int)av_rescale_rnd(swr_get_delay(swr_ctx_, f->sample_rate) + f->nb_samples,
                                          eff_rate, f->sample_rate, AV_ROUND_UP) + 64;
        if ((int)outbuf.size() < max_out * audio_out_channels_) {
            outbuf.resize((size_t)max_out * audio_out_channels_);
        }
        uint8_t *out_planes[1] = {(uint8_t *)outbuf.data()};
        int n = swr_convert(swr_ctx_, out_planes, max_out,
                            (const uint8_t **)f->extended_data, f->nb_samples);
        if (n <= 0) return;

        int skip = 0;
        if (dbs >= 0 && pts_ms < (double)dbs) { // frame straddles the seek target
            skip = (int)std::min<int64_t>(n, (int64_t)((dbs - pts_ms) * eff_rate / 1000.0));
        }
        if (skip >= n) return;

        if (anchored_gen != local_gen) {
            anchorAudioClock(local_gen, (int64_t)(pts_ms + skip * 1000.0 / eff_rate));
            anchored_gen = local_gen;
        }
        audio_output_.write(outbuf.data() + (size_t)skip * audio_out_channels_, n - skip);
    };

    auto drainFrames = [&]() {
        while (!stop_requested_.load()) {
            int r = avcodec_receive_frame(audio_ctx_, frame);
            if (r < 0) return; // EAGAIN / EOF / error
            handleFrame(frame);
            av_frame_unref(frame);
        }
    };

    while (!stop_requested_.load()) {
        AVPacket *pkt = nullptr;
        bool eof = false;
        if (!audio_packets_.pop(&pkt, &eof)) {
            if (stop_requested_.load() || !eof) break;

            uint32_t cur = seek_gen_.load();
            if (demux_gen_.load() != cur || eof_done_gen == cur) {
                sleepMs(3);
                continue;
            }
            if (local_gen != cur) resetForNewGeneration(cur);

            avcodec_send_packet(audio_ctx_, nullptr); // drain
            drainFrames();

            // Let the tail actually play out before declaring the stream over.
            for (;;) {
                audio_output_.finishAndDrain(250);
                if (stop_requested_.load() || seek_gen_.load() != cur) break;
                if (audio_output_.isDrained()) break;
            }
            eof_done_gen = cur;
            if (!stop_requested_.load()) onStreamFinished(false, cur);
            continue;
        }

        uint32_t tag = packetTag(pkt);
        if (tag != seek_gen_.load()) {
            av_packet_free(&pkt);
            continue;
        }
        if (tag != local_gen) resetForNewGeneration(tag);

        int r = avcodec_send_packet(audio_ctx_, pkt);
        av_packet_free(&pkt);
        if (r < 0 && r != AVERROR(EAGAIN) && r != AVERROR_EOF) {
            LOGW("avcodec_send_packet (audio) error %d", r);
            continue;
        }
        drainFrames();
    }

    av_frame_free(&frame);
}

/* ======================================================================
 * Render thread
 *
 * Owns the EGL/GL context. For each decoded frame: wait until the master
 * clock says it is due, drop it if it is hopelessly late, otherwise draw.
 * The first frame after any seek is always shown immediately, so seeking
 * while paused updates the picture.
 * ==================================================================== */

void *NativePlayer::renderThreadTrampoline(void *arg) {
    static_cast<NativePlayer *>(arg)->renderLoop();
    return nullptr;
}

void NativePlayer::renderLoop() {
    AVFrame *pending = nullptr;    // popped, waiting for its display time
    AVFrame *last_frame = nullptr; // last frame shown, re-drawn if the surface is re-created
    uint32_t shown_gen = 0xFFFFFFFFu;
    uint32_t finished_gen = 0xFFFFFFFFu;
    int64_t unanchored_since_ns = 0;

    while (!stop_requested_.load()) {
        // ---- 1. surface changes (EGL is thread-affine, so only we touch it)
        pthread_mutex_lock(&surface_mutex_);
        if (window_changed_) {
            ANativeWindow *w = pending_window_;
            pending_window_ = nullptr;
            window_changed_ = false;
            uint32_t seq = surface_seq_;
            pthread_mutex_unlock(&surface_mutex_);

            renderer_.release();
            if (w) {
                if (!renderer_.init(w)) {
                    LOGE("Renderer init failed");
                    renderer_.release(); // also drops the window reference
                } else if (last_frame) {
                    renderer_.renderFrame(last_frame);
                }
            }

            pthread_mutex_lock(&surface_mutex_);
            surface_applied_seq_ = seq;
            pthread_cond_broadcast(&surface_cond_);
        }
        pthread_mutex_unlock(&surface_mutex_);

        uint32_t gen = seek_gen_.load();

        // ---- 2. get the next frame
        if (!pending) {
            if (finished_gen == gen) { sleepMs(10); continue; }
            if (state_.load() != State::Playing && shown_gen == gen) { sleepMs(10); continue; }

            AVFrame *f = nullptr;
            bool eof = false;
            if (!video_frames_.tryPop(&f, &eof)) { sleepMs(4); continue; }

            if (frameTag(f) != gen) { av_frame_free(&f); continue; } // stale (pre-seek)
            if (f->format < 0) {                                     // end-of-stream marker
                av_frame_free(&f);
                finished_gen = gen;
                onStreamFinished(true, gen);
                continue;
            }
            pending = f;
            unanchored_since_ns = 0;
        }

        // ---- 3. is it still current / due?
        if (frameTag(pending) != gen) {
            av_frame_free(&pending);
            pending = nullptr;
            continue;
        }

        int64_t pts_ms = pending->pts;
        bool first_of_gen = (shown_gen != gen);

        if (!first_of_gen) {
            if (state_.load() != State::Playing) { sleepMs(10); continue; } // hold the frame while paused

            if (!clockAnchoredFor(gen)) {
                bool audio_done;
                pthread_mutex_lock(&end_mutex_);
                audio_done = audio_finished_;
                pthread_mutex_unlock(&end_mutex_);
                if (!has_audio_ || audio_done) {
                    anchorWallClock(gen, pts_ms);
                } else {
                    // Audio hasn't produced its first sample for this
                    // generation yet; give it a moment, then fall back.
                    int64_t now = nowNs();
                    if (unanchored_since_ns == 0) unanchored_since_ns = now;
                    if (now - unanchored_since_ns > 700LL * 1000000LL) {
                        anchorWallClock(gen, pts_ms);
                    } else {
                        sleepMs(5);
                        continue;
                    }
                }
            }

            sync_.setAudioClockMs(clockPositionMs());
            int64_t delay = sync_.delayForVideoPtsMs(pts_ms);
            if (delay == Sync::kDropFrame) {
                frames_dropped_.fetch_add(1);
                av_frame_free(&pending);
                pending = nullptr;
                continue;
            }
            if (delay > 0) {
                sleepMs((int)std::min<int64_t>(delay, 8));
                continue;
            }
        }

        // ---- 4. display
        if (renderer_.isInitialized()) {
            int64_t t_r = nowNs();
            renderer_.renderFrame(pending);
            render_us_.store((render_us_.load() * 9 + (nowNs() - t_r) / 1000) / 10);
        }
        frames_rendered_since_stat_.fetch_add(1);
        last_av_offset_ms_.store(pts_ms - clockPositionMs());

        if (last_frame) av_frame_free(&last_frame);
        last_frame = pending;
        pending = nullptr;
        shown_gen = gen;
    }

    if (pending) av_frame_free(&pending);
    if (last_frame) av_frame_free(&last_frame);
    renderer_.release(); // must happen on this thread: it owns the EGL context
}

/* ======================================================================
 * Stats -- JSON: fps, dropped, cpu, decoder, av_offset_ms
 * ==================================================================== */

std::string NativePlayer::getStats() {
    struct timespec wall, cpu;
    clock_gettime(CLOCK_MONOTONIC, &wall);
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu);
    int64_t wall_ns = tsToNs(wall);
    int64_t cpu_ns = tsToNs(cpu);

    if (last_stat_wall_.tv_sec == 0 && last_stat_wall_.tv_nsec == 0) {
        last_stat_wall_ = wall;
        last_stat_cpu_ns_ = cpu_ns;
        frames_rendered_since_stat_.store(0);
    } else {
        double dt = (double)(wall_ns - tsToNs(last_stat_wall_)) / 1e9;
        if (dt >= 0.4) { // hold the last value if polled faster than this
            int frames = frames_rendered_since_stat_.exchange(0);
            last_fps_ = frames / dt;
            last_cpu_ = (double)(cpu_ns - last_stat_cpu_ns_) / (dt * 1e9) * 100.0; // % of ONE core
            last_stat_wall_ = wall;
            last_stat_cpu_ns_ = cpu_ns;
        }
    }

    const char *decoder = has_video_ ? video_decoder_name_.c_str()
                                     : (has_audio_ ? audio_decoder_name_.c_str() : "none");
    const char *pix = av_get_pix_fmt_name((AVPixelFormat)src_pix_fmt_.load());
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"fps\":%.1f,\"dropped\":%d,\"cpu\":%.1f,\"decoder\":\"%s\",\"av_offset_ms\":%lld,"
             "\"res\":\"%dx%d\",\"pix\":\"%s\",\"cores\":%ld,\"dec_ms\":%.1f,\"conv_ms\":%.1f,"
             "\"rend_ms\":%.1f,\"vq\":%d,\"pq\":%d}",
             last_fps_, frames_dropped_.load(), last_cpu_, decoder,
             (long long)last_av_offset_ms_.load(),
             video_width_.load(), video_height_.load(), pix ? pix : "?",
             sysconf(_SC_NPROCESSORS_ONLN), dec_us_.load() / 1000.0, conv_us_.load() / 1000.0,
             render_us_.load() / 1000.0, video_frames_.size(), video_packets_.size());
    return std::string(buf);
}

/* ======================================================================
 * JNI -- FROZEN CONTRACT (matches NativePlayer.kt)
 *
 * Kotlin: package com.leandro.customplayer.nativebridge, `object NativePlayer`.
 * (The blueprint's literal package segment "native" is a Java keyword and
 * unusable from Java tooling, so it is spelled "nativebridge".)
 *
 * nativeInit() returns a handle for contract compatibility, but the other
 * calls carry no handle argument -- by contract there is exactly one
 * player per process. All calls come from the UI thread.
 * ==================================================================== */

namespace {

NativePlayer *g_player = nullptr;

void ffmpegLog(void *, int level, const char *fmt, va_list vl) {
    if (level > AV_LOG_WARNING) return;
    char line[1024];
    vsnprintf(line, sizeof(line), fmt, vl);
    int prio = level <= AV_LOG_ERROR ? ANDROID_LOG_ERROR : ANDROID_LOG_WARN;
    __android_log_print(prio, "ffmpeg", "%s", line);
}

} // namespace

#define JNI_FN(ret, name) extern "C" JNIEXPORT ret JNICALL \
    Java_com_leandro_customplayer_nativebridge_NativePlayer_##name

JNI_FN(jlong, nativeInit)(JNIEnv *, jobject) {
    av_log_set_callback(ffmpegLog);
    if (!g_player) g_player = new NativePlayer();
    return (jlong)(intptr_t)g_player;
}

JNI_FN(void, nativeSetSurface)(JNIEnv *env, jobject, jobject surface) {
    ANativeWindow *w = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    if (!g_player) {
        if (w) ANativeWindow_release(w);
        return;
    }
    g_player->setSurface(w);
}

JNI_FN(jboolean, nativeSetDataSource)(JNIEnv *env, jobject, jstring path) {
    if (!g_player || !path) return JNI_FALSE;
    const char *c = env->GetStringUTFChars(path, nullptr);
    if (!c) return JNI_FALSE;
    bool ok = g_player->setDataSource(c);
    env->ReleaseStringUTFChars(path, c);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNI_FN(jboolean, nativePrepare)(JNIEnv *, jobject) {
    return (g_player && g_player->prepare()) ? JNI_TRUE : JNI_FALSE;
}

JNI_FN(void, nativeStart)(JNIEnv *, jobject) { if (g_player) g_player->start(); }
JNI_FN(void, nativePause)(JNIEnv *, jobject) { if (g_player) g_player->pause(); }
JNI_FN(void, nativeSeekTo)(JNIEnv *, jobject, jlong ms) { if (g_player) g_player->seekTo(ms); }

JNI_FN(void, nativeRelease)(JNIEnv *, jobject) {
    if (g_player) {
        g_player->release();
        delete g_player;
        g_player = nullptr;
    }
}

JNI_FN(jlong, nativeGetDuration)(JNIEnv *, jobject) { return g_player ? g_player->getDuration() : 0; }
JNI_FN(jlong, nativeGetCurrentPosition)(JNIEnv *, jobject) { return g_player ? g_player->getCurrentPosition() : 0; }
JNI_FN(jboolean, nativeIsPlaying)(JNIEnv *, jobject) { return (g_player && g_player->isPlaying()) ? JNI_TRUE : JNI_FALSE; }
JNI_FN(void, nativeSetSpeed)(JNIEnv *, jobject, jfloat speed) { if (g_player) g_player->setSpeed(speed); }
JNI_FN(void, nativeSetLooping)(JNIEnv *, jobject, jboolean loop) { if (g_player) g_player->setLooping(loop == JNI_TRUE); }
JNI_FN(jint, nativeGetVideoWidth)(JNIEnv *, jobject) { return g_player ? g_player->getVideoWidth() : 0; }
JNI_FN(jint, nativeGetVideoHeight)(JNIEnv *, jobject) { return g_player ? g_player->getVideoHeight() : 0; }

JNI_FN(jstring, nativeGetStats)(JNIEnv *env, jobject) {
    std::string s = g_player ? g_player->getStats() : std::string("{}");
    return env->NewStringUTF(s.c_str());
}
