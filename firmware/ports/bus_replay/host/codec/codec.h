/*
 * libcodec: protocol decoders over a capture's raw records, and a session that runs several
 * of them over one capture.
 *
 * A session is fed what a capture session hands over: records, heartbeats and losses, in
 * any order the data channels arrive in. It merges them into tick order (codec/merge.h),
 * gives each decoder the records of the lines it reads, and collects the items the decoders
 * emit. Items come out of codec_next() in order of their start, across decoders, once no
 * decoder can still emit an earlier one and a late loss report could no longer mark them.
 *
 * Input that has no heartbeats, such as records read back from a store, gives what a
 * heartbeat would instead: codec_line_level() a channel's level at the end of a tick, and
 * codec_advance() that every record up to and including a tick has been given, on every
 * data channel. The records must then come in tick order.
 *
 * A decoder reads records, or is stacked on another decoder and reads its items: it sees
 * each of them before it is released, and may add children to it (a chip-level reading of
 * an I2C transaction) or emit items of its own.
 */

#ifndef CODEC_CODEC_H
#define CODEC_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "codec/item.h"
#include "common/capture_info.h"
#include "common/record.h"


#define CODEC_ERROR_LEN 128

// A decoder's arguments: key=value pairs, as given after its option ("scl=0,sda=1").
struct codec_args {
    size_t count;
    char **key;
    char **value;
};

// What a decoder argument takes: a channel (a line it reads), a number, or text.
#define CODEC_PARAM_TEXT 0
#define CODEC_PARAM_LINE 1
#define CODEC_PARAM_UINT 2
#define CODEC_PARAM_REAL 3

// Argument flags: the decoder can't run without it.
#define CODEC_PARAM_REQUIRED 0x01

/*
 * A decoder argument, as UIs and help list it. def is the value the decoder takes when it
 * is left out, as text (NULL: none, or no fixed one).
 */
struct codec_param {
    const char *name;
    const char *help;
    uint8_t type;
    uint8_t flags;
    const char *def;
};

/*
 * A payload export a decoder offers beside the common outputs (the bytes of a UART, the
 * samples of an I2S as WAV). write is called for each released top-level item of the
 * decoder, in order; finish once at the end, for a trailer.
 */
struct codec_export {
    const char *name;
    const char *help;
    int (*write)(FILE *out, const struct codec_item *item);
    int (*finish)(FILE *out);
};

/*
 * A protocol decoder. input is NULL for a decoder that reads records, or the name of the
 * decoder it is stacked on. params lists its arguments (NULL-terminated); every decoder also
 * takes name=, its label in the outputs, and a stacked one on=, the label of the decoder it
 * goes on. exports is NULL-terminated, or NULL. data is the decoder's own constant data,
 * for decoders that share their code (the chip-level ones, codec/chips/chip.h).
 *
 *  - create: reads the arguments, sets up d->state and the lines it reads
 *    (codec_dec_line()). error gets a message on failure.
 *  - record: a record of one of its lines, in tick order.
 *  - level: a heartbeat's level for one of its lines at the end of tick, after the records
 *    of that tick. It disagrees with the records only after records were lost.
 *  - advance: every record up to and including tick has been given. The decoder emits what
 *    it can and sets its horizon (codec_horizon()).
 *  - item: an item of the decoder it is stacked on, before it is released.
 *  - loss: records of its lines may be missing from loss->start to loss->end.
 *  - finish: the end of the input at tick: emit what is still open.
 */
struct codec_decoder {
    const char *name;
    const char *summary;
    const char *input;
    const struct codec_param *params;
    const struct codec_export *exports;
    const void *data;

    int (*create)(struct codec_dec *d, const struct codec_args *args, char *error, size_t size);
    void (*destroy)(struct codec_dec *d);
    void (*record)(struct codec_dec *d, const struct record *r);
    void (*level)(struct codec_dec *d, uint64_t tick, uint16_t channel, uint8_t level);
    void (*advance)(struct codec_dec *d, uint64_t tick);
    void (*item)(struct codec_dec *d, struct codec_item *item);
    void (*loss)(struct codec_dec *d, uint64_t start, uint64_t end);
    void (*finish)(struct codec_dec *d, uint64_t tick);
};

struct codec;

/*
 * A decoder in a session. lines are the channels it reads; lower is the decoder it is
 * stacked on. horizon: it will not emit a top-level item starting before this tick.
 */
struct codec_dec {
    struct codec *codec;
    const struct codec_decoder *def;
    char *name;
    void *state;
    uint16_t *lines;
    size_t line_count;
    struct codec_dec *lower;
    uint64_t horizon;
};


extern const struct codec_decoder *const codec_decoders[];

const struct codec_decoder *codec_find(const char *name);

int codec_new(struct codec **out, const struct capture_info *info, uint64_t hold);
void codec_free(struct codec *c);
int codec_add(struct codec *c, const struct codec_decoder *def, const char *args,
              struct codec_dec *lower, struct codec_dec **out, char *error, size_t size);
size_t codec_count(const struct codec *c);
struct codec_dec *codec_dec_at(const struct codec *c, size_t i);

int codec_record(struct codec *c, const struct record *r);
int codec_heartbeat(struct codec *c, const struct heartbeat *hb);
int codec_line_level(struct codec *c, uint64_t tick, uint16_t channel, uint8_t level);
int codec_advance(struct codec *c, uint64_t tick);
void codec_loss(struct codec *c, const struct loss *loss);
void codec_finish(struct codec *c);
struct codec_item *codec_next(struct codec *c);
int codec_error(const struct codec *c);

// For decoders.
const struct capture_info *codec_info(const struct codec_dec *d);
int codec_dec_line(struct codec_dec *d, uint16_t channel);
int codec_level(const struct codec_dec *d, uint16_t channel);
void codec_emit(struct codec_dec *d, struct codec_item *item);
void codec_horizon(struct codec_dec *d, uint64_t tick);
void codec_failed(struct codec_dec *d, int err);

const char *codec_arg(const struct codec_args *args, const char *key);
int codec_arg_channel(const struct codec_dec *d, const struct codec_args *args,
                      const char *key, bool required, int *channel, char *error, size_t size);
int codec_arg_uint(const struct codec_args *args, const char *key, uint64_t def,
                   uint64_t *v, char *error, size_t size);
int codec_arg_real(const struct codec_args *args, const char *key, double def, double *v,
                   char *error, size_t size);

#endif
