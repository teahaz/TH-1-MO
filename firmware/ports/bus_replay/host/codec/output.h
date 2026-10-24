/*
 * The common outputs of decoded items (docs/decode-output.md), generated from the item tree
 * and its kinds, so every protocol gets them without code of its own: text, for people, and
 * JSON lines, for tools. A decoder's own exports (codec.h, struct codec_export) are beside
 * them.
 */

#ifndef CODEC_OUTPUT_H
#define CODEC_OUTPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "codec/item.h"


/*
 * level: children up to this detail level are written. hex: words as hex also for kinds
 * read as text. pretty: the text output writes each item's most interpreted line only
 * (docs/decode-output.md, Pretty), and level is not used. name_width: the text output's
 * decoder column. rate turns ticks into seconds.
 */
struct output_options {
    unsigned int level;
    bool hex;
    bool pretty;
    size_t name_width;
    uint32_t rate;
};


int output_text(FILE *out, const struct codec_item *item, const struct output_options *opt);
int output_jsonl(FILE *out, const struct codec_item *item, const struct output_options *opt);

#endif
