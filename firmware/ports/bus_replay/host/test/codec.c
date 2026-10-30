/*
 * libcodec on hand-built records: the I2C, UART and activity decoders, losses, stacking,
 * and the text and JSON outputs.
 *
 * A simulated capture: one data channel, every channel enabled, records as built, and a
 * heartbeat every C ticks with the levels so far, as the device sends them.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codec/activity/activity.h"
#include "codec/codec.h"
#include "codec/i2c/i2c.h"
#include "codec/item.h"
#include "codec/output.h"
#include "codec/uart/uart.h"
#include "common/capture_info.h"
#include "common/record.h"
#include "test/test.h"


#define RATE 1000000
#define C 100
#define CHANNELS 4
#define RECORDS_MAX 20000
#define ITEMS_MAX 64

#define SCL 0
#define SDA 1
#define TX 2
#define RX 3

// I2C timing in ticks: a quarter bit.
#define Q 3
// UART: ticks per bit (100 kbaud).
#define BIT 10

struct sim {
    struct capture_info info;
    char *names[CHANNELS];
    uint16_t enabled[CHANNELS];
    uint16_t block[2];
    uint8_t tag[1];
    uint8_t width[1];
    uint8_t set[1];

    struct record rec[RECORDS_MAX];
    size_t count;
    uint8_t level[CHANNELS];
    uint64_t t;
};

// Items collected from a session, in release order.
struct got {
    struct codec_item *item[ITEMS_MAX];
    size_t count;
};


static void sim_init(struct sim *s);
static void edge(struct sim *s, unsigned int ch, int level);
static void run(struct sim *s, struct codec *c, struct got *g, bool finish);
static void got_free(struct got *g);
static struct codec *session(struct sim *s, const char *name, const char *args, uint64_t hold);
static void i2c_start(struct sim *s);
static void i2c_stop(struct sim *s);
static void i2c_bit(struct sim *s, int b);
static void i2c_byte(struct sim *s, uint8_t v, int nak);
static void uart_byte(struct sim *s, unsigned int ch, uint8_t v, bool bad_stop);
static void test_i2c_write(void);
static void test_i2c_restart(void);
static void test_i2c_nak(void);
static void test_i2c_cut(void);
static void test_i2c_same_tick(void);
static void test_i2c_loss(void);
static void test_uart(void);
static void test_uart_split(void);
static void test_uart_framing(void);
static void test_activity(void);
static void test_stacking(void);
static void test_output(void);
static void test_pretty(void);
static void test_args(void);
static void test_advance(void);


int main(void)
{
    test_i2c_write();
    test_i2c_restart();
    test_i2c_nak();
    test_i2c_cut();
    test_i2c_same_tick();
    test_i2c_loss();
    test_uart();
    test_uart_split();
    test_uart_framing();
    test_activity();
    test_stacking();
    test_output();
    test_pretty();
    test_args();
    test_advance();

    return test_result(__FILE__);
}


static void sim_init(struct sim *s)
{
    static char *const names[CHANNELS] = {"SCL", "SDA", "TX", "RX"};

    memset(s, 0, sizeof(*s));
    for (unsigned int i = 0; i < CHANNELS; i++) {
        s->names[i] = names[i];
        s->enabled[i] = (uint16_t)i;
        s->level[i] = 1;
    }
    s->block[1] = CHANNELS;
    s->set[0] = (1U << CHANNELS) - 1;
    s->info = (struct capture_info){
        .rate = RATE, .c = C, .channels = CHANNELS, .data_channels = 1, .names = s->names,
        .set_size = 1, .enabled_set = s->set, .split_set = NULL, .n = CHANNELS,
        .enabled = s->enabled, .block = s->block, .tag = s->tag, .width = s->width,
    };
    s->t = 10 * C;
}


// A change of ch to level at the current tick.
static void edge(struct sim *s, unsigned int ch, int level)
{
    if (s->level[ch] == level || s->count == RECORDS_MAX) {
        return;
    }
    s->rec[s->count++] = (struct record){.tick = s->t, .channel = (uint16_t)ch,
                                         .level = (uint8_t)level};
    s->level[ch] = (uint8_t)level;
}


/*
 * Feeds the records with a heartbeat at every grid point from 0 to well past the last
 * record, collecting what the session releases.
 */
static void run(struct sim *s, struct codec *c, struct got *g, bool finish)
{
    uint8_t lv[CHANNELS] = {1, 1, 1, 1};
    uint64_t end = s->t + 200 * C;
    size_t r = 0;
    struct codec_item *it = NULL;

    // Records are sorted by tick already; same-tick records stay in channel order.
    for (uint64_t grid = 0; grid * C <= end; grid++) {
        uint8_t mask = 0;
        struct heartbeat hb = {.grid = grid, .tick = grid * C, .n = CHANNELS, .levels = &mask};

        while (r < s->count && s->rec[r].tick <= grid * C) {
            CHECK(codec_record(c, &s->rec[r]) == 0);
            lv[s->rec[r].channel] = s->rec[r].level;
            r++;
        }
        for (unsigned int i = 0; i < CHANNELS; i++) {
            mask |= (uint8_t)(lv[i] << i);
        }
        CHECK(codec_heartbeat(c, &hb) == 0);
        while ((it = codec_next(c)) && g->count < ITEMS_MAX) {
            g->item[g->count++] = it;
        }
    }
    if (finish) {
        codec_finish(c);
        while ((it = codec_next(c)) && g->count < ITEMS_MAX) {
            g->item[g->count++] = it;
        }
    }
    CHECK(codec_error(c) == 0);
}


static void got_free(struct got *g)
{
    for (size_t i = 0; i < g->count; i++) {
        codec_item_free(g->item[i]);
    }
    g->count = 0;
}


static struct codec *session(struct sim *s, const char *name, const char *args, uint64_t hold)
{
    struct codec *c = NULL;
    char error[CODEC_ERROR_LEN];

    CHECK(codec_new(&c, &s->info, hold) == 0);
    if (!c) {
        return NULL;
    }
    CHECK(codec_add(c, codec_find(name), args, NULL, NULL, error, sizeof(error)) == 0);

    return c;
}


static void i2c_start(struct sim *s)
{
    edge(s, SDA, 0);
    s->t += 2 * Q;
    edge(s, SCL, 0);
    s->t += Q;
}


static void i2c_stop(struct sim *s)
{
    edge(s, SDA, 0);
    s->t += Q;
    edge(s, SCL, 1);
    s->t += 2 * Q;
    edge(s, SDA, 1);
    s->t += 4 * Q;
}


// SDA set while SCL is low, then a clock pulse.
static void i2c_bit(struct sim *s, int b)
{
    edge(s, SDA, b);
    s->t += Q;
    edge(s, SCL, 1);
    s->t += 2 * Q;
    edge(s, SCL, 0);
    s->t += Q;
}


static void i2c_byte(struct sim *s, uint8_t v, int nak)
{
    for (int b = 7; b >= 0; b--) {
        i2c_bit(s, (v >> b) & 1);
    }
    i2c_bit(s, nak);
}


static void uart_byte(struct sim *s, unsigned int ch, uint8_t v, bool bad_stop)
{
    uint64_t t0 = s->t;

    for (int b = -1; b <= 8; b++) {
        int level = b < 0 ? 0 : b == 8 ? !bad_stop : (v >> b) & 1;

        s->t = t0 + (uint64_t)(b + 1) * BIT;
        edge(s, ch, level);
    }
    s->t = t0 + 10 * BIT;
    if (bad_stop) {
        s->t += 5 * BIT;
        edge(s, ch, 1);
    }
}


static const struct codec_item *only(const struct got *g)
{
    CHECK(g->count == 1);
    return g->count ? g->item[0] : NULL;
}


static bool text_is(const struct codec_item *it, unsigned int field, const char *want)
{
    return it->values[field].set && !strcmp(it->values[field].s, want);
}


static void test_i2c_write(void)
{
    struct sim s;
    struct got g = {0};
    struct codec *c = NULL;
    const struct codec_item *t = NULL;

    sim_init(&s);
    i2c_start(&s);
    i2c_byte(&s, 0x88, 0);
    i2c_byte(&s, 0x24, 0);
    i2c_byte(&s, 0x00, 0);
    i2c_stop(&s);
    c = session(&s, "i2c", "scl=0,sda=1", 0);
    run(&s, c, &g, true);

    t = only(&g);
    if (t) {
        CHECK(t->kind == &i2c_transaction);
        CHECK(t->values[I2C_ADDR].u == 0x44);
        CHECK(text_is(t, I2C_DIR, "W"));
        CHECK(t->values[I2C_LEN].u == 2);
        CHECK(text_is(t, I2C_STATUS, "ACK"));
        CHECK(t->flags == 0);
        CHECK(t->word_count == 3);
        CHECK(t->words[0].value == 0x88 && t->words[1].value == 0x24 && t->words[2].value == 0);
        for (size_t i = 0; i < t->word_count; i++) {
            CHECK(t->words[i].lane == I2C_MASTER);
            CHECK(!(t->words[i].flags & I2C_WORD_NAK));
        }
        CHECK(t->start == 10 * C);
        CHECK(t->child && !t->child->next);
    }
    got_free(&g);
    codec_free(c);
}


// A write, a repeated START, a read whose last byte the master NAKs: one transaction.
static void test_i2c_restart(void)
{
    struct sim s;
    struct got g = {0};
    struct codec *c = NULL;
    const struct codec_item *t = NULL;

    sim_init(&s);
    i2c_start(&s);
    i2c_byte(&s, 0x86, 0);
    i2c_byte(&s, 0x02, 0);
    edge(&s, SDA, 1);
    s.t += Q;
    edge(&s, SCL, 1);
    s.t += Q;
    i2c_start(&s);
    i2c_byte(&s, 0x87, 0);
    i2c_byte(&s, 0x1F, 0);
    i2c_byte(&s, 0x92, 1);
    i2c_stop(&s);
    c = session(&s, "i2c", "scl=0,sda=1", 0);
    run(&s, c, &g, true);

    t = only(&g);
    if (t) {
        const struct codec_item *s1 = t->child;
        const struct codec_item *s2 = s1 ? s1->next : NULL;

        CHECK(t->values[I2C_ADDR].u == 0x43);
        CHECK(text_is(t, I2C_DIR, "W+R"));
        CHECK(t->values[I2C_LEN].u == 3);
        CHECK(text_is(t, I2C_STATUS, "ACK"));
        CHECK(t->flags == 0);
        CHECK(t->word_count == 5);
        CHECK(t->word_count == 5 && (t->words[2].flags & CODEC_WORD_BREAK));
        CHECK(t->word_count == 5 && t->words[2].lane == I2C_MASTER);
        CHECK(t->word_count == 5 && t->words[3].lane == I2C_TARGET);
        CHECK(s2 && !s2->next);
        if (s2) {
            const struct i2c_segment *p = s2->payload;

            CHECK(s1->detail == 1);
            CHECK(text_is(s1, I2C_SEG_DIR, "W") && text_is(s2, I2C_SEG_DIR, "R"));
            CHECK(s1->values[I2C_SEG_LEN].u == 1 && s2->values[I2C_SEG_LEN].u == 2);
            CHECK(p->has_addr && p->addr == 0x43 && p->read && p->first == 2 && p->count == 3);
        }
    }
    got_free(&g);
    codec_free(c);
}


static void test_i2c_nak(void)
{
    struct sim s;
    struct got g = {0};
    struct codec *c = NULL;

    // No target at the address; then a write whose data byte is refused.
    sim_init(&s);
    i2c_start(&s);
    i2c_byte(&s, 0xA0, 1);
    i2c_stop(&s);
    i2c_start(&s);
    i2c_byte(&s, 0x88, 0);
    i2c_byte(&s, 0x55, 1);
    i2c_stop(&s);
    c = session(&s, "i2c", "scl=0,sda=1", 0);
    run(&s, c, &g, true);

    CHECK(g.count == 2);
    if (g.count == 2) {
        CHECK(text_is(g.item[0], I2C_STATUS, "NAK addr"));
        CHECK(g.item[0]->flags & I2C_NAK_ADDR);
        CHECK(g.item[0]->words[0].flags & I2C_WORD_NAK);
        CHECK(text_is(g.item[1], I2C_STATUS, "NAK"));
        CHECK(g.item[1]->flags & I2C_NAK_DATA);
        CHECK(!(g.item[1]->flags & I2C_NAK_ADDR));
    }
    got_free(&g);
    codec_free(c);
}


// A STOP after four bits; a transaction never stopped is incomplete at the end.
static void test_i2c_cut(void)
{
    struct sim s;
    struct got g = {0};
    struct codec *c = NULL;

    sim_init(&s);
    i2c_start(&s);
    i2c_byte(&s, 0x88, 0);
    for (int i = 0; i < 4; i++) {
        i2c_bit(&s, i & 1);
    }
    i2c_stop(&s);
    i2c_start(&s);
    i2c_byte(&s, 0x88, 0);
    c = session(&s, "i2c", "scl=0,sda=1", 0);
    run(&s, c, &g, true);

    CHECK(g.count == 2);
    if (g.count == 2) {
        CHECK(g.item[0]->flags & I2C_CUT_BYTE);
        CHECK(text_is(g.item[0], I2C_STATUS, "ERR"));
        CHECK(g.item[0]->word_count == 1);
        CHECK(g.item[1]->flags & CODEC_ITEM_INCOMPLETE);
        CHECK(g.item[1]->word_count == 1);
    }
    got_free(&g);
    codec_free(c);
}


/*
 * Edges sampled into one tick: SDA changing on the tick SCL falls, and (every third bit) on
 * the tick SCL rises. In valid I2C SCL falls, SDA changes, SCL rises; taken in that order
 * nothing reads as a START or STOP. The records of a tick come in channel order, SCL first.
 */
static void test_i2c_same_tick(void)
{
    static const uint8_t bytes[] = {0x88, 0xA5, 0x5A};
    struct sim s;
    struct got g = {0};
    struct codec *c = NULL;
    unsigned int k = 0;

    sim_init(&s);
    edge(&s, SDA, 0);
    s.t += 2 * Q;
    for (size_t i = 0; i < sizeof(bytes); i++) {
        for (int b = 8; b >= 0; b--, k++) {
            int v = b ? (bytes[i] >> (b - 1)) & 1 : 0;

            edge(&s, SCL, 0);
            if (k % 3 == 2) {
                s.t += 2 * Q;
                edge(&s, SCL, 1);
                edge(&s, SDA, v);
            } else {
                edge(&s, SDA, v);
                s.t += 2 * Q;
                edge(&s, SCL, 1);
            }
            s.t += 2 * Q;
        }
    }
    edge(&s, SCL, 0);
    edge(&s, SDA, 0);
    s.t += 2 * Q;
    edge(&s, SCL, 1);
    s.t += 2 * Q;
    edge(&s, SDA, 1);
    s.t += 2 * Q;
    c = session(&s, "i2c", "scl=0,sda=1", 0);
    run(&s, c, &g, true);

    CHECK(g.count == 1);
    if (g.count == 1) {
        const struct codec_item *t = g.item[0];

        CHECK(t->word_count == 3);
        for (size_t i = 0; i < t->word_count && i < sizeof(bytes); i++) {
            CHECK(t->words[i].value == bytes[i]);
        }
        CHECK(text_is(t, I2C_STATUS, "ACK"));
        CHECK(t->flags == 0);
    }
    got_free(&g);
    codec_free(c);
}


// A loss over a transaction marks it, whether it is still open or held after its end.
static void test_i2c_loss(void)
{
    struct sim s;
    struct got g = {0};
    struct codec *c = NULL;
    uint32_t lost[CHANNELS] = {0, 3, 0, 0};
    uint32_t none[CHANNELS] = {0, 0, 0, 2};
    struct loss l = {.n = CHANNELS, .lost = lost};
    struct loss other = {.n = CHANNELS, .lost = none};
    uint64_t first = 0;

    sim_init(&s);
    i2c_start(&s);
    i2c_byte(&s, 0x88, 0);
    i2c_stop(&s);
    first = s.t;
    s.t += 50 * C;
    i2c_start(&s);
    i2c_byte(&s, 0x88, 0);
    i2c_stop(&s);

    // A long hold: nothing is released before the loss arrives.
    c = session(&s, "i2c", "scl=0,sda=1", 1000 * C);
    run(&s, c, &g, false);
    CHECK(g.count == 0);
    l.start = l.end = first - 4 * Q - 5;
    other.start = 0;
    other.end = s.t;
    codec_loss(c, &l);
    codec_loss(c, &other);
    codec_finish(c);
    for (struct codec_item *it; (it = codec_next(c)) && g.count < ITEMS_MAX;) {
        g.item[g.count++] = it;
    }

    CHECK(g.count == 2);
    if (g.count == 2) {
        CHECK(g.item[0]->flags & CODEC_ITEM_LOSS);
        CHECK(!(g.item[1]->flags & CODEC_ITEM_LOSS));
    }
    got_free(&g);
    codec_free(c);
}


static void test_uart(void)
{
    static const char msg[] = "Hi\n";
    struct sim s;
    struct got g = {0};
    struct codec *c = NULL;
    const struct codec_item *b = NULL;

    sim_init(&s);
    for (const char *p = msg; *p; p++) {
        uart_byte(&s, TX, (uint8_t)*p, false);
    }
    c = session(&s, "uart", "tx=2,baud=100000", 0);
    run(&s, c, &g, true);

    b = only(&g);
    if (b) {
        CHECK(b->kind == &uart_burst);
        CHECK(text_is(b, UART_DIR, "TX"));
        CHECK(b->values[UART_BYTES].u == 3);
        CHECK(!b->values[UART_ERRORS].set);
        CHECK(b->word_count == 3);
        for (size_t i = 0; i < b->word_count; i++) {
            CHECK(b->words[i].value == (uint8_t)msg[i]);
            CHECK(b->words[i].lane == UART_TX);
            CHECK(b->words[i].flags == 0);
        }
        CHECK(b->start == 10 * C);
    }
    got_free(&g);
    codec_free(c);
}


// Bursts end at a gap longer than gap= byte times, and when the other direction sends.
static void test_uart_split(void)
{
    struct sim s;
    struct got g = {0};
    struct codec *c = NULL;

    sim_init(&s);
    uart_byte(&s, TX, 'a', false);
    uart_byte(&s, TX, 'b', false);
    s.t += 5 * 10 * BIT;
    uart_byte(&s, TX, 'c', false);
    uart_byte(&s, RX, 'd', false);
    uart_byte(&s, TX, 'e', false);
    c = session(&s, "uart", "tx=2,rx=3,baud=100000,gap=4", 0);
    run(&s, c, &g, true);

    CHECK(g.count == 4);
    if (g.count == 4) {
        CHECK(g.item[0]->word_count == 2);
        CHECK(g.item[1]->word_count == 1 && g.item[1]->words[0].value == 'c');
        CHECK(text_is(g.item[2], UART_DIR, "RX") && g.item[2]->words[0].lane == UART_RX);
        CHECK(text_is(g.item[3], UART_DIR, "TX"));
    }
    got_free(&g);
    codec_free(c);
}


static void test_uart_framing(void)
{
    struct sim s;
    struct got g = {0};
    struct codec *c = NULL;
    const struct codec_item *b = NULL;

    sim_init(&s);
    uart_byte(&s, TX, 'x', false);
    uart_byte(&s, TX, 'y', true);
    c = session(&s, "uart", "tx=2,baud=100000", 0);
    run(&s, c, &g, true);

    b = only(&g);
    if (b) {
        CHECK(b->word_count == 2);
        CHECK(b->values[UART_ERRORS].set && b->values[UART_ERRORS].u == 1);
        CHECK(b->word_count == 2 && !(b->words[0].flags & CODEC_WORD_ERROR));
        CHECK(b->word_count == 2 && (b->words[1].flags & CODEC_WORD_ERROR));
    }
    got_free(&g);
    codec_free(c);
}


static void test_activity(void)
{
    struct sim s;
    struct got g = {0};
    struct codec *c = NULL;

    sim_init(&s);
    i2c_start(&s);
    i2c_byte(&s, 0x88, 0);
    i2c_stop(&s);
    s.t += 10 * C;
    uart_byte(&s, TX, 'z', false);
    c = session(&s, "activity", "gap=0.001", 0);
    run(&s, c, &g, true);

    CHECK(g.count == 2);
    if (g.count == 2) {
        CHECK(text_is(g.item[0], ACTIVITY_LINES, "0+1"));
        CHECK(g.item[0]->values[ACTIVITY_RECORDS].u > 18);
        CHECK(text_is(g.item[1], ACTIVITY_LINES, "2"));
        CHECK(g.item[1]->values[ACTIVITY_RECORDS].u > 2);
    }
    got_free(&g);
    codec_free(c);
}


/*
 * A decoder stacked on i2c: it sees every transaction before it is released, and adds its
 * reading as a child.
 */
static const struct codec_field_desc note_fields[] = {
    {.name = "note", .type = CODEC_TEXT, .flags = CODEC_FIELD_HEADER},
};

static const struct codec_kind note_kind = {
    .name = "note",
    .fields = note_fields,
    .field_count = 1,
};

static int note_create(struct codec_dec *d, const struct codec_args *args, char *error,
                       size_t size)
{
    (void)args;
    (void)error;
    (void)size;
    d->state = d;
    return 0;
}

static void note_item(struct codec_dec *d, struct codec_item *item)
{
    struct codec_item *n = codec_item_child(item, d, &note_kind, item->start);

    if (n) {
        n->end = item->end;
        codec_set_text(n, 0, item->word_count > 1 ? "register write" : "address only");
    }
}

static const struct codec_decoder note_decoder = {
    .name = "note",
    .summary = "a test",
    .input = "i2c",
    .create = note_create,
    .item = note_item,
};


static void test_stacking(void)
{
    struct sim s;
    struct got g = {0};
    struct codec *c = NULL;
    struct codec_dec *i2c = NULL;
    struct codec_dec *note = NULL;
    char error[CODEC_ERROR_LEN];

    sim_init(&s);
    i2c_start(&s);
    i2c_byte(&s, 0x88, 0);
    i2c_byte(&s, 0x01, 0);
    i2c_stop(&s);
    CHECK(codec_new(&c, &s.info, 0) == 0);
    if (!c) {
        return;
    }
    // Nothing to go on yet.
    CHECK(codec_add(c, &note_decoder, "", NULL, NULL, error, sizeof(error)) < 0);
    CHECK(codec_add(c, codec_find("i2c"), "scl=0,sda=1", NULL, &i2c, error, sizeof(error)) == 0);
    CHECK(codec_add(c, &note_decoder, "", NULL, &note, error, sizeof(error)) == 0);
    CHECK(note && note->lower == i2c);
    run(&s, c, &g, true);

    CHECK(g.count == 1);
    if (g.count == 1) {
        const struct codec_item *n = g.item[0]->child ? g.item[0]->child->next : NULL;

        CHECK(n && n->dec == note && n->kind == &note_kind && n->detail == 0);
        CHECK(n && text_is(n, 0, "register write"));
        CHECK(n && n->id > g.item[0]->id);
    }
    got_free(&g);
    codec_free(c);
}


// The outputs of a small tree: lanes taking turns, text words, side-by-side lanes, children.
static void test_output(void)
{
    static const struct codec_field_desc fields[] = {
        {.name = "addr", .type = CODEC_UINT, .flags = CODEC_FIELD_HEADER | CODEC_FIELD_HEX,
         .digits = 2, .width = 4},
        {.name = "len", .type = CODEC_UINT, .flags = CODEC_FIELD_HEADER, .unit = "bytes"},
        {.name = "hidden", .type = CODEC_TEXT},
    };
    static const char *const lanes[] = {"mosi", "miso"};
    static const char *const flags[] = {"odd", NULL};
    const struct codec_kind turns = {.name = "t", .fields = fields, .field_count = 3,
                                     .lanes = lanes, .lane_count = 2,
                                     .lane_mode = CODEC_LANES_TURNS, .item_flags = flags};
    const struct codec_kind side = {.name = "s", .fields = fields, .field_count = 2,
                                    .lanes = lanes, .lane_count = 2,
                                    .lane_mode = CODEC_LANES_SIDE};
    const struct codec_kind text = {.name = "x", .lanes = lanes, .lane_count = 2,
                                    .flags = CODEC_KIND_TEXT};
    struct codec_dec dec = {.name = "dev"};
    struct codec_dec other = {.name = "chip"};
    struct output_options opt = {.level = 0, .name_width = 4, .rate = 1000};
    struct codec_item *a = codec_item_new(&dec, &turns, 1500);
    struct codec_item *b = codec_item_new(&dec, &side, 2000);
    struct codec_item *x = codec_item_new(&dec, &text, 2500);
    struct codec_item *ch = NULL;
    char *buf = NULL;
    size_t len = 0;
    FILE *f = NULL;

    if (!a || !b || !x) {
        CHECK(false);
        return;
    }
    a->id = 7;
    a->end = 1510;
    a->flags = CODEC_ITEM_LOSS | (1U << CODEC_ITEM_KIND_SHIFT);
    codec_set_uint(a, 0, 0x44);
    codec_set_uint(a, 1, 2);
    codec_set_text(a, 2, "x\"y");
    codec_word_add(a, &(struct codec_word){.value = 0x89, .bits = 8, .lane = 0});
    codec_word_add(a, &(struct codec_word){.value = 0x60, .bits = 8, .lane = 1});
    codec_word_add(a, &(struct codec_word){.value = 0xD5, .bits = 8, .lane = 1,
                                           .flags = CODEC_WORD_ERROR});
    ch = codec_item_child(a, &other, &turns, 1500);
    codec_set_uint(ch, 0, 0x10);
    ch->id = 8;
    ch = codec_item_child(a, &dec, &turns, 1500);
    ch->detail = 1;
    codec_set_uint(ch, 0, 0x11);

    codec_set_uint(b, 0, 1);
    codec_word_add(b, &(struct codec_word){.value = 0x9F, .bits = 8, .lane = 0});
    codec_word_add(b, &(struct codec_word){.value = 0xFF, .bits = 8, .lane = 1});
    codec_word_add(b, &(struct codec_word){.value = 0x1, .bits = 12, .lane = 0});
    codec_word_add(b, &(struct codec_word){.value = 0xEF, .bits = 12, .lane = 1});

    for (const char *p = "ok\n\"\x01"; *p; p++) {
        codec_word_add(x, &(struct codec_word){.value = (uint8_t)*p, .bits = 8});
    }

    f = open_memstream(&buf, &len);
    if (!f) {
        CHECK(false);
        return;
    }
    output_text(f, a, &opt);
    output_text(f, b, &opt);
    output_text(f, x, &opt);
    opt.level = 1;
    output_text(f, a, &opt);
    fflush(f);
    CHECK(buf && !strcmp(buf,
        "  1.500000  dev   0x44  2 bytes  [loss, odd]\n"
        "             > 89\n"
        "             < 60 D5\n"
        "               = 0x10\n"
        "  2.000000  dev   0x01\n"
        "             mosi  9F 001\n"
        "             miso  FF 0EF\n"
        "  2.500000  dev\n"
        "             > \"ok\\n\"\n"
        "               \"\\\"\\x01\"\n"
        "  1.500000  dev   0x44  2 bytes  [loss, odd]\n"
        "             > 89\n"
        "             < 60 D5\n"
        "               = 0x10\n"
        "               - 0x11\n"));
    if (buf && strncmp(buf, "  1.500000", 10)) {
        fprintf(stderr, "%s", buf);
    }
    fclose(f);
    free(buf);
    buf = NULL;

    f = open_memstream(&buf, &len);
    opt.level = 0;
    output_jsonl(f, a, &opt);
    fflush(f);
    CHECK(buf && !strcmp(buf,
        "{\"decoder\":\"dev\",\"kind\":\"t\",\"id\":7,\"start\":1500,\"end\":1510,"
        "\"start_s\":1.5,\"end_s\":1.51,\"flags\":[\"loss\",\"odd\"],"
        "\"fields\":{\"addr\":68,\"len\":2,\"hidden\":\"x\\\"y\"},"
        "\"words\":[{\"lane\":\"mosi\",\"value\":137,\"bits\":8,\"start\":0,\"end\":0},"
        "{\"lane\":\"miso\",\"value\":96,\"bits\":8,\"start\":0,\"end\":0},"
        "{\"lane\":\"miso\",\"value\":213,\"bits\":8,\"start\":0,\"end\":0,"
        "\"flags\":[\"error\"]}],"
        "\"children\":[{\"decoder\":\"chip\",\"kind\":\"t\",\"id\":8,\"start\":1500,"
        "\"end\":1500,\"start_s\":1.5,\"end_s\":1.5,\"fields\":{\"addr\":16}}]}\n"));
    if (buf && buf[0] != '{') {
        fprintf(stderr, "%s", buf);
    }
    fclose(f);
    free(buf);

    codec_item_free(a);
    codec_item_free(b);
    codec_item_free(x);
}


/*
 * The pretty text layout: a transaction a chip read gives the chip's readings only, one
 * nothing read its header alone, a text kind its words alone, a kind with fields only its
 * one line.
 */
static void test_pretty(void)
{
    static const struct codec_field_desc fields[] = {
        {.name = "addr", .type = CODEC_UINT, .flags = CODEC_FIELD_HEADER | CODEC_FIELD_HEX,
         .digits = 2, .width = 4},
        {.name = "len", .type = CODEC_UINT, .flags = CODEC_FIELD_HEADER},
    };
    static const struct codec_field_desc chip_fields[] = {
        {.name = "chip", .type = CODEC_TEXT, .flags = CODEC_FIELD_HEADER, .width = 6},
        {.name = "op", .type = CODEC_TEXT, .flags = CODEC_FIELD_HEADER, .width = 7},
        {.name = "temp", .type = CODEC_REAL, .flags = CODEC_FIELD_HEADER, .digits = 2,
         .unit = "C"},
    };
    static const struct codec_field_desc act_fields[] = {
        {.name = "records", .type = CODEC_UINT, .flags = CODEC_FIELD_HEADER, .unit = "records"},
    };
    static const char *const lanes[] = {"master", "target"};
    static const char *const uart_lanes[] = {"tx", "rx"};
    static const char *const chip_flags[] = {"crc", NULL};
    const struct codec_kind trans = {.name = "transaction", .fields = fields, .field_count = 2,
                                     .lanes = lanes, .lane_count = 2,
                                     .lane_mode = CODEC_LANES_TURNS};
    const struct codec_kind reading = {.name = "reading", .fields = chip_fields,
                                       .field_count = 3, .item_flags = chip_flags};
    const struct codec_kind burst = {.name = "burst", .lanes = uart_lanes, .lane_count = 2,
                                     .flags = CODEC_KIND_TEXT};
    const struct codec_kind packet = {.name = "packet", .fields = act_fields,
                                      .field_count = 1};
    struct codec_dec i2c = {.name = "i2c"};
    struct codec_dec sht = {.name = "sht3x"};
    struct codec_dec uart = {.name = "uart"};
    struct codec_dec act = {.name = "activity"};
    static const char *const want =
        "  1.001000  SHT3x     command\n"
        "  1.500000  SHT3x     read     21.17 C  [crc]\n"
        "  2.000000  i2c       0x43  1\n"
        "  3.000000  uart      [loss] < \"hi\\n\"\n"
        "                               \"yo\"\n"
        "  3.000000  uart      [loss] < 68 69 0A 79 6F\n"
        "  4.000000  activity  12 records\n";
    struct output_options opt = {.pretty = true, .level = 9, .name_width = 8, .rate = 1000};
    struct codec_item *t = codec_item_new(&i2c, &trans, 1000);
    struct codec_item *u = codec_item_new(&i2c, &trans, 2000);
    struct codec_item *b = codec_item_new(&uart, &burst, 3000);
    struct codec_item *k = codec_item_new(&act, &packet, 4000);
    struct codec_item *ch = NULL;
    char *buf = NULL;
    size_t len = 0;
    FILE *f = NULL;

    if (!t || !u || !b || !k) {
        CHECK(false);
        return;
    }
    // Read by a chip: a segment of its own and two readings, the second with a flag.
    codec_set_uint(t, 0, 0x44);
    codec_set_uint(t, 1, 2);
    codec_word_add(t, &(struct codec_word){.value = 0x88, .bits = 8, .lane = 0});
    codec_word_add(t, &(struct codec_word){.value = 0x60, .bits = 8, .lane = 1});
    ch = codec_item_child(t, &i2c, &trans, 1000);
    codec_set_uint(ch, 0, 0x44);
    ch = codec_item_child(t, &sht, &reading, 1001);
    codec_set_text(ch, 0, "SHT3x");
    codec_set_text(ch, 1, "command");
    ch = codec_item_child(t, &sht, &reading, 1500);
    codec_set_text(ch, 0, "SHT3x");
    codec_set_text(ch, 1, "read");
    codec_set_real(ch, 2, 21.17);
    ch->flags = 1U << CODEC_ITEM_KIND_SHIFT;

    // Read by nothing: its header, words and segment left out.
    codec_set_uint(u, 0, 0x43);
    codec_set_uint(u, 1, 1);
    codec_word_add(u, &(struct codec_word){.value = 0x86, .bits = 8, .lane = 0});
    ch = codec_item_child(u, &i2c, &trans, 2000);
    codec_set_uint(ch, 0, 0x43);

    // Text: its words alone, the flags before them; a line feed goes on below.
    b->flags = CODEC_ITEM_LOSS;
    for (const char *p = "hi\nyo"; *p; p++) {
        codec_word_add(b, &(struct codec_word){.value = (uint8_t)*p, .bits = 8, .lane = 1});
    }

    codec_set_uint(k, 0, 12);

    f = open_memstream(&buf, &len);
    if (!f) {
        CHECK(false);
        return;
    }
    output_text(f, t, &opt);
    output_text(f, u, &opt);
    output_text(f, b, &opt);
    opt.hex = true;
    output_text(f, b, &opt);
    output_text(f, k, &opt);
    fflush(f);
    CHECK(buf && !strcmp(buf, want));
    if (buf && strcmp(buf, want)) {
        fprintf(stderr, "%s", buf);
    }
    fclose(f);
    free(buf);

    codec_item_free(t);
    codec_item_free(u);
    codec_item_free(b);
    codec_item_free(k);
}


static void test_args(void)
{
    struct sim s;
    struct codec *c = NULL;
    struct codec_dec *d = NULL;
    char error[CODEC_ERROR_LEN];

    sim_init(&s);
    CHECK(codec_new(&c, &s.info, 0) == 0);
    if (!c) {
        return;
    }
    CHECK(codec_add(c, codec_find("i2c"), "scl=0", NULL, NULL, error, sizeof(error)) < 0);
    CHECK(codec_add(c, codec_find("i2c"), "scl=0,sda=9", NULL, NULL, error, sizeof(error)) < 0);
    CHECK(codec_add(c, codec_find("i2c"), "scl=0,sda=1,x=2", NULL, NULL, error,
                    sizeof(error)) < 0);
    CHECK(codec_add(c, codec_find("i2c"), "scl0", NULL, NULL, error, sizeof(error)) < 0);
    CHECK(codec_add(c, codec_find("uart"), "tx=2", NULL, NULL, error, sizeof(error)) < 0);
    CHECK(codec_add(c, codec_find("i2c"), "scl=0,sda=1", NULL, &d, error, sizeof(error)) == 0);
    CHECK(d && !strcmp(d->name, "i2c"));
    CHECK(codec_add(c, codec_find("i2c"), "scl=0,sda=1", NULL, &d, error, sizeof(error)) == 0);
    CHECK(d && !strcmp(d->name, "i2c#2"));
    CHECK(codec_add(c, codec_find("i2c"), "scl=0,sda=1,name=bus", NULL, &d, error,
                    sizeof(error)) == 0);
    CHECK(d && !strcmp(d->name, "bus"));
    CHECK(codec_count(c) == 3);
    codec_free(c);
}


/*
 * Records with levels and advances in place of heartbeats, at steps that are not the
 * grid: the same items, written out the same, as with heartbeats.
 */
static void test_advance(void)
{
    static const uint64_t steps[] = {1, 37, 1000};
    struct sim s;
    struct got want = {0};
    struct codec *c = NULL;
    struct output_options opt = {.level = 9, .rate = RATE, .name_width = 6};
    char *a = NULL;
    size_t a_len = 0;
    FILE *fa = NULL;

    sim_init(&s);
    i2c_start(&s);
    i2c_byte(&s, 0x88, 0);
    i2c_byte(&s, 0x24, 1);
    i2c_stop(&s);
    s.t += 3 * C;
    uart_byte(&s, TX, 'h', false);
    uart_byte(&s, TX, 'i', true);
    s.t += 3 * C;
    i2c_start(&s);
    i2c_byte(&s, 0x89, 0);
    i2c_byte(&s, 0x55, 1);
    i2c_stop(&s);

    c = session(&s, "i2c", "scl=0,sda=1", 0);
    CHECK(codec_add(c, codec_find("uart"), "tx=2,baud=100000", NULL, NULL, NULL, 0) == 0);
    run(&s, c, &want, true);
    CHECK(want.count == 3);
    fa = open_memstream(&a, &a_len);
    for (size_t i = 0; i < want.count; i++) {
        output_text(fa, want.item[i], &opt);
    }
    fclose(fa);
    got_free(&want);
    codec_free(c);

    for (size_t k = 0; k < sizeof(steps) / sizeof(steps[0]); k++) {
        struct got g = {0};
        uint8_t lv[CHANNELS] = {1, 1, 1, 1};
        uint64_t end = s.t + 200 * C;
        struct codec_item *it = NULL;
        char *b = NULL;
        size_t b_len = 0;
        FILE *fb = NULL;
        size_t r = 0;

        c = session(&s, "i2c", "scl=0,sda=1", 0);
        CHECK(codec_add(c, codec_find("uart"), "tx=2,baud=100000", NULL, NULL, NULL, 0) == 0);
        for (uint64_t t = 0; t <= end; t += steps[k]) {
            while (r < s.count && s.rec[r].tick <= t) {
                CHECK(codec_record(c, &s.rec[r]) == 0);
                lv[s.rec[r].channel] = s.rec[r].level;
                r++;
            }
            for (unsigned int ch = 0; ch < CHANNELS; ch++) {
                CHECK(codec_line_level(c, t, (uint16_t)ch, lv[ch]) == 0);
            }
            CHECK(codec_advance(c, t) == 0);
            while ((it = codec_next(c)) && g.count < ITEMS_MAX) {
                g.item[g.count++] = it;
            }
        }
        codec_finish(c);
        while ((it = codec_next(c)) && g.count < ITEMS_MAX) {
            g.item[g.count++] = it;
        }
        CHECK(codec_error(c) == 0);
        CHECK(g.count == 3);

        fb = open_memstream(&b, &b_len);
        for (size_t i = 0; i < g.count; i++) {
            output_text(fb, g.item[i], &opt);
        }
        fclose(fb);
        CHECK(a_len == b_len && a && b && !memcmp(a, b, a_len));
        free(b);
        got_free(&g);
        codec_free(c);
    }
    free(a);
}
