#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codec/chips/chip.h"
#include "codec/codec.h"
#include "codec/i2c/i2c.h"
#include "codec/item.h"


#define WORDS_MAX 32
#define DATA_TEXT_MAX 128

/*
 * A chip decoder's state: the address it reads, the register pointer and the last command
 * (-1 when not known), and the chip's own state.
 */
struct chip_dec {
    const struct chip *chip;
    uint8_t addr;
    int pointer;
    int cmd;
    void *own;
};

// One segment of a transaction, as a chip reads it: its data bytes, after the address.
struct seg {
    const struct codec_item *item;
    const struct i2c_segment *sp;
    const struct codec_word *data;
    size_t n;
};


static bool seg_get(const struct chip_dec *s, const struct codec_item *t,
                    const struct codec_item *item, struct seg *out);
static const struct codec_item *seg_from(const struct codec_item *item);
static void pointer_write(struct codec_dec *d, struct codec_item *t, const struct seg *sg);
static void register_read(struct codec_dec *d, struct codec_item *t, const struct seg *sg,
                          uint64_t start);
static void register_write(struct codec_dec *d, struct codec_item *t, const struct seg *sg);
static bool command_write(struct codec_dec *d, struct codec_item *t, const struct seg *sg,
                          bool read_follows);
static void command_read(struct codec_dec *d, struct codec_item *t, const struct seg *sg,
                         uint64_t start);
static struct codec_item *reading(struct codec_dec *d, struct codec_item *t, uint64_t start,
                                  uint64_t end, const char *op);
static const struct chip_reg *reg_find(const struct chip *chip, unsigned int reg);
static const struct chip_cmd *cmd_find(const struct chip *chip, unsigned int cmd);
static void name_set(struct codec_dec *d, struct codec_item *it, const char *name,
                     const char *fmt, unsigned int v);
static size_t words_get(struct codec_item *it, const struct codec_word *b, size_t n,
                        uint16_t *w, uint32_t *bad);
static void data_set(struct codec_dec *d, struct codec_item *it, const struct codec_word *b,
                     size_t n);
static uint64_t be_value(const struct codec_word *b, size_t n);


int chip_create(struct codec_dec *d, const struct codec_args *args, char *error, size_t size)
{
    const struct chip *chip = d->def->data;
    const char *v = codec_arg(args, "addr");
    struct chip_dec *s = NULL;
    char *end = NULL;
    unsigned long addr = 0;
    bool known = false;
    int err = 0;

    if (!v) {
        snprintf(error, size, "%s: addr= is required", d->def->name);
        return -ENOENT;
    }
    addr = strtoul(v, &end, 0);
    if (!*v || *end || addr > 0x7F) {
        snprintf(error, size, "%s: addr=%s is not a 7-bit I2C address", d->def->name, v);
        return -EINVAL;
    }
    for (size_t i = 0; i < chip->addr_count && !known; i++) {
        known = chip->addrs[i] == addr;
    }
    if (!known) {
        snprintf(error, size, "%s: a %s does not answer at 0x%02lX", d->def->name, chip->label,
                 addr);
        return -EINVAL;
    }

    s = calloc(1, sizeof(*s));
    if (!s) {
        snprintf(error, size, "out of memory");
        return -ENOMEM;
    }
    s->chip = chip;
    s->addr = (uint8_t)addr;
    s->pointer = -1;
    s->cmd = -1;
    if (chip->state_size) {
        s->own = calloc(1, chip->state_size);
        if (!s->own) {
            free(s);
            snprintf(error, size, "out of memory");
            return -ENOMEM;
        }
    }
    d->state = s;
    if (chip->create) {
        err = chip->create(s->own, args, error, size);
    }

    return err;
}


void chip_destroy(struct codec_dec *d)
{
    struct chip_dec *s = d->state;

    free(s->own);
    free(s);
}


/*
 * A transaction of the i2c decoder the chip is on: a reading of each of its segments to the
 * chip's address, in order. A pointer write or command followed by a read in the same
 * transaction is one reading, from the write to the read.
 */
void chip_item(struct codec_dec *d, struct codec_item *item)
{
    struct chip_dec *s = d->state;
    bool command = s->chip->pattern == CHIP_COMMAND;

    for (const struct codec_item *c = seg_from(item->child); c; c = seg_from(c->next)) {
        const struct codec_item *after = seg_from(c->next);
        struct seg sg;
        struct seg next;
        bool read_follows = false;

        if (!seg_get(s, item, c, &sg)) {
            continue;
        }
        read_follows = after && seg_get(s, item, after, &next) && next.sp->read;

        if (sg.sp->read) {
            if (command) {
                command_read(d, item, &sg, c->start);
            } else {
                register_read(d, item, &sg, c->start);
            }
        } else if (sg.n == 0) {
            reading(d, item, c->start, c->end, "empty");
        } else if (!command && sg.n == 1) {
            s->pointer = (int)sg.data[0].value;
            if (read_follows) {
                register_read(d, item, &next, c->start);
                c = after;
            } else {
                pointer_write(d, item, &sg);
            }
        } else if (!command) {
            register_write(d, item, &sg);
        } else if (command_write(d, item, &sg, read_follows)) {
            command_read(d, item, &next, c->start);
            c = after;
        }
    }
}


// A loss may have taken a pointer write or a command: what came before is not known.
void chip_loss(struct codec_dec *d, uint64_t start, uint64_t end)
{
    struct chip_dec *s = d->state;

    (void)start;
    (void)end;
    s->pointer = -1;
    s->cmd = -1;
}


/*
 * CRC-8 of Sensirion's sensors (and others): polynomial 0x31, initial value 0xFF, no
 * reflection, no final XOR.
 */
uint8_t chip_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;

    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = crc & 0x80 ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }

    return crc;
}


/*
 * item as a segment the chip reads: an I2C segment to its address, the address byte
 * acknowledged.
 */
static bool seg_get(const struct chip_dec *s, const struct codec_item *t,
                    const struct codec_item *item, struct seg *out)
{
    const struct i2c_segment *sp = item->kind == &i2c_segment ? item->payload : NULL;

    if (!sp || !sp->has_addr || sp->addr != s->addr ||
        (t->words[sp->first].flags & I2C_WORD_NAK)) {
        return false;
    }
    out->item = item;
    out->sp = sp;
    out->data = &t->words[sp->first + 1];
    out->n = sp->count - 1;

    return true;
}


// The first I2C segment from item on among its siblings, NULL if none.
static const struct codec_item *seg_from(const struct codec_item *item)
{
    for (; item; item = item->next) {
        if (item->kind == &i2c_segment) {
            return item;
        }
    }

    return NULL;
}


// A pointer write with no read after it in the same transaction.
static void pointer_write(struct codec_dec *d, struct codec_item *t, const struct seg *sg)
{
    struct chip_dec *s = d->state;
    const struct chip_reg *reg = reg_find(s->chip, (unsigned int)s->pointer);
    struct codec_item *it = reading(d, t, sg->item->start, sg->item->end, "pointer");

    if (it) {
        name_set(d, it, reg ? reg->name : NULL, "0x%02X", (unsigned int)s->pointer);
    }
}


// A read of the register the pointer is at, from start (its pointer write, or the read).
static void register_read(struct codec_dec *d, struct codec_item *t, const struct seg *sg,
                          uint64_t start)
{
    struct chip_dec *s = d->state;
    const struct chip *chip = s->chip;
    const struct chip_reg *reg = s->pointer >= 0 ? reg_find(chip, (unsigned int)s->pointer) :
                                                    NULL;
    struct codec_item *it = reading(d, t, start, sg->item->end, "read");

    if (!it) {
        return;
    }
    if (s->pointer < 0) {
        it->flags |= CHIP_UNKNOWN;
    } else {
        name_set(d, it, reg ? reg->name : NULL, "0x%02X", (unsigned int)s->pointer);
    }
    if (sg->n != chip->reg_bytes) {
        it->flags |= sg->n < chip->reg_bytes ? CHIP_SHORT : CHIP_LONG;
        data_set(d, it, sg->data, sg->n);
    }
    if (sg->n < chip->reg_bytes) {
        return;
    }
    codec_set_uint(it, CHIP_RAW, be_value(sg->data, chip->reg_bytes));
    if (reg && reg->decode) {
        reg->decode(s->own, it, false, be_value(sg->data, chip->reg_bytes));
    } else if (sg->n == chip->reg_bytes) {
        data_set(d, it, sg->data, sg->n);
    }
}


// A write of the pointer and a register's value.
static void register_write(struct codec_dec *d, struct codec_item *t, const struct seg *sg)
{
    struct chip_dec *s = d->state;
    const struct chip *chip = s->chip;
    const struct chip_reg *reg = reg_find(chip, (unsigned int)sg->data[0].value);
    struct codec_item *it = reading(d, t, sg->item->start, sg->item->end, "write");
    size_t n = sg->n - 1;

    s->pointer = (int)sg->data[0].value;
    if (!it) {
        return;
    }
    name_set(d, it, reg ? reg->name : NULL, "0x%02X", (unsigned int)s->pointer);
    if (n != chip->reg_bytes) {
        it->flags |= n < chip->reg_bytes ? CHIP_SHORT : CHIP_LONG;
        data_set(d, it, &sg->data[1], n);
    }
    if (n < chip->reg_bytes) {
        return;
    }
    codec_set_uint(it, CHIP_RAW, be_value(&sg->data[1], chip->reg_bytes));
    if (reg && reg->decode) {
        reg->decode(s->own, it, true, be_value(&sg->data[1], chip->reg_bytes));
    } else if (n == chip->reg_bytes) {
        data_set(d, it, &sg->data[1], n);
    }
}


/*
 * A command, with data words after it if it takes any. With a read right after it in the
 * same transaction, a command that gives words is only remembered, and true returned: the
 * read is its reading.
 */
static bool command_write(struct codec_dec *d, struct codec_item *t, const struct seg *sg,
                          bool read_follows)
{
    struct chip_dec *s = d->state;
    const struct chip_cmd *cmd = NULL;
    struct codec_item *it = NULL;
    uint16_t w[WORDS_MAX];
    uint32_t bad = 0;
    size_t nw = 0;

    if (sg->n < 2) {
        it = reading(d, t, sg->item->start, sg->item->end, "command");
        if (it) {
            it->flags |= CHIP_SHORT;
            data_set(d, it, sg->data, sg->n);
        }
        s->cmd = -1;
        return false;
    }
    s->cmd = (int)be_value(sg->data, 2);
    cmd = cmd_find(s->chip, (unsigned int)s->cmd);
    if (read_follows && sg->n == 2 && cmd && cmd->words) {
        return true;
    }

    it = reading(d, t, sg->item->start, sg->item->end, "command");
    if (!it) {
        return false;
    }
    name_set(d, it, cmd ? cmd->name : NULL, "0x%04X", (unsigned int)s->cmd);
    codec_set_uint(it, CHIP_RAW, (uint64_t)s->cmd);
    if (sg->n == 2) {
        return false;
    }
    nw = words_get(it, &sg->data[2], sg->n - 2, w, &bad);
    if (cmd && cmd->decode) {
        cmd->decode(s->own, it, true, w, nw, bad);
    } else {
        data_set(d, it, &sg->data[2], sg->n - 2);
    }

    return false;
}


// A read of data words, for the command last sent; from start (the command, or the read).
static void command_read(struct codec_dec *d, struct codec_item *t, const struct seg *sg,
                         uint64_t start)
{
    struct chip_dec *s = d->state;
    const struct chip_cmd *cmd = s->cmd >= 0 ? cmd_find(s->chip, (unsigned int)s->cmd) : NULL;
    struct codec_item *it = reading(d, t, start, sg->item->end, "read");
    uint16_t w[WORDS_MAX];
    uint32_t bad = 0;
    size_t nw = 0;

    if (!it) {
        return;
    }
    if (s->cmd < 0) {
        it->flags |= CHIP_UNKNOWN;
    } else {
        name_set(d, it, cmd ? cmd->name : NULL, "0x%04X", (unsigned int)s->cmd);
        codec_set_uint(it, CHIP_RAW, (uint64_t)s->cmd);
    }
    nw = words_get(it, sg->data, sg->n, w, &bad);
    if (cmd && nw != cmd->words) {
        it->flags |= nw < cmd->words ? CHIP_SHORT : CHIP_LONG;
    }
    if (cmd && cmd->decode && nw >= cmd->words && cmd->words) {
        cmd->decode(s->own, it, false, w, nw, bad);
    } else {
        data_set(d, it, sg->data, sg->n);
    }
}


/*
 * A new reading of transaction t from start to end, as op. It shares the transaction's
 * loss and incomplete flags.
 */
static struct codec_item *reading(struct codec_dec *d, struct codec_item *t, uint64_t start,
                                  uint64_t end, const char *op)
{
    struct chip_dec *s = d->state;
    struct codec_item *it = codec_item_child(t, d, s->chip->kind, start);

    if (!it) {
        codec_failed(d, -ENOMEM);
        return NULL;
    }
    it->end = end;
    it->flags = t->flags & (CODEC_ITEM_INCOMPLETE | CODEC_ITEM_LOSS);
    if (codec_set_text(it, CHIP_CHIP, s->chip->label) < 0 ||
        codec_set_text(it, CHIP_OP, op) < 0) {
        codec_failed(d, -ENOMEM);
    }

    return it;
}


static const struct chip_reg *reg_find(const struct chip *chip, unsigned int reg)
{
    for (size_t i = 0; i < chip->reg_count; i++) {
        if (chip->regs[i].reg == reg) {
            return &chip->regs[i];
        }
    }

    return NULL;
}


static const struct chip_cmd *cmd_find(const struct chip *chip, unsigned int cmd)
{
    for (size_t i = 0; i < chip->cmd_count; i++) {
        if (chip->cmds[i].cmd == cmd) {
            return &chip->cmds[i];
        }
    }

    return NULL;
}


// The reg field: name, or the number in hex (fmt) and the unknown flag when it has none.
static void name_set(struct codec_dec *d, struct codec_item *it, const char *name,
                     const char *fmt, unsigned int v)
{
    char num[16];

    if (!name) {
        snprintf(num, sizeof(num), fmt, v);
        it->flags |= CHIP_UNKNOWN;
    }
    if (codec_set_text(it, CHIP_REG, name ? name : num) < 0) {
        codec_failed(d, -ENOMEM);
    }
}


/*
 * Splits n bytes into a command device's data words, two bytes and a CRC each. bad gets a
 * bit for each word whose CRC is wrong; bytes left over make the reading short.
 */
static size_t words_get(struct codec_item *it, const struct codec_word *b, size_t n,
                        uint16_t *w, uint32_t *bad)
{
    size_t nw = 0;

    *bad = 0;
    for (size_t i = 0; i + 3 <= n && nw < WORDS_MAX; i += 3, nw++) {
        uint8_t bytes[2] = {(uint8_t)b[i].value, (uint8_t)b[i + 1].value};

        w[nw] = (uint16_t)(bytes[0] << 8 | bytes[1]);
        if (chip_crc8(bytes, 2) != (uint8_t)b[i + 2].value) {
            *bad |= 1U << nw;
            it->flags |= CHIP_CRC;
        }
    }
    if (n % 3) {
        it->flags |= CHIP_SHORT;
    }

    return nw;
}


// The data field: the bytes in hex.
static void data_set(struct codec_dec *d, struct codec_item *it, const struct codec_word *b,
                     size_t n)
{
    char text[DATA_TEXT_MAX] = "";
    size_t len = 0;

    for (size_t i = 0; i < n && len + 4 < sizeof(text); i++) {
        len += (size_t)snprintf(&text[len], sizeof(text) - len, "%s%02X", i ? " " : "",
                                (unsigned int)b[i].value);
    }
    if (n && codec_set_text(it, CHIP_DATA, text) < 0) {
        codec_failed(d, -ENOMEM);
    }
}


// n bytes as one big-endian value.
static uint64_t be_value(const struct codec_word *b, size_t n)
{
    uint64_t v = 0;

    for (size_t i = 0; i < n; i++) {
        v = v << 8 | (b[i].value & 0xFF);
    }

    return v;
}
