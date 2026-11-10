#include "decoder.h"
#include <android/log.h>

#define LOG_TAG "customplayer_decoder"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

const AVCodec *pickDecoder(enum AVCodecID id) {
    const char *names[4] = {nullptr, nullptr, nullptr, nullptr};

    switch (id) {
        case AV_CODEC_ID_AV1:
            names[0] = "libdav1d";
            names[1] = "av1";
            break;
        case AV_CODEC_ID_HEVC:
            names[0] = "hevc";
            break;
        case AV_CODEC_ID_VVC:
            /* FFmpeg's built-in VVC decoder (native since FFmpeg 7.0). There is
             * no libvvdec/VVdeC wrapper in FFmpeg -- checked 7.1 through 9.1
             * and master -- so this is the only VVC path through FFmpeg. VVC is
             * far heavier per pixel than HEVC/AV1: expect small (144p-480p)
             * files to be watchable on this hardware, larger ones not. If a
             * faster VVC decoder ever appears in FFmpeg, add its name above
             * "vvc" here and nothing else changes. */
            names[0] = "vvc";
            break;
        case AV_CODEC_ID_H264:
            names[0] = "h264";
            break;
        case AV_CODEC_ID_VP9:
            names[0] = "vp9";
            break;
        case AV_CODEC_ID_AAC:
            /* FFmpeg's own native AAC decoder is known-incomplete for the
             * USAC/xHE-AAC speech-coding path (ACELP/TCX) at low
             * bitrates -- confirmed by hand against real files in the
             * xheaac-player sibling project. libfdk_aac (Fraunhofer's own
             * reference decoder) handles the full spec correctly and is
             * also a perfectly good plain-AAC decoder, so it's preferred
             * for AAC generally, not just for xHE-AAC specifically. */
            names[0] = "libfdk_aac";
            names[1] = "aac";
            break;
        case AV_CODEC_ID_OPUS:
            names[0] = "opus";
            break;
        case AV_CODEC_ID_VORBIS:
            names[0] = "vorbis";
            break;
        case AV_CODEC_ID_MP3:
            names[0] = "mp3float";
            break;
        case AV_CODEC_ID_AC3:
            names[0] = "ac3";
            break;
        case AV_CODEC_ID_EAC3:
            names[0] = "eac3";
            break;
        default:
            break;
    }

    for (int i = 0; names[i] != nullptr; ++i) {
        const AVCodec *c = avcodec_find_decoder_by_name(names[i]);
        if (c && c->id == id) {
            LOGI("Selected decoder '%s' for codec id %d", names[i], (int)id);
            return c;
        }
    }

    LOGW("No preferred decoder found for codec id %d, falling back to default", (int)id);
    return avcodec_find_decoder(id);
}
