#include "frame_queue.h"

FrameQueue::FrameQueue(int capacity) : capacity_(capacity) {
    pthread_mutex_init(&mutex_, nullptr);
    pthread_cond_init(&not_empty_, nullptr);
    pthread_cond_init(&not_full_, nullptr);
}

FrameQueue::~FrameQueue() {
    flush();
    pthread_cond_destroy(&not_full_);
    pthread_cond_destroy(&not_empty_);
    pthread_mutex_destroy(&mutex_);
}

bool FrameQueue::push(AVFrame *frame) {
    pthread_mutex_lock(&mutex_);
    while ((int)queue_.size() >= capacity_ && !aborted_) {
        pthread_cond_wait(&not_full_, &mutex_);
    }
    if (aborted_) {
        pthread_mutex_unlock(&mutex_);
        return false;
    }
    queue_.push(frame);
    pthread_cond_signal(&not_empty_);
    pthread_mutex_unlock(&mutex_);
    return true;
}

void FrameQueue::signalEof() {
    pthread_mutex_lock(&mutex_);
    eof_ = true;
    pthread_cond_signal(&not_empty_);
    pthread_mutex_unlock(&mutex_);
}

bool FrameQueue::pop(AVFrame **out_frame, bool *out_eof) {
    pthread_mutex_lock(&mutex_);
    while (queue_.empty() && !eof_ && !aborted_) {
        pthread_cond_wait(&not_empty_, &mutex_);
    }
    bool got = false;
    if (!queue_.empty()) {
        *out_frame = queue_.front();
        queue_.pop();
        got = true;
        pthread_cond_signal(&not_full_);
    } else {
        if (out_eof) *out_eof = eof_;
    }
    pthread_mutex_unlock(&mutex_);
    return got;
}

void FrameQueue::flush() {
    pthread_mutex_lock(&mutex_);
    while (!queue_.empty()) {
        AVFrame *f = queue_.front();
        queue_.pop();
        av_frame_free(&f);
    }
    eof_ = false;
    pthread_cond_broadcast(&not_full_);
    pthread_mutex_unlock(&mutex_);
}

void FrameQueue::abort() {
    pthread_mutex_lock(&mutex_);
    aborted_ = true;
    pthread_cond_broadcast(&not_empty_);
    pthread_cond_broadcast(&not_full_);
    pthread_mutex_unlock(&mutex_);
}

int FrameQueue::size() {
    pthread_mutex_lock(&mutex_);
    int n = (int)queue_.size();
    pthread_mutex_unlock(&mutex_);
    return n;
}

bool FrameQueue::tryPop(AVFrame **out_frame, bool *out_eof) {
    pthread_mutex_lock(&mutex_);
    bool got = false;
    if (!queue_.empty()) {
        *out_frame = queue_.front();
        queue_.pop();
        got = true;
        pthread_cond_signal(&not_full_);
    } else if (out_eof) {
        *out_eof = eof_;
    }
    pthread_mutex_unlock(&mutex_);
    return got;
}
