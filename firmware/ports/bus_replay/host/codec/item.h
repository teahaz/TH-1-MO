/*
 * Decoded items: a tree of items, each with a kind, a tick span, flags, named fields and
 * optional words. The model holds for any line: packets with bytes (I2C, UART),
 * side-by-side lanes (SPI, I2S), measurements with fields only (PWM, plain pins). Items are
 * a cache over the raw records: they can always be rebuilt, and every one keeps the tick
 * span it came from.
 *
 * A kind describes its items once: its fields, its lanes and the names of its flags. Items
 * carry only values, so the outputs are generated from the kind without knowing the
 * protocol.
 */

#ifndef CODEC_ITEM_H
#define CODEC_ITEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


// Field types.
#define CODEC_UINT 0
#define CODEC_INT 1
#define CODEC_REAL 2
#define CODEC_TEXT 3
#define CODEC_BOOL 4

// Field flags: shown on the text output's header line; an unsigned value shown in hex.
#define CODEC_FIELD_HEADER 0x01
#define CODEC_FIELD_HEX 0x02

// How a kind's lanes carry words: one at a time (I2C, UART), or side by side (SPI, I2S).
#define CODEC_LANES_TURNS 0
#define CODEC_LANES_SIDE 1

// Words are read as text where they are printable (UART), not as hex.
#define CODEC_KIND_TEXT 0x01

/*
 * Item flags. The low 8 bits are the same for every kind; bits 8 to 31 are the kind's own,
 * named by its flags list (bit 8 + i is flags[i]).
 *  - INCOMPLETE: the item was cut off: the capture ended, or it never finished.
 *  - LOSS: records the item depends on may be missing (a reported loss, or levels a
 *    heartbeat corrected).
 */
#define CODEC_ITEM_INCOMPLETE 0x01
#define CODEC_ITEM_LOSS 0x02
#define CODEC_ITEM_KIND_SHIFT 8

/*
 * Word flags. BREAK starts a new group (a new line in the text output), ERROR marks a word
 * that did not read cleanly (a framing error); bits 2 to 7 are the kind's own, named by its
 * word_flags list (bit 2 + i is word_flags[i]).
 */
#define CODEC_WORD_BREAK 0x01
#define CODEC_WORD_ERROR 0x02
#define CODEC_WORD_KIND_SHIFT 2

/*
 * digits: hex digits for a hex field, decimals for a real one (0: as needed). width is the
 * text output's minimum column width for a header field. unit follows the value.
 */
struct codec_field_desc {
    const char *name;
    uint8_t type;
    uint8_t flags;
    uint8_t digits;
    uint8_t width;
    const char *unit;
};

// flags and word_flags are NULL-terminated lists, or NULL.
struct codec_kind {
    const char *name;
    const struct codec_field_desc *fields;
    uint8_t field_count;
    const char *const *lanes;
    uint8_t lane_count;
    uint8_t lane_mode;
    uint8_t flags;
    const char *const *item_flags;
    const char *const *word_flags;
};

struct codec_value {
    bool set;
    union {
        uint64_t u;
        int64_t i;
        double r;
        char *s;
        bool b;
    };
};

// A word of data: bits wide, on lane, read from start to end (ticks, inclusive).
struct codec_word {
    uint64_t start;
    uint64_t end;
    uint64_t value;
    uint8_t bits;
    uint8_t lane;
    uint8_t flags;
};

struct codec_dec;

/*
 * An item. dec is the decoder that made it: a child made by a decoder stacked on its
 * parent's is that decoder's interpretation of the parent. detail is the output level the
 * item needs to be shown (0: always). values has one entry per field of the kind. payload
 * is an optional typed payload in the kind's own header, freed with payload_free.
 */
struct codec_item {
    uint64_t id;
    const struct codec_kind *kind;
    const struct codec_dec *dec;
    uint64_t start;
    uint64_t end;
    uint32_t flags;
    uint8_t detail;

    struct codec_item *parent;
    struct codec_item *child;
    struct codec_item *last_child;
    struct codec_item *next;

    struct codec_value *values;
    struct codec_word *words;
    size_t word_count;
    size_t word_cap;

    void *payload;
    void (*payload_free)(void *payload);
};


struct codec_item *codec_item_new(const struct codec_dec *dec, const struct codec_kind *kind,
                                  uint64_t start);
struct codec_item *codec_item_child(struct codec_item *parent, const struct codec_dec *dec,
                                    const struct codec_kind *kind, uint64_t start);
void codec_item_free(struct codec_item *item);

void codec_set_uint(struct codec_item *item, unsigned int field, uint64_t v);
void codec_set_int(struct codec_item *item, unsigned int field, int64_t v);
void codec_set_real(struct codec_item *item, unsigned int field, double v);
void codec_set_bool(struct codec_item *item, unsigned int field, bool v);
int codec_set_text(struct codec_item *item, unsigned int field, const char *s);
int codec_word_add(struct codec_item *item, const struct codec_word *w);

const char *codec_flag_name(const struct codec_kind *kind, unsigned int bit);
const char *codec_word_flag_name(const struct codec_kind *kind, unsigned int bit);

#endif
