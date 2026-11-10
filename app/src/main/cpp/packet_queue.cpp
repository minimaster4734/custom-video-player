#include "packet_queue.h"

PacketQueue::PacketQueue() {
    pthread_mutex_init(&mutex_, nullptr);
    pthread_cond_init(&cond_, nullptr);
}

PacketQueue::~PacketQueue() {
    flush();
    pthread_cond_destroy(&cond_);
    pthread_mutex_destroy(&mutex_);
}

void PacketQueue::push(AVPacket *pkt) {
    pthread_mutex_lock(&mutex_);
    queue_.push(pkt);
    bytes_ += pkt->size;
    pthread_cond_signal(&cond_);
    pthread_mutex_unlock(&mutex_);
}

void PacketQueue::signalEof() {
    pthread_mutex_lock(&mutex_);
    eof_ = true;
    pthread_cond_signal(&cond_);
    pthread_mutex_unlock(&mutex_);
}

bool PacketQueue::pop(AVPacket **out_pkt, bool *out_eof) {
    pthread_mutex_lock(&mutex_);
    while (queue_.empty() && !eof_ && !aborted_) {
        pthread_cond_wait(&cond_, &mutex_);
    }
    bool got = false;
    if (!queue_.empty()) {
        *out_pkt = queue_.front();
        queue_.pop();
        bytes_ -= (*out_pkt)->size;
        got = true;
    } else {
        if (out_eof) *out_eof = eof_;
    }
    pthread_mutex_unlock(&mutex_);
    return got;
}

void PacketQueue::flush() {
    pthread_mutex_lock(&mutex_);
    while (!queue_.empty()) {
        AVPacket *pkt = queue_.front();
        queue_.pop();
        av_packet_free(&pkt);
    }
    bytes_ = 0;
    eof_ = false;
    pthread_mutex_unlock(&mutex_);
}

void PacketQueue::abort() {
    pthread_mutex_lock(&mutex_);
    aborted_ = true;
    pthread_cond_broadcast(&cond_);
    pthread_mutex_unlock(&mutex_);
}

int PacketQueue::size() {
    pthread_mutex_lock(&mutex_);
    int n = (int)queue_.size();
    pthread_mutex_unlock(&mutex_);
    return n;
}

int64_t PacketQueue::bytes() {
    pthread_mutex_lock(&mutex_);
    int64_t n = bytes_;
    pthread_mutex_unlock(&mutex_);
    return n;
}
