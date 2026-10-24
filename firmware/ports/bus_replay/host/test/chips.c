/*
 * Chip-level decoders on hand-built I2C transactions: the INA219 (register pointer device)
 * and the SHT3x (command device), their binding, and how they show in the text output.
 *
 * Transactions are built as the i2c decoder builds them (codec/i2c/i2c.h) and handed to the
 * session as its items, so the chip decoders see them as they would from records. Fields
 * are looked up by name, as the outputs show them.
 */

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
#include "codec/output.h"
#include "common/capture_info.h"
#include "common/record.h"
#include "test/test.h"


#define RATE 1000000
#define CHANNELS 2
#define ITEMS_MAX 32
#define BYTES_MAX 16
#define TEXT_MAX 4096

/*
 * One segment of a transaction: its address, direction, data bytes, and whether the address
 * byte was not acknowledged. W() and R() write and read the bytes given, NAKED() is an
 * address nobody acknowledged, TXN() a transaction of the segments given.
 */
struct seg {
    uint8_t addr;
    bool read;
    uint8_t n;
    uint8_t data[BYTES_MAX];
    bool nak;
};

#define W(a, ...) {.addr = (a), .n = sizeof((uint8_t[]){0, ##__VA_ARGS__}) - 1,              \
                   .data = {__VA_ARGS__}}
#define R(a, ...) {.addr = (a), .read = true, .n = sizeof((uint8_t[]){__VA_ARGS__}),          \
                   .data = {__VA_ARGS__}}
#define NAKED(a) {.addr = (a), .nak = true}
#define TXN(b, ...) txn((b), (struct seg[]){__VA_ARGS__},                                     \
                        sizeof((struct seg[]){__VA_ARGS__}) / sizeof(struct seg))

struct bench {
    struct capture_info info;
    char *names[CHANNELS];
    uint16_t enabled[CHANNELS];
    uint16_t block[2];
    uint8_t tag[1];
    uint8_t width[1];
    uint8_t set[1];

    struct codec *codec;
    struct codec_dec *i2c;
    uint64_t t;
    struct codec_item *item[ITEMS_MAX];
    size_t count;
};


static void bench_init(struct bench *b);
static void bench_free(struct bench *b);
static bool add(struct bench *b, const char *name, const char *args);
static void txn(struct bench *b, const struct seg *segs, size_t n);
static void finish(struct bench *b);
static const struct codec_item *reading(const struct bench *b, size_t t, size_t k);
static const struct codec_value *val(const struct codec_item *it, const char *name);
static bool text_is(const struct codec_item *it, const char *name, const char *want);
static bool real_is(const struct codec_item *it, const char *name, double want);
static bool flag(const struct codec_item *it, const char *name);
static void test_crc(void);
static void test_ina219_read(void);
static void test_ina219_pointer(void);
static void test_ina219_config(void);
static void test_ina219_calibration(void);
static void test_ina219_odd(void);
static void test_ina219_loss(void);
static void test_sht3x_fetch(void);
static void test_sht3x_crc(void);
static void test_sht3x_separate(void);
static void test_sht3x_odd(void);
static void test_binding(void);
static void test_text(void);


int main(void)
{
    test_crc();
    test_ina219_read();
    test_ina219_pointer();
    test_ina219_config();
    test_ina219_calibration();
    test_ina219_odd();
    test_ina219_loss();
    test_sht3x_fetch();
    test_sht3x_crc();
    test_sht3x_separate();
    test_sht3x_odd();
    test_binding();
    test_text();

    return test_result(__FILE__);
}


static void bench_init(struct bench *b)
{
    static char *const names[CHANNELS] = {"SCL", "SDA"};
    char error[CODEC_ERROR_LEN];

    memset(b, 0, sizeof(*b));
    for (unsigned int i = 0; i < CHANNELS; i++) {
        b->names[i] = names[i];
        b->enabled[i] = (uint16_t)i;
    }
    b->block[1] = CHANNELS;
    b->set[0] = (1U << CHANNELS) - 1;
    b->info = (struct capture_info){
        .rate = RATE, .c = 100, .channels = CHANNELS, .data_channels = 1, .names = b->names,
        .set_size = 1, .enabled_set = b->set, .n = CHANNELS, .enabled = b->enabled,
        .block = b->block, .tag = b->tag, .width = b->width,
    };
    b->t = 1000;
    CHECK(codec_new(&b->codec, &b->info, 0) == 0);
    CHECK(codec_add(b->codec, codec_find("i2c"), "scl=0,sda=1", NULL, &b->i2c, error,
                    sizeof(error)) == 0);
}


static void bench_free(struct bench *b)
{
    for (size_t i = 0; i < b->count; i++) {
        codec_item_free(b->item[i]);
    }
    codec_free(b->codec);
}


static bool add(struct bench *b, const char *name, const char *args)
{
    char error[CODEC_ERROR_LEN];

    return codec_add(b->codec, codec_find(name), args, NULL, NULL, error, sizeof(error)) == 0;
}


// A transaction of the given segments, emitted by the i2c decoder; 100 ticks a byte.
static void txn(struct bench *b, const struct seg *segs, size_t n)
{
    struct codec_item *t = codec_item_new(b->i2c, &i2c_transaction, b->t);

    CHECK(t != NULL);
    for (size_t s = 0; s < n && t; s++) {
        struct i2c_segment *sp = calloc(1, sizeof(*sp));
        struct codec_item *seg = codec_item_child(t, b->i2c, &i2c_segment, b->t);
        struct codec_word w = {.bits = 8, .lane = I2C_MASTER, .flags = CODEC_WORD_BREAK};

        CHECK(sp && seg);
        if (!sp || !seg) {
            free(sp);
            break;
        }
        seg->detail = 1;
        seg->payload = sp;
        seg->payload_free = free;
        sp->has_addr = true;
        sp->addr = segs[s].addr;
        sp->read = segs[s].read;
        sp->first = t->word_count;
        sp->count = 1U + segs[s].n;

        w.start = b->t;
        w.end = b->t + 99;
        w.value = (uint64_t)(segs[s].addr << 1 | segs[s].read);
        w.flags |= segs[s].nak ? I2C_WORD_NAK : 0;
        CHECK(codec_word_add(t, &w) == 0);
        b->t += 100;
        for (size_t i = 0; i < segs[s].n; i++) {
            struct codec_word dw = {.bits = 8, .start = b->t, .end = b->t + 99,
                                    .value = segs[s].data[i],
                                    .lane = segs[s].read ? I2C_TARGET : I2C_MASTER};

            CHECK(codec_word_add(t, &dw) == 0);
            b->t += 100;
        }
        seg->end = b->t - 1;
    }
    if (t) {
        t->end = b->t - 1;
        codec_emit(b->i2c, t);
    }
    b->t += 1000;
}


static void finish(struct bench *b)
{
    struct codec_item *it = NULL;

    codec_finish(b->codec);
    while ((it = codec_next(b->codec)) && b->count < ITEMS_MAX) {
        b->item[b->count++] = it;
    }
    CHECK(codec_error(b->codec) == 0);
}


// The k-th chip reading (a child not from the i2c decoder) of transaction t, NULL if none.
static const struct codec_item *reading(const struct bench *b, size_t t, size_t k)
{
    if (t >= b->count) {
        return NULL;
    }
    for (const struct codec_item *c = b->item[t]->child; c; c = c->next) {
        if (c->dec != b->i2c && k-- == 0) {
            return c;
        }
    }

    return NULL;
}


// The value of the field called name, NULL if the item has no such field set.
static const struct codec_value *val(const struct codec_item *it, const char *name)
{
    for (unsigned int f = 0; it && f < it->kind->field_count; f++) {
        if (!strcmp(it->kind->fields[f].name, name)) {
            return it->values[f].set ? &it->values[f] : NULL;
        }
    }

    return NULL;
}


static bool text_is(const struct codec_item *it, const char *name, const char *want)
{
    const struct codec_value *v = val(it, name);

    return v && !strcmp(v->s, want);
}


static bool real_is(const struct codec_item *it, const char *name, double want)
{
    const struct codec_value *v = val(it, name);

    return v && v->r - want < 1e-9 && want - v->r < 1e-9;
}


static bool flag(const struct codec_item *it, const char *name)
{
    for (unsigned int bit = 0; it && bit < 32; bit++) {
        const char *n = (it->flags >> bit) & 1 ? codec_flag_name(it->kind, bit) : NULL;

        if (n && !strcmp(n, name)) {
            return true;
        }
    }

    return false;
}


static void test_crc(void)
{
    // Sensirion's example (0xBEEF, CRC 0x92), and a temperature word from a real capture.
    CHECK(chip_crc8((const uint8_t[]){0xBE, 0xEF}, 2) == 0x92);
    CHECK(chip_crc8((const uint8_t[]){0x60, 0xCA}, 2) == 0x48);
    CHECK(chip_crc8((const uint8_t[]){0xA3, 0x63}, 2) == 0xBB);
}


// A pointer write and a read in one transaction.
static void test_ina219_read(void)
{
    struct bench b;
    const struct codec_item *r = NULL;

    bench_init(&b);
    CHECK(add(&b, "ina219", "addr=0x43,shunt=0.01,cal=4096"));
    TXN(&b, W(0x43, 0x02), R(0x43, 0x1F, 0x9A));
    TXN(&b, W(0x43, 0x04), R(0x43, 0x00, 0x05));
    TXN(&b, W(0x43, 0x03), R(0x43, 0x00, 0x02));
    TXN(&b, W(0x43, 0x04), R(0x43, 0xFF, 0xFB));
    TXN(&b, W(0x43, 0x01), R(0x43, 0xFF, 0x38));
    finish(&b);
    CHECK(b.count == 5);

    r = reading(&b, 0, 0);
    CHECK(text_is(r, "chip", "INA219") && text_is(r, "op", "read") && text_is(r, "reg", "bus"));
    CHECK(val(r, "raw") && val(r, "raw")->u == 0x1F9A);
    CHECK(real_is(r, "bus", 1011 * 0.004));
    CHECK(val(r, "cnvr") && val(r, "cnvr")->b && val(r, "ovf") && !val(r, "ovf")->b);
    // One reading, spanning the pointer write and the read.
    CHECK(r && r->start == b.item[0]->start && r->end == b.item[0]->end);
    CHECK(r && r->flags == 0 && !reading(&b, 0, 1));

    // 1 mA and 20 mW a bit with calibration 4096 and 10 mohm.
    CHECK(text_is(reading(&b, 1, 0), "reg", "current"));
    CHECK(real_is(reading(&b, 1, 0), "current", 0.005));
    CHECK(real_is(reading(&b, 1, 0), "current_lsb", 0.001));
    CHECK(real_is(reading(&b, 2, 0), "power", 0.04));
    CHECK(real_is(reading(&b, 3, 0), "current", -0.005));
    CHECK(text_is(reading(&b, 4, 0), "reg", "shunt"));
    CHECK(real_is(reading(&b, 4, 0), "shunt", -2.0));
    bench_free(&b);
}


// A pointer write alone, then reads that leave the pointer out.
static void test_ina219_pointer(void)
{
    struct bench b;
    const struct codec_item *r = NULL;

    bench_init(&b);
    CHECK(add(&b, "ina219", "addr=0x43"));
    TXN(&b, R(0x43, 0x12, 0x34));
    TXN(&b, W(0x43, 0x02));
    TXN(&b, R(0x43, 0x1F, 0x92));
    TXN(&b, R(0x43, 0x1F, 0x90));
    TXN(&b, W(0x43, 0x04));
    TXN(&b, R(0x43, 0x00, 0x06));
    finish(&b);
    CHECK(b.count == 6);

    // No pointer seen yet: the value is shown, the register is not known.
    r = reading(&b, 0, 0);
    CHECK(text_is(r, "op", "read") && !val(r, "reg"));
    CHECK(flag(r, "unknown") && text_is(r, "data", "12 34"));
    CHECK(text_is(reading(&b, 1, 0), "op", "pointer"));
    CHECK(text_is(reading(&b, 1, 0), "reg", "bus"));
    CHECK(text_is(reading(&b, 2, 0), "reg", "bus"));
    CHECK(real_is(reading(&b, 2, 0), "bus", 1010 * 0.004));
    r = reading(&b, 3, 0);
    CHECK(text_is(r, "reg", "bus") && val(r, "cnvr") && !val(r, "cnvr")->b);
    // No calibration and no shunt: the count, flagged.
    r = reading(&b, 5, 0);
    CHECK(text_is(r, "reg", "current") && val(r, "count") && val(r, "count")->i == 6);
    CHECK(flag(r, "uncalibrated") && !val(r, "current") && !flag(r, "unknown"));
    bench_free(&b);
}


// The configuration register, written and read.
static void test_ina219_config(void)
{
    struct bench b;
    const struct codec_item *r = NULL;

    bench_init(&b);
    CHECK(add(&b, "ina219", "addr=0x43"));
    TXN(&b, W(0x43, 0x00, 0x39, 0xEB));
    TXN(&b, R(0x43, 0x39, 0x9F));
    finish(&b);
    CHECK(b.count == 2);

    // 32 V, /8, 12 bit bus, 32 samples shunt, shunt and bus triggered.
    r = reading(&b, 0, 0);
    CHECK(text_is(r, "op", "write") && text_is(r, "reg", "config"));
    CHECK(val(r, "range") && val(r, "range")->u == 32 && text_is(r, "pga", "/8 320 mV"));
    CHECK(text_is(r, "bus_adc", "bus 12 bit") && text_is(r, "shunt_adc", "shunt 32 samples"));
    CHECK(text_is(r, "mode", "shunt+bus triggered") && val(r, "reset") && !val(r, "reset")->b);
    // The power-on default, read back from the pointer the write left.
    r = reading(&b, 1, 0);
    CHECK(text_is(r, "op", "read") && text_is(r, "reg", "config"));
    CHECK(text_is(r, "shunt_adc", "shunt 12 bit") && text_is(r, "mode", "shunt+bus continuous"));
    bench_free(&b);
}


// A calibration write in the capture is used over cal=.
static void test_ina219_calibration(void)
{
    struct bench b;

    bench_init(&b);
    CHECK(add(&b, "ina219", "addr=0x40,shunt=0.1,cal=8192"));
    TXN(&b, W(0x40, 0x04), R(0x40, 0x00, 0x0A));
    TXN(&b, W(0x40, 0x05, 0x10, 0x00));
    TXN(&b, W(0x40, 0x04), R(0x40, 0x00, 0x0A));
    finish(&b);
    CHECK(b.count == 3);

    // 0.04096 / (8192 * 0.1) = 50 uA a bit; then 0.04096 / (4096 * 0.1) = 100 uA.
    CHECK(real_is(reading(&b, 0, 0), "current", 10 * 0.00005));
    CHECK(text_is(reading(&b, 1, 0), "reg", "calibration"));
    CHECK(val(reading(&b, 1, 0), "calibration") &&
          val(reading(&b, 1, 0), "calibration")->u == 4096);
    CHECK(real_is(reading(&b, 2, 0), "current", 10 * 0.0001));
    bench_free(&b);
}


// Unknown registers, short and long transfers, NAKed and other addresses.
static void test_ina219_odd(void)
{
    struct bench b;
    const struct codec_item *r = NULL;

    bench_init(&b);
    CHECK(add(&b, "ina219", "addr=0x43"));
    TXN(&b, W(0x43, 0x07), R(0x43, 0x12, 0x34));
    TXN(&b, W(0x43, 0x02), R(0x43, 0x1F));
    TXN(&b, W(0x43, 0x00, 0x39));
    TXN(&b, W(0x43, 0x00, 0x39, 0x9F, 0x00));
    TXN(&b, NAKED(0x43));
    TXN(&b, W(0x44, 0x02), R(0x44, 0x1F, 0x9A));
    TXN(&b, W(0x43));
    finish(&b);
    CHECK(b.count == 7);

    r = reading(&b, 0, 0);
    CHECK(text_is(r, "reg", "0x07") && flag(r, "unknown") && text_is(r, "data", "12 34"));
    CHECK(val(r, "raw") && val(r, "raw")->u == 0x1234);
    r = reading(&b, 1, 0);
    CHECK(text_is(r, "reg", "bus") && flag(r, "short") && text_is(r, "data", "1F"));
    CHECK(!val(r, "bus") && !val(r, "raw"));
    r = reading(&b, 2, 0);
    CHECK(text_is(r, "op", "write") && flag(r, "short") && text_is(r, "data", "39"));
    r = reading(&b, 3, 0);
    CHECK(text_is(r, "op", "write") && flag(r, "long"));
    CHECK(text_is(r, "mode", "shunt+bus continuous"));
    CHECK(text_is(r, "data", "39 9F 00"));
    // A NAK on the address, and another device: no reading.
    CHECK(!reading(&b, 4, 0) && !reading(&b, 5, 0));
    CHECK(text_is(reading(&b, 6, 0), "op", "empty"));
    bench_free(&b);
}


// A loss forgets the pointer: the read after it is not known.
static void test_ina219_loss(void)
{
    struct bench b;
    const uint32_t lost[CHANNELS] = {3, 0};
    struct loss loss = {.n = CHANNELS, .lost = lost};

    bench_init(&b);
    CHECK(add(&b, "ina219", "addr=0x43"));
    TXN(&b, W(0x43, 0x02));
    loss.start = b.t;
    loss.end = b.t + 10;
    codec_loss(b.codec, &loss);
    b.t += 100;
    TXN(&b, R(0x43, 0x1F, 0x92));
    finish(&b);
    CHECK(b.count == 2);

    CHECK(flag(reading(&b, 1, 0), "unknown") && !val(reading(&b, 1, 0), "reg"));
    CHECK(!val(reading(&b, 1, 0), "bus"));
    bench_free(&b);
}


// Fetch data and its read in one transaction.
static void test_sht3x_fetch(void)
{
    struct bench b;
    const struct codec_item *r = NULL;

    bench_init(&b);
    CHECK(add(&b, "sht3x", "addr=0x44"));
    TXN(&b, W(0x44, 0xE0, 0x00), R(0x44, 0x60, 0xCA, 0x48, 0xA3, 0x63, 0xBB));
    finish(&b);
    CHECK(b.count == 1);

    r = reading(&b, 0, 0);
    CHECK(text_is(r, "chip", "SHT3x") && text_is(r, "op", "read"));
    CHECK(text_is(r, "reg", "fetch data") && val(r, "raw") && val(r, "raw")->u == 0xE000);
    CHECK(real_is(r, "temp", -45 + 175.0 * 0x60CA / 65535));
    CHECK(real_is(r, "rh", 100.0 * 0xA363 / 65535));
    CHECK(val(r, "temp_raw") && val(r, "temp_raw")->u == 0x60CA);
    CHECK(r && r->flags == 0 && !reading(&b, 0, 1));
    CHECK(r && r->start == b.item[0]->start && r->end == b.item[0]->end);
    bench_free(&b);
}


// A wrong CRC is flagged; the values are still shown.
static void test_sht3x_crc(void)
{
    struct bench b;
    const struct codec_item *r = NULL;

    bench_init(&b);
    CHECK(add(&b, "sht3x", "addr=0x44"));
    TXN(&b, W(0x44, 0xE0, 0x00), R(0x44, 0x60, 0xCA, 0x49, 0xA3, 0x63, 0xBB));
    finish(&b);

    r = reading(&b, 0, 0);
    CHECK(flag(r, "crc") && !flag(r, "short"));
    CHECK(real_is(r, "temp", -45 + 175.0 * 0x60CA / 65535));
    bench_free(&b);
}


// A command in one transaction and its read in the next; the status register.
static void test_sht3x_separate(void)
{
    struct bench b;
    const struct codec_item *r = NULL;

    bench_init(&b);
    CHECK(add(&b, "sht3x", "addr=0x45"));
    TXN(&b, W(0x45, 0x24, 0x00));
    TXN(&b, R(0x45, 0x60, 0xCA, 0x48, 0xA3, 0x63, 0xBB));
    TXN(&b, W(0x45, 0xF3, 0x2D), R(0x45, 0x80, 0x10, 0xE1));
    TXN(&b, W(0x45, 0x30, 0xA2));
    finish(&b);
    CHECK(b.count == 4);

    r = reading(&b, 0, 0);
    CHECK(text_is(r, "op", "command") && text_is(r, "reg", "single shot high"));
    CHECK(!val(r, "temp"));
    r = reading(&b, 1, 0);
    CHECK(text_is(r, "op", "read") && text_is(r, "reg", "single shot high"));
    CHECK(real_is(r, "temp", -45 + 175.0 * 0x60CA / 65535));
    r = reading(&b, 2, 0);
    CHECK(text_is(r, "reg", "status") && text_is(r, "status", "alert, reset seen"));
    CHECK(r && r->flags == 0);
    CHECK(text_is(reading(&b, 3, 0), "reg", "soft reset"));
    CHECK(text_is(reading(&b, 3, 0), "op", "command"));
    bench_free(&b);
}


// Unknown commands, reads with no command before them, short reads.
static void test_sht3x_odd(void)
{
    struct bench b;
    const struct codec_item *r = NULL;

    bench_init(&b);
    CHECK(add(&b, "sht3x", "addr=0x44"));
    TXN(&b, R(0x44, 0x60, 0xCA, 0x48));
    TXN(&b, W(0x44, 0x12, 0x34));
    TXN(&b, R(0x44, 0x60, 0xCA, 0x48));
    TXN(&b, W(0x44, 0xE0, 0x00), R(0x44, 0x60, 0xCA, 0x48, 0xA3));
    TXN(&b, W(0x44, 0xE0));
    finish(&b);
    CHECK(b.count == 5);

    r = reading(&b, 0, 0);
    CHECK(text_is(r, "op", "read") && flag(r, "unknown") && !val(r, "reg"));
    CHECK(text_is(r, "data", "60 CA 48"));
    r = reading(&b, 1, 0);
    CHECK(text_is(r, "op", "command") && text_is(r, "reg", "0x1234") && flag(r, "unknown"));
    r = reading(&b, 2, 0);
    CHECK(text_is(r, "reg", "0x1234") && flag(r, "unknown") && text_is(r, "data", "60 CA 48"));
    r = reading(&b, 3, 0);
    CHECK(text_is(r, "reg", "fetch data") && flag(r, "short") && !val(r, "temp"));
    CHECK(text_is(r, "data", "60 CA 48 A3"));
    r = reading(&b, 4, 0);
    CHECK(text_is(r, "op", "command") && flag(r, "short") && text_is(r, "data", "E0"));
    bench_free(&b);
}


// addr= and on=: required, checked against the chip, and on= picks the i2c decoder.
static void test_binding(void)
{
    struct bench b;
    char error[CODEC_ERROR_LEN];
    struct codec_dec *other = NULL;
    struct codec_dec *chip = NULL;

    bench_init(&b);
    CHECK(!add(&b, "sht3x", ""));
    CHECK(!add(&b, "sht3x", "addr=0x50"));
    CHECK(!add(&b, "sht3x", "addr=0x80"));
    CHECK(!add(&b, "sht3x", "addr=0x44,on=nothing"));
    CHECK(!add(&b, "sht3x", "addr=0x44,shunt=1"));
    CHECK(!add(&b, "ina219", "addr=0x43,shunt=-1"));
    CHECK(!add(&b, "ina219", "addr=0x43,cal=0x10000"));
    CHECK(codec_add(b.codec, codec_find("i2c"), "scl=0,sda=1,name=bus2", NULL, &other, error,
                    sizeof(error)) == 0);
    CHECK(codec_add(b.codec, codec_find("sht3x"), "addr=0x44,on=i2c", NULL, &chip, error,
                    sizeof(error)) == 0);
    CHECK(chip && chip->lower == b.i2c);
    CHECK(codec_add(b.codec, codec_find("sht3x"), "addr=0x44", NULL, &chip, error,
                    sizeof(error)) == 0);
    CHECK(chip && chip->lower == other);
    // on= is only for stacked decoders.
    CHECK(!add(&b, "i2c", "scl=0,sda=1,on=i2c"));
    bench_free(&b);

    {
        struct codec *c = NULL;

        bench_init(&b);
        CHECK(codec_new(&c, &b.info, 0) == 0);
        CHECK(codec_add(c, codec_find("ina219"), "addr=0x43", NULL, NULL, error,
                        sizeof(error)) == -22);
        codec_free(c);
        bench_free(&b);
    }
}


// A reading in the text output: an = line under its transaction, the bytes kept.
static void test_text(void)
{
    struct bench b;
    struct output_options opt = {.rate = RATE, .name_width = 3};
    char text[TEXT_MAX] = "";
    FILE *out = NULL;

    bench_init(&b);
    CHECK(add(&b, "sht3x", "addr=0x44"));
    TXN(&b, W(0x44, 0xE0, 0x00), R(0x44, 0x60, 0xCA, 0x48, 0xA3, 0x63, 0xBB));
    finish(&b);
    out = fmemopen(text, sizeof(text) - 1, "w");
    CHECK(out != NULL);
    if (out && b.count) {
        CHECK(output_text(out, b.item[0], &opt) == 0);
        fclose(out);
    }
    CHECK(strstr(text, "             > 88 E0 00\n") != NULL);
    CHECK(strstr(text, "             < 60 CA 48 A3 63 BB\n") != NULL);
    CHECK(strstr(text, "               = SHT3x   read     fetch data   21.17 C  63.82 %RH\n") !=
          NULL);
    bench_free(&b);
}
