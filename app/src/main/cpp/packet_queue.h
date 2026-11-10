#ifndef CUSTOMPLAYER_PACKET_QUEUE_H
#define CUSTOMPLAYER_PACKET_QUEUE_H

extern "C" {
#include <libavcodec/avcodec.h>
}
#include <pthread.h>
#include <queue>

/* Thread-safe queue of AVPacket* between the demux thread (producer) and
 * a decode thread (consumer). Unbounded by design -- packets are small
 * relative to decoded frames, and FrameQueue is the one that actually
 * bounds memory use (see frame_queue.h). */
class PacketQueue {
public:
    PacketQueue();
    ~PacketQueue();

    /* Takes ownership of pkt. */
    void push(AVPacket *pkt);

    /* Marks that no more packets are coming for this stream (demuxer hit
     * EOF or the file only has one of audio/video). Wakes any waiter. */
    void signalEof();

    /* Blocks until a packet is available, EOF is signalled, or abort() is
     * called. Returns true with *out_pkt set if a packet was popped;
     * false (with *out_eof set accordingly) otherwise. Caller owns the
     * returned packet and must av_packet_free() it. */
    bool pop(AVPacket **out_pkt, bool *out_eof);

    /* Drops all queued packets and clears the EOF flag -- used on seek. */
    void flush();

    /* Unblocks any waiter permanently (shutdown). */
    void abort();

    int size();
    /* Total payload bytes currently queued -- the demuxer throttles on this
     * (not on packet count) so a 4K stream can't exhaust RAM while a tiny
     * audio-only stream still buffers plenty of seconds. */
    int64_t bytes();

private:
    std::queue<AVPacket *> queue_;
    pthread_mutex_t mutex_;
    pthread_cond_t cond_;
    int64_t bytes_ = 0;
    bool eof_ = false;
    bool aborted_ = false;
};

#endif /* CUSTOMPLAYER_PACKET_QUEUE_H */
