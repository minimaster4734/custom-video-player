#include "audio_output.h"
#include <android/log.h>
#include <cstdlib>
#include <cstring>
#include <ctime>

#define LOG_TAG "customplayer_audio"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

AudioOutput::AudioOutput() {
    pthread_mutex_init(&mutex_, nullptr);
    pthread_cond_init(&cond_, nullptr);
}

AudioOutput::~AudioOutput() {
    close();
    pthread_cond_destroy(&cond_);
    pthread_mutex_destroy(&mutex_);
}

bool AudioOutput::open(int sample_rate, int channels) {
    if (opened_) close();

    sample_rate_ = sample_rate;
    channels_ = channels;

    SLresult result;

    result = slCreateEngine(&engine_object_, 0, nullptr, 0, nullptr, nullptr);
    if (result != SL_RESULT_SUCCESS) {
        LOGE("slCreateEngine failed: %d", (int)result);
        return false;
    }
    (*engine_object_)->Realize(engine_object_, SL_BOOLEAN_FALSE);
    (*engine_object_)->GetInterface(engine_object_, SL_IID_ENGINE, &engine_engine_);

    result = (*engine_engine_)->CreateOutputMix(engine_engine_, &output_mix_object_, 0, nullptr, nullptr);
    if (result != SL_RESULT_SUCCESS) {
        LOGE("CreateOutputMix failed: %d", (int)result);
        return false;
    }
    (*output_mix_object_)->Realize(output_mix_object_, SL_BOOLEAN_FALSE);

    SLDataLocator_AndroidSimpleBufferQueue loc_bufq = {
        SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, (SLuint32)kMaxBuffered};

    SLuint32 channel_mask = (channels_ >= 2)
        ? (SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT)
        : SL_SPEAKER_FRONT_CENTER;

    SLDataFormat_PCM format_pcm = {
        SL_DATAFORMAT_PCM,
        (SLuint32)channels_,
        (SLuint32)(sample_rate_ * 1000), // OpenSL ES wants milliHz
        SL_PCMSAMPLEFORMAT_FIXED_16,
        SL_PCMSAMPLEFORMAT_FIXED_16,
        channel_mask,
        SL_BYTEORDER_LITTLEENDIAN
    };

    SLDataSource audio_src = {&loc_bufq, &format_pcm};

    SLDataLocator_OutputMix loc_outmix = {SL_DATALOCATOR_OUTPUTMIX, output_mix_object_};
    SLDataSink audio_sink = {&loc_outmix, nullptr};

    const SLInterfaceID ids[1] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
    const SLboolean req[1] = {SL_BOOLEAN_TRUE};

    result = (*engine_engine_)->CreateAudioPlayer(
        engine_engine_, &player_object_, &audio_src, &audio_sink, 1, ids, req);
    if (result != SL_RESULT_SUCCESS) {
        LOGE("CreateAudioPlayer failed: %d", (int)result);
        return false;
    }
    (*player_object_)->Realize(player_object_, SL_BOOLEAN_FALSE);

    (*player_object_)->GetInterface(player_object_, SL_IID_PLAY, &player_play_);
    (*player_object_)->GetInterface(player_object_, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &player_bufq_);

    (*player_bufq_)->RegisterCallback(player_bufq_, bufferQueueCallbackTrampoline, this);

    (*player_play_)->SetPlayState(player_play_, SL_PLAYSTATE_PLAYING);

    partial_.data = (int16_t *)malloc(kBufferFrames * channels_ * sizeof(int16_t));
    partial_.frames = 0;

    opened_ = true;
    frames_written_.store(0);
    LOGI("AudioOutput opened: %d Hz, %d ch", sample_rate_, channels_);
    return true;
}

void AudioOutput::close() {
    if (!opened_) return;
    opened_ = false;

    if (player_object_) {
        (*player_object_)->Destroy(player_object_);
        player_object_ = nullptr;
        player_play_ = nullptr;
        player_bufq_ = nullptr;
    }
    if (output_mix_object_) {
        (*output_mix_object_)->Destroy(output_mix_object_);
        output_mix_object_ = nullptr;
    }
    if (engine_object_) {
        (*engine_object_)->Destroy(engine_object_);
        engine_object_ = nullptr;
        engine_engine_ = nullptr;
    }

    pthread_mutex_lock(&mutex_);
    for (auto &c : ready_queue_) free(c.data);
    ready_queue_.clear();
    for (auto &c : in_flight_) free(c.data);
    in_flight_.clear();
    if (partial_.data) {
        free(partial_.data);
        partial_.data = nullptr;
    }
    outstanding_ = 0;
    pthread_cond_broadcast(&cond_);
    pthread_mutex_unlock(&mutex_);
}

void AudioOutput::bufferQueueCallbackTrampoline(SLAndroidSimpleBufferQueueItf bq, void *context) {
    (void)bq;
    static_cast<AudioOutput *>(context)->onBufferConsumed();
}

void AudioOutput::onBufferConsumed() {
    pthread_mutex_lock(&mutex_);
    if (!in_flight_.empty()) {
        Chunk done = in_flight_.front();
        in_flight_.pop_front();
        frames_written_.fetch_add(done.frames);
        free(done.data);
        outstanding_--;
    }
    enqueueReadyLocked();
    pthread_cond_signal(&cond_);
    pthread_mutex_unlock(&mutex_);
}

/* Caller must hold mutex_. */
void AudioOutput::enqueueReadyLocked() {
    while (outstanding_ < kMaxBuffered - 1 && !ready_queue_.empty() && player_bufq_) {
        Chunk c = ready_queue_.front();
        ready_queue_.pop_front();
        SLresult result = (*player_bufq_)->Enqueue(
            player_bufq_, c.data, (SLuint32)(c.frames * channels_ * sizeof(int16_t)));
        if (result != SL_RESULT_SUCCESS) {
            LOGE("Enqueue failed: %d", (int)result);
            free(c.data);
            continue;
        }
        in_flight_.push_back(c);
        outstanding_++;
    }
}

void AudioOutput::write(const int16_t *data, int num_frames) {
    if (!opened_) return;
    int written = 0;
    while (written < num_frames) {
        pthread_mutex_lock(&mutex_);
        if (!opened_) {
            pthread_mutex_unlock(&mutex_);
            return;
        }
        // Backpressure: if we already have plenty buffered, wait for the
        // callback to free some up rather than growing ready_queue_
        // unbounded.
        while (opened_ && (int)(ready_queue_.size() + in_flight_.size()) >= kMaxBuffered) {
            pthread_cond_wait(&cond_, &mutex_);
        }
        if (!opened_) {
            pthread_mutex_unlock(&mutex_);
            return;
        }

        int space = kBufferFrames - partial_.frames;
        int take = num_frames - written;
        if (take > space) take = space;

        memcpy(partial_.data + (size_t)partial_.frames * channels_,
               data + (size_t)written * channels_,
               (size_t)take * channels_ * sizeof(int16_t));
        partial_.frames += take;
        written += take;

        if (partial_.frames >= kBufferFrames) {
            ready_queue_.push_back(partial_);
            partial_.data = (int16_t *)malloc(kBufferFrames * channels_ * sizeof(int16_t));
            partial_.frames = 0;
            enqueueReadyLocked();
        }
        pthread_mutex_unlock(&mutex_);
    }
}

void AudioOutput::flush() {
    pthread_mutex_lock(&mutex_);
    if (player_bufq_) {
        (*player_bufq_)->Clear(player_bufq_);
    }
    for (auto &c : ready_queue_) free(c.data);
    ready_queue_.clear();
    for (auto &c : in_flight_) free(c.data);
    in_flight_.clear();
    outstanding_ = 0;
    partial_.frames = 0;
    pthread_cond_broadcast(&cond_);
    pthread_mutex_unlock(&mutex_);
}

void AudioOutput::setPaused(bool paused) {
    if (!player_play_) return;
    (*player_play_)->SetPlayState(player_play_, paused ? SL_PLAYSTATE_PAUSED : SL_PLAYSTATE_PLAYING);
}

void AudioOutput::finishAndDrain(int timeout_ms) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }

    pthread_mutex_lock(&mutex_);
    if (opened_ && partial_.data && partial_.frames > 0) {
        ready_queue_.push_back(partial_);
        partial_.data = (int16_t *)malloc(kBufferFrames * channels_ * sizeof(int16_t));
        partial_.frames = 0;
        enqueueReadyLocked();
    }
    while (opened_ && (!ready_queue_.empty() || !in_flight_.empty())) {
        if (pthread_cond_timedwait(&cond_, &mutex_, &deadline) != 0) break; // timeout
    }
    pthread_mutex_unlock(&mutex_);
}

bool AudioOutput::isDrained() {
    pthread_mutex_lock(&mutex_);
    bool drained = ready_queue_.empty() && in_flight_.empty() && partial_.frames == 0;
    pthread_mutex_unlock(&mutex_);
    return drained;
}
