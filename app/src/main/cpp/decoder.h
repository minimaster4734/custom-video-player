#ifndef CUSTOMPLAYER_DECODER_H
#define CUSTOMPLAYER_DECODER_H

extern "C" {
#include <libavcodec/avcodec.h>
}

/* Runtime, name-based decoder selection -- never hardcoded at compile
 * time. If a future FFmpeg ships a faster AV1 decoder than dav1d, or a
 * fixed native USAC/xHE-AAC decoder that no longer needs the libfdk_aac
 * detour, this is the one place that changes: add/reorder a name in the
 * preference list, nothing else in the app needs to know. */
const AVCodec *pickDecoder(enum AVCodecID id);

#endif /* CUSTOMPLAYER_DECODER_H */
