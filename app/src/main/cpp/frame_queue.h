#ifndef CUSTOMPLAYER_FRAME_QUEUE_H
#define CUSTOMPLAYER_FRAME_QUEUE_H

extern "C" {
#include <libavutil/frame.h>
}
#include <pthread.h>
#include <queue>

/* Thread-safe, BOUNDED queue of AVFrame* between a decode thread
 * (producer) and the render thread (consumer). Bounded (default: 4
 * frames) so a slow renderer can't let decoded-but-undisplayed frames
 * pile up and exhaust memory on a low-RAM device -- push() blocks until
 * there's room, which naturally paces the decode thread to render speed. */
class FrameQueue {
public:
    explicit FrameQueue(int capacity = 4);
    ~FrameQueue();

    /* Takes ownership of frame. Blocks if the queue is already at
     * capacity, until space frees up or abort() is called (in which case
     * it returns without pushing and the caller still owns -- and must
     * free -- the frame). Returns true if the frame was pushed. */
    bool push(AVFrame *frame);

    void signalEof();

    /* Blocks until a frame is available, EOF is signalled, or abort() is
     * called. Caller owns the returned frame and must av_frame_free() it. */
    bool pop(AVFrame **out_frame, bool *out_eof);

    /* Non-blocking variant of pop(): returns true with *out_frame set if a
     * frame was available; otherwise false, with *out_eof telling the
     * caller whether the stream has ended (true) or just nothing is ready
     * yet (false). */
    bool tryPop(AVFrame **out_frame, bool *out_eof);

    void flush();
    void abort();
    int size();

private:
    std::queue<AVFrame *> queue_;
    int capacity_;
    pthread_mutex_t mutex_;
    pthread_cond_t not_empty_;
    pthread_cond_t not_full_;
    bool eof_ = false;
    bool aborted_ = false;
};

#endif /* CUSTOMPLAYER_FRAME_QUEUE_H */
