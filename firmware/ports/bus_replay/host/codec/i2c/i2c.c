#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codec/codec.h"
#include "codec/i2c/i2c.h"
#include "codec/item.h"
#include "common/record.h"


/*
 * The edges of one tick are taken together (group), in the order valid I2C implies: SCL
 * falls, then SDA changes, then SCL rises. Sampling turns faster edges into edges of the
 * same tick, and in pin order they would read as false STARTs and STOPs.
 *
 * Levels are -1 until known. cur is the open transaction, seg its open segment.
 */
struct i2c {
    uint16_t scl;
    uint16_t sda;
    int8_t lscl;
    int8_t lsda;

    bool group;
    uint64_t gtick;
    int8_t gscl;
    int8_t gsda;

    struct codec_item *cur;
    struct codec_item *seg;
    uint32_t byte;
    unsigned int nbits;
    uint64_t byte_start;
    uint64_t last;
};


static int create(struct codec_dec *d, const struct codec_args *args, char *error, size_t size);
static void destroy(struct codec_dec *d);
static void record(struct codec_dec *d, const struct record *r);
static void level(struct codec_dec *d, uint64_t tick, uint16_t channel, uint8_t lvl);
static void advance(struct codec_dec *d, uint64_t tick);
static void loss(struct codec_dec *d, uint64_t start, uint64_t end);
static void finish(struct codec_dec *d, uint64_t tick);
static void group_take(struct codec_dec *d);
static void sda_change(struct codec_dec *d, uint64_t t, int8_t l);
static void scl_change(struct codec_dec *d, uint64_t t, int8_t l);
static void start(struct codec_dec *d, uint64_t t);
static void stop(struct codec_dec *d, uint64_t t);
static void bit(struct codec_dec *d, uint64_t t, int b);
static void segment_open(struct codec_dec *d, uint64_t t);
static void segment_close(struct codec_item *seg, uint64_t t);
static void close_transaction(struct codec_dec *d);


static const struct codec_field_desc transaction_fields[] = {
    {.name = "addr", .type = CODEC_UINT, .flags = CODEC_FIELD_HEADER | CODEC_FIELD_HEX,
     .digits = 2, .width = 4},
    {.name = "dir", .type = CODEC_TEXT, .flags = CODEC_FIELD_HEADER, .width = 3},
    {.name = "len", .type = CODEC_UINT, .flags = CODEC_FIELD_HEADER, .width = 3},
    {.name = "status", .type = CODEC_TEXT, .flags = CODEC_FIELD_HEADER},
};

static const struct codec_field_desc segment_fields[] = {
    {.name = "addr", .type = CODEC_UINT, .flags = CODEC_FIELD_HEADER | CODEC_FIELD_HEX,
     .digits = 2, .width = 4},
    {.name = "dir", .type = CODEC_TEXT, .flags = CODEC_FIELD_HEADER, .width = 1},
    {.name = "len", .type = CODEC_UINT, .flags = CODEC_FIELD_HEADER},
};

static const char *const lanes[] = {"master", "target"};
static const char *const transaction_flags[] = {"nak_addr", "nak_data", "cut_byte", NULL};
static const char *const word_flags[] = {"nak", NULL};

const struct codec_kind i2c_transaction = {
    .name = "transaction",
    .fields = transaction_fields,
    .field_count = sizeof(transaction_fields) / sizeof(transaction_fields[0]),
    .lanes = lanes,
    .lane_count = 2,
    .lane_mode = CODEC_LANES_TURNS,
    .item_flags = transaction_flags,
    .word_flags = word_flags,
};

const struct codec_kind i2c_segment = {
    .name = "segment",
    .fields = segment_fields,
    .field_count = sizeof(segment_fields) / sizeof(segment_fields[0]),
};

static const struct codec_param params[] = {
    {"scl", "channel of the clock line (required)"},
    {"sda", "channel of the data line (required)"},
    {NULL, NULL},
};

const struct codec_decoder i2c_decoder = {
    .name = "i2c",
    .summary = "I2C transactions: address, direction, bytes and ACKs",
    .params = params,
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
    struct i2c *s = NULL;
    int scl = -1;
    int sda = -1;
    int err = codec_arg_channel(d, args, "scl", true, &scl, error, size);

    err = err ? err : codec_arg_channel(d, args, "sda", true, &sda, error, size);
    if (err == 0 && scl == sda) {
        snprintf(error, size, "i2c: scl and sda must be different channels");
        err = -EINVAL;
    }
    if (err < 0) {
        return err;
    }
    s = calloc(1, sizeof(*s));
    if (!s) {
        return -ENOMEM;
    }
    s->scl = (uint16_t)scl;
    s->sda = (uint16_t)sda;
    s->lscl = s->lsda = -1;
    d->state = s;
    err = codec_dec_line(d, s->scl);

    return err ? err : codec_dec_line(d, s->sda);
}


static void destroy(struct codec_dec *d)
{
    struct i2c *s = d->state;

    codec_item_free(s->cur);
    free(s);
}


static void record(struct codec_dec *d, const struct record *r)
{
    struct i2c *s = d->state;

    if (s->group && r->tick != s->gtick) {
        group_take(d);
    }
    if (!s->group) {
        s->group = true;
        s->gtick = r->tick;
        s->gscl = s->gsda = -1;
    }
    if (r->channel == s->scl) {
        s->gscl = (int8_t)r->level;
    } else {
        s->gsda = (int8_t)r->level;
    }
}


// A heartbeat's level: it sets a level not known yet, and corrects one after a loss.
static void level(struct codec_dec *d, uint64_t tick, uint16_t channel, uint8_t lvl)
{
    struct i2c *s = d->state;
    int8_t *l = channel == s->scl ? &s->lscl : &s->lsda;

    if (s->group && s->gtick <= tick) {
        group_take(d);
    }
    if (*l >= 0 && *l != (int8_t)lvl && s->cur) {
        s->cur->flags |= CODEC_ITEM_LOSS;
    }
    *l = (int8_t)lvl;
}


static void advance(struct codec_dec *d, uint64_t tick)
{
    struct i2c *s = d->state;

    if (s->group && s->gtick <= tick) {
        group_take(d);
    }
    if (s->cur) {
        codec_horizon(d, s->cur->start);
    }
}


static void loss(struct codec_dec *d, uint64_t start, uint64_t end)
{
    struct i2c *s = d->state;

    (void)start;
    if (s->cur && s->cur->start <= end) {
        s->cur->flags |= CODEC_ITEM_LOSS;
    }
}


// The end of the input: a transaction still open never saw its STOP.
static void finish(struct codec_dec *d, uint64_t tick)
{
    struct i2c *s = d->state;

    (void)tick;
    if (s->group) {
        group_take(d);
    }
    if (s->cur) {
        s->cur->flags |= CODEC_ITEM_INCOMPLETE;
        s->cur->end = s->last;
        segment_close(s->seg, s->last);
        close_transaction(d);
    }
}


static void group_take(struct codec_dec *d)
{
    struct i2c *s = d->state;
    uint64_t t = s->gtick;

    s->group = false;
    s->last = t;
    // A line not known yet was at the other level before its first record.
    if (s->lscl < 0 && s->gscl >= 0) {
        s->lscl = (int8_t)!s->gscl;
    }
    if (s->lsda < 0 && s->gsda >= 0) {
        s->lsda = (int8_t)!s->gsda;
    }
    if (s->gscl == 0) {
        scl_change(d, t, 0);
    }
    if (s->gsda >= 0) {
        sda_change(d, t, s->gsda);
    }
    if (s->gscl == 1) {
        scl_change(d, t, 1);
    }
}


// SDA falling with SCL high is a START, rising a STOP.
static void sda_change(struct codec_dec *d, uint64_t t, int8_t l)
{
    struct i2c *s = d->state;

    if (l != s->lsda && s->lscl == 1) {
        if (l == 0) {
            start(d, t);
        } else {
            stop(d, t);
        }
    }
    s->lsda = l;
}


// Bits are read as SCL rises.
static void scl_change(struct codec_dec *d, uint64_t t, int8_t l)
{
    struct i2c *s = d->state;

    if (l != s->lscl) {
        s->lscl = l;
        if (l == 1 && s->cur && s->lsda >= 0) {
            bit(d, t, s->lsda);
        }
    }
}


/*
 * SCL rises once before a repeated START or a STOP, so one bit read since the last byte is
 * normal there; more is a cut byte.
 */
static void start(struct codec_dec *d, uint64_t t)
{
    struct i2c *s = d->state;

    if (s->cur) {
        if (s->nbits > 1) {
            s->cur->flags |= I2C_CUT_BYTE;
        }
        segment_close(s->seg, t);
    } else {
        s->cur = codec_item_new(d, &i2c_transaction, t);
        if (!s->cur) {
            codec_failed(d, -ENOMEM);
            return;
        }
    }
    segment_open(d, t);
    s->byte = 0;
    s->nbits = 0;
}


static void stop(struct codec_dec *d, uint64_t t)
{
    struct i2c *s = d->state;

    if (!s->cur) {
        return;
    }
    if (s->nbits > 1) {
        s->cur->flags |= I2C_CUT_BYTE;
    }
    s->cur->end = t;
    segment_close(s->seg, t);
    close_transaction(d);
}


// Eight data bits, MSB first, then the ACK bit (0: ACK).
static void bit(struct codec_dec *d, uint64_t t, int b)
{
    struct i2c *s = d->state;
    struct i2c_segment *sp = s->seg ? s->seg->payload : NULL;
    struct codec_word w = {.bits = 8, .lane = I2C_MASTER};

    s->cur->end = t;
    if (s->nbits == 0) {
        s->byte_start = t;
    }
    if (s->nbits < 8) {
        s->byte = (s->byte << 1) | (uint32_t)b;
        s->nbits++;
        return;
    }

    w.start = s->byte_start;
    w.end = t;
    w.value = s->byte;
    w.flags = b ? I2C_WORD_NAK : 0;
    if (sp && sp->count == 0) {
        w.flags |= CODEC_WORD_BREAK;
        sp->has_addr = true;
        sp->addr = (uint8_t)(s->byte >> 1);
        sp->read = s->byte & 1;
    } else if (sp && sp->read) {
        w.lane = I2C_TARGET;
    }
    if (codec_word_add(s->cur, &w) < 0) {
        codec_failed(d, -ENOMEM);
    } else if (sp) {
        sp->count++;
        s->seg->end = t;
    }
    s->byte = 0;
    s->nbits = 0;
}


static void segment_open(struct codec_dec *d, uint64_t t)
{
    struct i2c *s = d->state;
    struct i2c_segment *sp = calloc(1, sizeof(*sp));

    s->seg = sp ? codec_item_child(s->cur, d, &i2c_segment, t) : NULL;
    if (!s->seg) {
        free(sp);
        codec_failed(d, -ENOMEM);
        return;
    }
    s->seg->detail = 1;
    sp->first = s->cur->word_count;
    s->seg->payload = sp;
    s->seg->payload_free = free;
}


static void segment_close(struct codec_item *seg, uint64_t t)
{
    if (seg && seg->end < t) {
        seg->end = t;
    }
}


/*
 * Fills in the transaction's and its segments' fields and emits it. A NAK on the last byte
 * a master reads ends the read; it is not an error.
 */
static void close_transaction(struct codec_dec *d)
{
    struct i2c *s = d->state;
    struct codec_item *t = s->cur;
    char dir[64] = "";
    size_t dlen = 0;
    uint64_t len = 0;
    bool have_addr = false;

    for (struct codec_item *seg = t->child; seg; seg = seg->next) {
        const struct i2c_segment *sp = seg->payload;
        size_t data = sp->count > 0 ? sp->count - 1 : 0;

        if (!sp->has_addr) {
            continue;
        }
        if (!have_addr) {
            codec_set_uint(t, I2C_ADDR, sp->addr);
            have_addr = true;
        }
        codec_set_uint(seg, I2C_SEG_ADDR, sp->addr);
        codec_set_text(seg, I2C_SEG_DIR, sp->read ? "R" : "W");
        codec_set_uint(seg, I2C_SEG_LEN, data);
        if (dlen + 3 < sizeof(dir)) {
            dlen += (size_t)snprintf(&dir[dlen], sizeof(dir) - dlen, "%s%s", dlen ? "+" : "",
                                     sp->read ? "R" : "W");
        }
        len += data;

        if (t->words[sp->first].flags & I2C_WORD_NAK) {
            t->flags |= I2C_NAK_ADDR;
        }
        for (size_t i = sp->first + 1; i < sp->first + sp->count; i++) {
            bool last = i == sp->first + sp->count - 1;

            if ((t->words[i].flags & I2C_WORD_NAK) && (!sp->read || !last)) {
                t->flags |= I2C_NAK_DATA;
            }
        }
    }
    if (dlen) {
        codec_set_text(t, I2C_DIR, dir);
    }
    codec_set_uint(t, I2C_LEN, len);
    codec_set_text(t, I2C_STATUS, t->flags & I2C_CUT_BYTE   ? "ERR" :
                                  t->flags & I2C_NAK_ADDR   ? "NAK addr" :
                                  t->flags & I2C_NAK_DATA   ? "NAK" :
                                  !have_addr                ? "empty" :
                                                              "ACK");
    s->cur = NULL;
    s->seg = NULL;
    codec_emit(d, t);
}
