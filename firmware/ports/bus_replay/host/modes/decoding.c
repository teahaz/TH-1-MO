/*
 * Decoding for decode and capture --decode (decoding.h). Decoders that read records are
 * given as --NAME, chip-level ones as --chip NAME@ADDR; the chip-level ones are added after
 * the others, so they can go on any of them.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "capture/capture.h"
#include "codec/codec.h"
#include "codec/item.h"
#include "codec/output.h"
#include "common/capture_info.h"
#include "common/record.h"
#include "modes/decoding.h"
#include "modes/modes.h"


static void on_record(void *ctx, const struct record *r);
static void on_heartbeat(void *ctx, const struct heartbeat *hb);
static void on_loss(void *ctx, const struct loss *loss);
static int write_item(struct decoding *d, const struct codec_item *item);
static const struct codec_export *find_export(const struct codec_decoder *def,
                                              const char *name);
static const struct codec_decoder *chip_parse(const char *spec, char *args, size_t size);
static int parse_time(const char *s, uint32_t rate, uint64_t *tick);


/*
 * The decoder options in argv[1..argc-1] (argv[0] is what they follow: the mode's name, or
 * --decode). For a file, the one argument that is not an option is its path; live, there is
 * none, and the options only a file has are refused. 0, DECODING_HELP, or -EINVAL: with a
 * message already given, or with d->bad set to the argument that is not an option here.
 */
int decoding_parse(struct decoding *d, int argc, char **argv, bool live, const char *who)
{
    memset(d, 0, sizeof(*d));
    d->format = "text";

    for (int i = 1; i < argc; i++) {
        const struct codec_decoder *def = NULL;

        if (arg_is(argv[i], "-h", "--help")) {
            return DECODING_HELP;
        } else if (arg_is(argv[i], "-f", "--format") && i + 1 < argc) {
            d->format = argv[++i];
        } else if (arg_is(argv[i], "-l", "--level") && i + 1 < argc) {
            d->level = argv[++i];
        } else if (arg_is(argv[i], "-x", "--hex")) {
            d->hex = true;
        } else if (arg_is(argv[i], "-p", "--pretty")) {
            d->pretty = true;
        } else if (live && (!strcmp(argv[i], "--from") || !strcmp(argv[i], "--to") ||
                            arg_is(argv[i], "-F", "--follow"))) {
            fprintf(stderr, "%s: %s reads a file; a live capture has no use for it\n", who,
                    argv[i]);
            return -EINVAL;
        } else if (!strcmp(argv[i], "--from") && i + 1 < argc) {
            d->from_arg = argv[++i];
        } else if (!strcmp(argv[i], "--to") && i + 1 < argc) {
            d->to_arg = argv[++i];
        } else if (arg_is(argv[i], "-F", "--follow")) {
            d->follow = true;
        } else if (!strncmp(argv[i], "--", 2) && (def = codec_find(&argv[i][2])) &&
                   !def->input) {
            if (d->count == DECODERS_MAX) {
                fprintf(stderr, "%s: at most %d decoders\n", who, DECODERS_MAX);
                return -EINVAL;
            }
            d->defs[d->count] = def;
            d->args[d->count++] = i + 1 < argc && argv[i + 1][0] != '-' ? argv[++i] : "";
        } else if (!strcmp(argv[i], "--chip") && i + 1 < argc) {
            if (d->count == DECODERS_MAX) {
                fprintf(stderr, "%s: at most %d decoders\n", who, DECODERS_MAX);
                return -EINVAL;
            }
            d->defs[d->count] = chip_parse(argv[++i], d->chip_args[d->count],
                                           sizeof(d->chip_args[d->count]));
            if (!d->defs[d->count]) {
                fprintf(stderr, "%s: --chip %s: give NAME@ADDR, NAME one of the chips listed "
                        "in bus-replay decode --help\n", who, argv[i]);
                return -EINVAL;
            }
            d->args[d->count] = d->chip_args[d->count];
            d->count++;
        } else if (!live && !d->path && argv[i][0] != '-') {
            d->path = argv[i];
        } else {
            d->bad = argv[i];
            return -EINVAL;
        }
    }

    // Pretty is a way of writing text, one line per reading, so it has no levels.
    if (d->pretty && strcmp(d->format, "text")) {
        fprintf(stderr, "%s: --pretty is a text layout; it does not go with -f %s\n", who,
                d->format);
        return -EINVAL;
    }
    if (d->pretty && d->level) {
        fprintf(stderr, "%s: --pretty writes one line per reading; it takes no --level\n",
                who);
        return -EINVAL;
    }

    return 0;
}


/*
 * Starts the session for a capture with info: the decoders, the span, the output (stdout).
 * Records before this are not decoded. -ENOMEM, or -EINVAL with a message given.
 */
int decoding_start(struct decoding *d, const struct capture_info *info, const char *who)
{
    char error[CODEC_ERROR_LEN];
    int err = 0;

    d->out = stdout;
    d->to = UINT64_MAX;
    d->opt.hex = d->hex;
    d->opt.pretty = d->pretty;
    d->opt.rate = info->rate;
    d->opt.level = d->level ? (unsigned int)strtoul(d->level, NULL, 10) :
                   !strcmp(d->format, "jsonl") ? 255 : 0;

    err = codec_new(&d->codec, info, info->rate);
    for (int stacked = 0; stacked < 2; stacked++) {
        for (size_t i = 0; i < d->count && err == 0; i++) {
            if (!d->defs[i]->input != !stacked) {
                continue;
            }
            err = codec_add(d->codec, d->defs[i], d->args[i], NULL, NULL, error, sizeof(error));
            if (err < 0) {
                fprintf(stderr, "%s: %s\n", who, error);
            }
        }
    }
    if (err == 0 && ((d->from_arg && parse_time(d->from_arg, info->rate, &d->from) < 0) ||
                     (d->to_arg && parse_time(d->to_arg, info->rate, &d->to) < 0))) {
        fprintf(stderr, "%s: --from and --to take seconds\n", who);
        err = -EINVAL;
    }
    if (err == 0 && strcmp(d->format, "text") && strcmp(d->format, "jsonl")) {
        for (size_t i = 0; i < codec_count(d->codec) && !d->exported; i++) {
            d->exported = find_export(codec_dec_at(d->codec, i)->def, d->format);
        }
        if (!d->exported) {
            fprintf(stderr, "%s: no format or decoder export \"%s\"\n", who, d->format);
            err = -EINVAL;
        }
    }
    if (err < 0) {
        return err;
    }

    /*
     * Only decoders that read records give top-level items, and so a label in the column.
     * Pretty also starts the readings of stacked decoders there (a chip's name), so the
     * column fits their labels too.
     */
    for (size_t i = 0; i < codec_count(d->codec); i++) {
        const struct codec_dec *dec = codec_dec_at(d->codec, i);
        size_t n = dec->def->input && !d->pretty ? 0 : strlen(dec->name);

        d->opt.name_width = n > d->opt.name_width ? n : d->opt.name_width;
    }

    return 0;
}


// The handler's record, heartbeat and loss go to the session; its event is left as it is.
void decoding_handler(struct decoding *d, struct capture_handler *handler)
{
    handler->record = on_record;
    handler->heartbeat = on_heartbeat;
    handler->loss = on_loss;
    handler->ctx = d;
}


// Writes every item that can be released.
void decoding_drain(struct decoding *d)
{
    struct codec_item *item = NULL;

    while ((item = codec_next(d->codec))) {
        if (d->err == 0 && item->end >= d->from && item->start <= d->to) {
            d->err = write_item(d, item);
        }
        codec_item_free(item);
    }
}


// Ends the input: writes the items still held, and ends the exports. Returns the first error.
int decoding_finish(struct decoding *d)
{
    codec_finish(d->codec);
    decoding_drain(d);
    for (size_t i = 0; i < codec_count(d->codec) && d->exported; i++) {
        const struct codec_export *ex = find_export(codec_dec_at(d->codec, i)->def, d->format);

        if (ex && ex->finish) {
            ex->finish(d->out);
        }
    }
    if (fflush(d->out) != 0 && d->err == 0) {
        d->err = -EIO;
    }

    return d->err ? d->err : codec_error(d->codec);
}


void decoding_free(struct decoding *d)
{
    codec_free(d->codec);
    d->codec = NULL;
}


static void on_record(void *ctx, const struct record *r)
{
    struct decoding *d = ctx;
    int err = d->codec ? codec_record(d->codec, r) : 0;

    if (err < 0 && !d->err) {
        d->err = err;
    }
}


static void on_heartbeat(void *ctx, const struct heartbeat *hb)
{
    struct decoding *d = ctx;
    int err = d->codec ? codec_heartbeat(d->codec, hb) : 0;

    if (err < 0 && !d->err) {
        d->err = err;
    }
    if (hb->tick > d->tick) {
        d->tick = hb->tick;
    }
}


static void on_loss(void *ctx, const struct loss *loss)
{
    struct decoding *d = ctx;

    if (d->codec) {
        codec_loss(d->codec, loss);
    }
}


static int write_item(struct decoding *d, const struct codec_item *item)
{
    const struct codec_export *ex = NULL;

    if (!strcmp(d->format, "text")) {
        return output_text(d->out, item, &d->opt);
    }
    if (!strcmp(d->format, "jsonl")) {
        return output_jsonl(d->out, item, &d->opt);
    }
    ex = find_export(item->dec->def, d->format);

    return ex ? ex->write(d->out, item) : 0;
}


static const struct codec_export *find_export(const struct codec_decoder *def,
                                              const char *name)
{
    for (unsigned int i = 0; def->exports && def->exports[i].name; i++) {
        if (!strcmp(def->exports[i].name, name)) {
            return &def->exports[i];
        }
    }

    return NULL;
}


/*
 * A chip-level decoder given as NAME@ADDR[,key=value...]: the decoder, with its arguments as
 * addr=ADDR[,key=value...] in args. NULL if NAME is not one, or there is no @.
 */
static const struct codec_decoder *chip_parse(const char *spec, char *args, size_t size)
{
    const char *at = strchr(spec, '@');
    const struct codec_decoder *def = NULL;
    char name[32];

    if (!at || at == spec || (size_t)(at - spec) >= sizeof(name)) {
        return NULL;
    }
    memcpy(name, spec, (size_t)(at - spec));
    name[at - spec] = '\0';
    def = codec_find(name);
    if (!def || !def->input) {
        return NULL;
    }
    snprintf(args, size, "addr=%s", at + 1);

    return def;
}


// Parses seconds into the capture as a tick.
static int parse_time(const char *s, uint32_t rate, uint64_t *tick)
{
    char *end = NULL;
    double v = strtod(s, &end);

    if (!*s || *end || v < 0) {
        return -EINVAL;
    }
    *tick = (uint64_t)(v * rate);

    return 0;
}
