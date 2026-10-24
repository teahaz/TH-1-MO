/*
 * Decoding for the modes that decode: decode, from a file, and capture --decode, live. Both
 * take the same decoder options (decoding_parse()) and write the items the same way, as
 * text, JSON lines or a decoder's own export (docs/decode-output.md). The mode passes the
 * capture's records in through the handler decoding_handler() fills, as they arrive.
 */

#ifndef MODES_DECODING_H
#define MODES_DECODING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "capture/capture.h"
#include "codec/codec.h"
#include "codec/output.h"
#include "common/capture_info.h"


#define DECODERS_MAX 16
#define CHIP_ARGS_MAX 256

// decoding_parse() found -h or --help.
#define DECODING_HELP 1

/*
 * The options, from decoding_parse(): the decoders with their arguments, the output, and
 * for a file the path and the span. bad is the argument the parse stopped at, when it was
 * not one of these.
 *
 * The session, from decoding_start(): how far the input has got (tick), the span to write
 * in ticks, and the first error feeding the session or writing an item.
 */
struct decoding {
    const struct codec_decoder *defs[DECODERS_MAX];
    const char *args[DECODERS_MAX];
    char chip_args[DECODERS_MAX][CHIP_ARGS_MAX];
    size_t count;
    const char *format;
    const char *level;
    bool hex;
    bool pretty;
    const char *path;
    const char *from_arg;
    const char *to_arg;
    bool follow;
    const char *bad;

    struct codec *codec;
    FILE *out;
    struct output_options opt;
    bool exported;
    uint64_t tick;
    uint64_t from;
    uint64_t to;
    int err;
};


int decoding_parse(struct decoding *d, int argc, char **argv, bool live, const char *who);
int decoding_start(struct decoding *d, const struct capture_info *info, const char *who);
void decoding_handler(struct decoding *d, struct capture_handler *handler);
void decoding_drain(struct decoding *d);
int decoding_finish(struct decoding *d);
void decoding_free(struct decoding *d);

#endif
