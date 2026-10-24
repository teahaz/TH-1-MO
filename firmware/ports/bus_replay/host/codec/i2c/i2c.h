/*
 * I2C: transactions from START to STOP, with a segment per START (a repeated START begins
 * a new one), and every byte with its ACK. Its arguments: scl=, sda= (channels).
 *
 * A transaction's words are its bytes: the address bytes and written data on the master
 * lane, read data on the target lane, each flagged I2C_WORD_NAK when it was not
 * acknowledged; a segment's address byte starts a new group. Its segments are children (at
 * detail 1), each with a typed payload, struct i2c_segment, saying which of the
 * transaction's words are its own; chip-level decoders read them from it.
 */

#ifndef CODEC_I2C_H
#define CODEC_I2C_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "codec/codec.h"
#include "codec/item.h"


// Transaction fields.
#define I2C_ADDR 0
#define I2C_DIR 1
#define I2C_LEN 2
#define I2C_STATUS 3

// Segment fields.
#define I2C_SEG_ADDR 0
#define I2C_SEG_DIR 1
#define I2C_SEG_LEN 2

#define I2C_MASTER 0
#define I2C_TARGET 1

#define I2C_WORD_NAK (1U << CODEC_WORD_KIND_SHIFT)

/*
 * Transaction flags: an address byte not acknowledged; a data byte not acknowledged that
 * should have been (not the last byte a master reads); a START or STOP in the middle of a
 * byte.
 */
#define I2C_NAK_ADDR (1U << CODEC_ITEM_KIND_SHIFT)
#define I2C_NAK_DATA (1U << (CODEC_ITEM_KIND_SHIFT + 1))
#define I2C_CUT_BYTE (1U << (CODEC_ITEM_KIND_SHIFT + 2))

/*
 * A segment: its words are first to first + count - 1 of the transaction's, the address
 * byte first (has_addr false for a segment with no complete byte).
 */
struct i2c_segment {
    bool has_addr;
    uint8_t addr;
    bool read;
    size_t first;
    size_t count;
};

extern const struct codec_kind i2c_transaction;
extern const struct codec_kind i2c_segment;
extern const struct codec_decoder i2c_decoder;

#endif
