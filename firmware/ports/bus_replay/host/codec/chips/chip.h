/*
 * Chip-level decoders: a reading of what an I2C device's transactions mean, stacked on an
 * i2c decoder and bound to one address (addr=, required). Each reading is a child of the
 * transaction it reads, so the bytes stay where they are and the chip line is a layer on
 * top.
 *
 * A chip is described by a struct chip in a file of its own: the addresses it can answer to
 * (a binding to any other is refused), and a table of its registers or commands with a
 * decode function each. The code here follows the access pattern, so a chip file never
 * looks at bytes:
 *
 *  - CHIP_REGISTER: a register pointer device (INA219). A write of one byte sets the
 *    pointer; a write of the pointer and a value writes a register; a read reads the
 *    register the pointer was last set to, in this transaction or an earlier one.
 *  - CHIP_COMMAND: a command device with CRC words (SHT3x). A write of a 16-bit command,
 *    with data words after it if the command takes any; a read gives data words, each two
 *    bytes and a CRC-8, read for the command last sent.
 *
 * A segment whose address byte was not acknowledged gives no reading; the I2C line shows
 * the NAK. Anything else not understood (an unknown register or command, a short or long
 * transfer, a read with no command or pointer before it) is still read, and flagged. A loss
 * forgets the pointer and the command: a read after it is flagged unknown.
 */

#ifndef CODEC_CHIPS_CHIP_H
#define CODEC_CHIPS_CHIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "codec/codec.h"
#include "codec/item.h"


#define CHIP_REGISTER 0
#define CHIP_COMMAND 1

/*
 * The fields every chip kind starts with (CHIP_COMMON_FIELDS), then its own from
 * CHIP_FIELDS on:
 *  - chip: the chip's label (INA219).
 *  - op: read, write, pointer (a pointer write with no read after it), command, empty (an
 *    address byte alone).
 *  - reg: the register's or command's name, its number in hex when it has none.
 *  - raw: the register value, or the command.
 *  - data: words nothing else shows, in hex (an unknown register's value, a short read).
 */
#define CHIP_CHIP 0
#define CHIP_OP 1
#define CHIP_REG 2
#define CHIP_RAW 3
#define CHIP_DATA 4
#define CHIP_FIELDS 5

#define CHIP_COMMON_FIELDS                                                                    \
    {.name = "chip", .type = CODEC_TEXT, .flags = CODEC_FIELD_HEADER, .width = 6},            \
    {.name = "op", .type = CODEC_TEXT, .flags = CODEC_FIELD_HEADER, .width = 7},              \
    {.name = "reg", .type = CODEC_TEXT, .flags = CODEC_FIELD_HEADER, .width = 11},            \
    {.name = "raw", .type = CODEC_UINT, .flags = CODEC_FIELD_HEX, .digits = 4},               \
    {.name = "data", .type = CODEC_TEXT, .flags = CODEC_FIELD_HEADER}

/*
 * The flags every chip kind starts with (CHIP_COMMON_FLAGS), then its own from
 * CHIP_FLAG_SHIFT on:
 *  - short, long: fewer or more bytes than the register or command needs.
 *  - unknown: a register or command not in the table, or a read with no pointer or command
 *    before it.
 *  - crc: a data word's CRC is wrong.
 */
#define CHIP_SHORT (1U << CODEC_ITEM_KIND_SHIFT)
#define CHIP_LONG (1U << (CODEC_ITEM_KIND_SHIFT + 1))
#define CHIP_UNKNOWN (1U << (CODEC_ITEM_KIND_SHIFT + 2))
#define CHIP_CRC (1U << (CODEC_ITEM_KIND_SHIFT + 3))
#define CHIP_FLAG_SHIFT (CODEC_ITEM_KIND_SHIFT + 4)

#define CHIP_COMMON_FLAGS "short", "long", "unknown", "crc"

// The arguments every chip takes (CHIP_COMMON_PARAMS), before its own.
#define CHIP_COMMON_PARAMS                                                                    \
    {"addr", "its 7-bit I2C address, for example 0x44 (required)"},                           \
    {"on", "label of the i2c decoder it reads (default the last one given)"}

// The decoder callbacks every chip uses (codec.h, struct codec_decoder).
#define CHIP_DECODER_OPS .input = "i2c", .create = chip_create, .destroy = chip_destroy,     \
                         .item = chip_item, .loss = chip_loss

/*
 * A register of a register pointer device, with bytes bytes of value (chip->reg_bytes).
 * decode fills in the reading of a read or write of value; it is NULL for a register only
 * named.
 */
struct chip_reg {
    uint8_t reg;
    const char *name;
    void (*decode)(void *state, struct codec_item *it, bool write, uint64_t value);
};

/*
 * A command of a command device. words: data words a read after it gives (0 for a command
 * that gives none). decode fills in the reading of those words (or of the data words written
 * with the command, write true); it is NULL for a command only named. bad has a bit set for
 * each word whose CRC is wrong; the words are given anyway.
 */
struct chip_cmd {
    uint16_t cmd;
    const char *name;
    uint8_t words;
    void (*decode)(void *state, struct codec_item *it, bool write, const uint16_t *w, size_t n,
                   uint32_t bad);
};

/*
 * A chip. addrs lists the addresses it can answer to. kind's fields start with
 * CHIP_COMMON_FIELDS and its flags with CHIP_COMMON_FLAGS. state_size bytes of state are
 * kept per decoder, zeroed, and set up by create from the chip's own arguments (NULL if it
 * takes none).
 */
struct chip {
    const char *label;
    const uint8_t *addrs;
    size_t addr_count;
    const struct codec_kind *kind;
    uint8_t pattern;

    const struct chip_reg *regs;
    size_t reg_count;
    uint8_t reg_bytes;

    const struct chip_cmd *cmds;
    size_t cmd_count;

    size_t state_size;
    int (*create)(void *state, const struct codec_args *args, char *error, size_t size);
};


int chip_create(struct codec_dec *d, const struct codec_args *args, char *error, size_t size);
void chip_destroy(struct codec_dec *d);
void chip_item(struct codec_dec *d, struct codec_item *item);
void chip_loss(struct codec_dec *d, uint64_t start, uint64_t end);

uint8_t chip_crc8(const uint8_t *data, size_t len);

#endif
