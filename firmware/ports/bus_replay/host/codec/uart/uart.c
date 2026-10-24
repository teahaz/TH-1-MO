#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codec/codec.h"
#include "codec/item.h"
#include "codec/uart/uart.h"
#include "common/capture_info.h"
#include "common/record.h"


/*
 * One line. Its records wait in edges until a whole frame after them has been handed over:
 * a byte is read at the middle of each bit time, so it needs the records up to its stop
 * bit. base is the level before edges[head] (-1 until known); busy is the tick up to which
 * the line is inside a frame read already.
 */
struct line {
    bool used;
    uint16_t ch;
    uint8_t lane;
    int8_t base;
    double busy;

    struct record *edges;
    size_t head;
    size_t len;
    size_t cap;
};

// A byte read, before it goes into a burst.
struct byte {
    uint64_t start;
    uint64_t end;
    uint16_t value;
    uint8_t lane;
    bool error;
};

/*
 * bit is a bit time in ticks. burst is the open burst; last its last byte's end, errors its
 * framing errors.
 */
struct uart {
    struct line line[2];
    unsigned int bits;
    double bit;
    double gap;

    struct codec_item *burst;
    uint64_t last;
    uint64_t errors;

    struct byte *read;
    size_t read_len;
    size_t read_cap;
};


static int create(struct codec_dec *d, const struct codec_args *args, char *error, size_t size);
static void destroy(struct codec_dec *d);
static void record(struct codec_dec *d, const struct record *r);
static void level(struct codec_dec *d, uint64_t tick, uint16_t channel, uint8_t lvl);
static void advance(struct codec_dec *d, uint64_t tick);
static void loss(struct codec_dec *d, uint64_t start, uint64_t end);
static void finish(struct codec_dec *d, uint64_t tick);
static void frames(struct codec_dec *d, struct line *l, double upto);
static int level_at(const struct line *l, double t);
static void pop(struct line *l);
static int push_byte(struct uart *s, const struct byte *b);
static int byte_cmp(const void *a, const void *b);
static void take(struct codec_dec *d, const struct byte *b);
static void burst_close(struct codec_dec *d);
static int raw_write(FILE *out, const struct codec_item *item);


static const struct codec_field_desc burst_fields[] = {
    {.name = "dir", .type = CODEC_TEXT, .flags = CODEC_FIELD_HEADER, .width = 2},
    {.name = "bytes", .type = CODEC_UINT, .flags = CODEC_FIELD_HEADER, .unit = "bytes"},
    {.name = "errors", .type = CODEC_UINT, .flags = CODEC_FIELD_HEADER,
     .unit = "framing errors"},
};

static const char *const lanes[] = {"tx", "rx"};

const struct codec_kind uart_burst = {
    .name = "burst",
    .fields = burst_fields,
    .field_count = sizeof(burst_fields) / sizeof(burst_fields[0]),
    .lanes = lanes,
    .lane_count = 2,
    .lane_mode = CODEC_LANES_TURNS,
    .flags = CODEC_KIND_TEXT,
};

static const struct codec_param params[] = {
    {"tx", "channel of one direction, shown with > (tx, rx or both)"},
    {"rx", "channel of the other direction, shown with <"},
    {"baud", "bits per second, for example 115200 (required; at most half the sample rate)"},
    {"bits", "data bits per byte, 5 to 9 (default 8); always no parity, 1 stop bit"},
    {"gap", "idle time, in byte times, that ends a burst (default 10)"},
    {NULL, NULL},
};

static const struct codec_export exports[] = {
    {.name = "raw", .help = "the bytes alone, in order", .write = raw_write},
    {0},
};

const struct codec_decoder uart_decoder = {
    .name = "uart",
    .summary = "UART bytes in bursts, per direction, with framing errors",
    .params = params,
    .exports = exports,
    .create = create,
    .destroy = destroy,
    .record = record,
    .level = level,
    .advance = advance,
    .loss = loss,
    .finish = finish,
};


static int create(struct codec_dec *d, const struct codec_args *args, char *error, size_t size)
{
    const struct capture_info *info = codec_info(d);
    struct uart *s = NULL;
    int tx = -1;
    int rx = -1;
    uint64_t baud = 0;
    uint64_t bits = 0;
    double gap = 0;
    int err = codec_arg_channel(d, args, "tx", false, &tx, error, size);

    err = err ? err : codec_arg_channel(d, args, "rx", false, &rx, error, size);
    err = err ? err : codec_arg_uint(args, "baud", 0, &baud, error, size);
    err = err ? err : codec_arg_uint(args, "bits", 8, &bits, error, size);
    err = err ? err : codec_arg_real(args, "gap", UART_GAP, &gap, error, size);
    if (err < 0) {
        return err;
    }
    if (tx < 0 && rx < 0) {
        snprintf(error, size, "uart: give tx=, rx= or both");
        return -EINVAL;
    }
    if (tx >= 0 && tx == rx) {
        snprintf(error, size, "uart: tx and rx must be different channels");
        return -EINVAL;
    }
    if (baud == 0 || baud * 2 > info->rate) {
        snprintf(error, size, "uart: baud= is required, and at most half the sample rate (%u)",
                 info->rate);
        return -EINVAL;
    }
    if (bits < 5 || bits > 9 || gap < 0) {
        snprintf(error, size, "uart: bits= is 5 to 9, gap= at least 0");
        return -EINVAL;
    }

    s = calloc(1, sizeof(*s));
    if (!s) {
        return -ENOMEM;
    }
    d->state = s;
    s->bits = (unsigned int)bits;
    s->bit = (double)info->rate / (double)baud;
    s->gap = gap * (s->bits + 2) * s->bit;
    for (unsigned int i = 0; i < 2; i++) {
        int ch = i == UART_TX ? tx : rx;

        s->line[i] = (struct line){.used = ch >= 0, .ch = (uint16_t)ch, .lane = (uint8_t)i,
                                   .base = -1};
        if (ch >= 0) {
            err = codec_dec_line(d, (uint16_t)ch);
            if (err < 0) {
                return err;
            }
        }
    }

    return 0;
}


static void destroy(struct codec_dec *d)
{
    struct uart *s = d->state;

    for (unsigned int i = 0; i < 2; i++) {
        free(s->line[i].edges);
    }
    codec_item_free(s->burst);
    free(s->read);
    free(s);
}


static void record(struct codec_dec *d, const struct record *r)
{
    struct uart *s = d->state;
    struct line *l = &s->line[s->line[UART_TX].used && s->line[UART_TX].ch == r->channel ?
                                  UART_TX : UART_RX];

    if (l->base < 0 && !l->len) {
        l->base = (int8_t)!r->level;
    }
    if (l->head + l->len == l->cap) {
        if (l->head > 0) {
            memmove(l->edges, &l->edges[l->head], l->len * sizeof(*l->edges));
            l->head = 0;
        } else {
            size_t cap = l->cap ? 2 * l->cap : 256;
            struct record *bigger = realloc(l->edges, cap * sizeof(*bigger));

            if (!bigger) {
                codec_failed(d, -ENOMEM);
                return;
            }
            l->edges = bigger;
            l->cap = cap;
        }
    }
    l->edges[l->head + l->len++] = *r;
}


/*
 * A heartbeat's level. A line not known yet takes it; a line whose records say otherwise
 * lost records: what waits on it is dropped, and the open burst marked.
 */
static void level(struct codec_dec *d, uint64_t tick, uint16_t channel, uint8_t lvl)
{
    struct uart *s = d->state;
    struct line *l = &s->line[s->line[UART_TX].used && s->line[UART_TX].ch == channel ?
                                  UART_TX : UART_RX];
    int now = level_at(l, (double)tick);

    if (now < 0) {
        l->base = (int8_t)lvl;
        return;
    }
    if (now != lvl) {
        if (s->burst && s->burst->words[0].lane == l->lane) {
            s->burst->flags |= CODEC_ITEM_LOSS;
        }
        while (l->len && l->edges[l->head].tick <= tick) {
            pop(l);
        }
        l->base = (int8_t)lvl;
        l->busy = (double)tick;
    }
}


/*
 * Reads every frame that ends by tick on both lines, takes the bytes in order of their
 * start, and closes the burst once the line has been idle for the gap.
 */
static void advance(struct codec_dec *d, uint64_t tick)
{
    struct uart *s = d->state;

    s->read_len = 0;
    for (unsigned int i = 0; i < 2; i++) {
        if (s->line[i].used) {
            frames(d, &s->line[i], (double)tick);
        }
    }
    qsort(s->read, s->read_len, sizeof(*s->read), byte_cmp);
    for (size_t i = 0; i < s->read_len; i++) {
        take(d, &s->read[i]);
    }
    if (s->burst && (double)tick > (double)s->last + s->gap) {
        burst_close(d);
    }

    if (s->burst) {
        codec_horizon(d, s->burst->start);
    }
    for (unsigned int i = 0; i < 2; i++) {
        const struct line *l = &s->line[i];

        // A frame waiting for its end starts a burst no later than its start bit.
        for (size_t k = 0; k < l->len; k++) {
            const struct record *e = &l->edges[l->head + k];

            if (e->level == 0 && (double)e->tick >= l->busy) {
                codec_horizon(d, e->tick);
                break;
            }
        }
    }
}


static void loss(struct codec_dec *d, uint64_t start, uint64_t end)
{
    struct uart *s = d->state;

    (void)start;
    if (s->burst && s->burst->start <= end) {
        s->burst->flags |= CODEC_ITEM_LOSS;
    }
}


// The end of the input: a frame not finished by then is not read.
static void finish(struct codec_dec *d, uint64_t tick)
{
    struct uart *s = d->state;

    advance(d, tick);
    if (s->burst) {
        burst_close(d);
    }
}


// Reads the frames on l that end by upto into s->read.
static void frames(struct codec_dec *d, struct line *l, double upto)
{
    struct uart *s = d->state;
    double last_sample = (s->bits + 1.5) * s->bit;

    while (l->len) {
        const struct record *e = &l->edges[l->head];
        struct byte b = {.lane = l->lane};
        double t = (double)e->tick;

        // A rising edge, or an edge inside a frame read already.
        if (t < l->busy || e->level != 0) {
            pop(l);
            continue;
        }
        if (t + last_sample > upto) {
            break;
        }

        b.start = e->tick;
        b.end = (uint64_t)(t + last_sample);
        for (unsigned int k = 0; k < s->bits; k++) {
            if (level_at(l, t + (1.5 + k) * s->bit) == 1) {
                b.value |= (uint16_t)(1U << k);
            }
        }
        b.error = level_at(l, t + 0.5 * s->bit) != 0 || level_at(l, t + last_sample) != 1;
        l->busy = t + last_sample;
        pop(l);
        if (push_byte(s, &b) < 0) {
            codec_failed(d, -ENOMEM);
            return;
        }
    }
}


// The level at tick t, by the records waiting on the line; -1 if not known.
static int level_at(const struct line *l, double t)
{
    int lvl = l->base;

    for (size_t k = 0; k < l->len; k++) {
        const struct record *e = &l->edges[l->head + k];

        if ((double)e->tick > t) {
            break;
        }
        lvl = e->level;
    }

    return lvl;
}


static void pop(struct line *l)
{
    l->base = (int8_t)l->edges[l->head].level;
    l->head++;
    l->len--;
    if (!l->len) {
        l->head = 0;
    }
}


static int push_byte(struct uart *s, const struct byte *b)
{
    if (s->read_len == s->read_cap) {
        size_t cap = s->read_cap ? 2 * s->read_cap : 64;
        struct byte *bigger = realloc(s->read, cap * sizeof(*bigger));

        if (!bigger) {
            return -ENOMEM;
        }
        s->read = bigger;
        s->read_cap = cap;
    }
    s->read[s->read_len++] = *b;

    return 0;
}


static int byte_cmp(const void *a, const void *b)
{
    const struct byte *x = a;
    const struct byte *y = b;

    if (x->start != y->start) {
        return x->start < y->start ? -1 : 1;
    }

    return (int)x->lane - (int)y->lane;
}


// A byte into the open burst, or a new one: the other direction or a gap closes it.
static void take(struct codec_dec *d, const struct byte *b)
{
    struct uart *s = d->state;
    struct codec_word w = {.start = b->start, .end = b->end, .value = b->value,
                           .bits = (uint8_t)s->bits, .lane = b->lane,
                           .flags = b->error ? CODEC_WORD_ERROR : 0};

    if (s->burst && (s->burst->words[0].lane != b->lane ||
                     (double)b->start > (double)s->last + s->gap)) {
        burst_close(d);
    }
    if (!s->burst) {
        s->burst = codec_item_new(d, &uart_burst, b->start);
        if (!s->burst) {
            codec_failed(d, -ENOMEM);
            return;
        }
        s->errors = 0;
    }
    if (codec_word_add(s->burst, &w) < 0) {
        codec_failed(d, -ENOMEM);
        return;
    }
    s->burst->end = b->end;
    s->last = b->end;
    s->errors += b->error;
}


static void burst_close(struct codec_dec *d)
{
    struct uart *s = d->state;
    struct codec_item *b = s->burst;

    codec_set_text(b, UART_DIR, b->words[0].lane == UART_TX ? "TX" : "RX");
    codec_set_uint(b, UART_BYTES, b->word_count);
    if (s->errors) {
        codec_set_uint(b, UART_ERRORS, s->errors);
    }
    s->burst = NULL;
    codec_emit(d, b);
}


// The raw export: each byte as one byte (9-bit words as two, low byte first).
static int raw_write(FILE *out, const struct codec_item *item)
{
    for (size_t i = 0; i < item->word_count; i++) {
        const struct codec_word *w = &item->words[i];

        if (fputc((int)(w->value & 0xFF), out) == EOF) {
            return -EIO;
        }
        if (w->bits > 8 && fputc((int)(w->value >> 8), out) == EOF) {
            return -EIO;
        }
    }

    return 0;
}
