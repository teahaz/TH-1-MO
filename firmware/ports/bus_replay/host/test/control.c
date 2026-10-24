/*
 * Control messages and the capture info: a hello as the bench device sends it, a
 * configuration write and reply, a loss report, and the spec's mapping examples.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "capture/control.h"
#include "capture/info.h"
#include "common/capture_info.h"
#include "test/test.h"


struct bytes {
    uint8_t data[1024];
    size_t len;
};


static void put(struct bytes *b, uint32_t v, size_t n);
static void put_str(struct bytes *b, const char *s);
static void build_hello(struct bytes *b);
static void test_hello(void);
static void test_hello_short(void);
static void test_config(void);
static void test_loss_report(void);
static void test_mapping(void);


int main(void)
{
    test_hello();
    test_hello_short();
    test_config();
    test_loss_report();
    test_mapping();

    return test_result(__FILE__);
}


static void put(struct bytes *b, uint32_t v, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        b->data[b->len++] = (uint8_t)(v >> (8 * i));
    }
}


static void put_str(struct bytes *b, const char *s)
{
    size_t n = strlen(s);

    put(b, (uint32_t)n, 1);
    memcpy(&b->data[b->len], s, n);
    b->len += n;
}


// A 6-channel device on one data channel, with the five core registers.
static void build_hello(struct bytes *b)
{
    static const char *const names[] = {"CH1", "CH2", "CH3", "CH4", "CH5", "CH6"};

    b->len = 0;
    put(b, MSG_HELLO, 1);
    put(b, 1, 1);
    put(b, 2, 1);
    put_str(b, "MO");
    put_str(b, "0.3.0");
    put(b, 4, 1);
    put(b, 0xDDCCBBAA, 4);
    put(b, 32768, 4);
    put(b, 6, 2);
    put(b, 1, 1);
    for (size_t i = 0; i < 6; i++) {
        put_str(b, names[i]);
    }
    put(b, 5, 1);

    // capture: bool, list { on }, off.
    put(b, REG_CAPTURE, 1);
    put(b, REG_TYPE_BOOL, 1);
    put(b, REG_WRITABLE, 1);
    put_str(b, "capture");
    put_str(b, "");
    put(b, REG_KIND_LIST, 1);
    put(b, 1, 2);
    put(b, 1, 1);
    put(b, 0, 1);

    // sample rate: u32, divider 64 MHz / 128 to 65535, 500 kHz.
    put(b, REG_SAMPLE_RATE, 1);
    put(b, REG_TYPE_U32, 1);
    put(b, REG_WRITABLE, 1);
    put_str(b, "sample rate");
    put_str(b, "Hz");
    put(b, REG_KIND_DIVIDER, 1);
    put(b, 64000000, 4);
    put(b, 128, 4);
    put(b, 65535, 4);
    put(b, 500000, 4);

    // C: u32, range 2 to 2^27 - 1, 1000.
    put(b, REG_C, 1);
    put(b, REG_TYPE_U32, 1);
    put(b, REG_WRITABLE, 1);
    put_str(b, "C");
    put_str(b, "ticks");
    put(b, REG_KIND_RANGE, 1);
    put(b, 2, 4);
    put(b, (1U << 27) - 1, 4);
    put(b, 1000, 4);

    // enabled: channel set, any, channels 0 to 3.
    put(b, REG_ENABLED, 1);
    put(b, REG_TYPE_CHANNEL_SET, 1);
    put(b, REG_WRITABLE, 1);
    put_str(b, "enabled");
    put_str(b, "");
    put(b, REG_KIND_ANY, 1);
    put(b, 0x0F, 1);

    // split: channel set, any, empty.
    put(b, REG_SPLIT, 1);
    put(b, REG_TYPE_CHANNEL_SET, 1);
    put(b, REG_WRITABLE, 1);
    put_str(b, "split");
    put_str(b, "");
    put(b, REG_KIND_ANY, 1);
    put(b, 0x00, 1);
}


static void test_hello(void)
{
    struct bytes b;
    struct hello h;
    struct capture_info info;
    const struct reg *r = NULL;
    uint32_t base = 0;
    uint32_t min = 0;
    uint32_t max = 0;

    build_hello(&b);
    CHECK(hello_parse(b.data, b.len, &h) == 0);
    CHECK(h.version_major == 1 && h.version_minor == 2);
    CHECK(!strcmp(h.model, "MO") && !strcmp(h.firmware, "0.3.0"));
    CHECK(h.uid_len == 4 && h.uid[0] == 0xAA && h.uid[3] == 0xDD);
    CHECK(h.buffer == 32768 && h.channels == 6 && h.data_channels == 1);
    CHECK(!strcmp(h.names[0], "CH1") && !strcmp(h.names[5], "CH6"));
    CHECK(h.reg_count == 5);

    r = hello_reg(&h, REG_SAMPLE_RATE);
    CHECK(r && !strcmp(r->unit, "Hz") && reg_u32(r) == 500000);
    CHECK(r && reg_divider(r, &base, &min, &max) == 0);
    CHECK(base == 64000000 && min == 128 && max == 65535);
    CHECK(r && reg_range(r, &min, &max) == -ENOENT);
    r = hello_reg(&h, REG_C);
    CHECK(r && reg_u32(r) == 1000 && reg_range(r, &min, &max) == 0);
    CHECK(min == 2 && max == (1U << 27) - 1);
    r = hello_reg(&h, REG_CAPTURE);
    CHECK(r && r->kind == REG_KIND_LIST && r->allowed_len == 3 && reg_u32(r) == 0);
    r = hello_reg(&h, REG_ENABLED);
    CHECK(r && r->size == 1 && r->value[0] == 0x0F);
    CHECK(hello_reg(&h, 0x80) == NULL);

    // The device's defaults: channels 0 to 3 on one data channel, 4 per block, t = 2, and
    // (2 * 1000 + 1) * 4 - 1 = 8003 needs two bytes.
    CHECK(capture_info_from_hello(&info, &h) == 0);
    CHECK(info.rate == 500000 && info.c == 1000 && info.n == 4);
    CHECK(info.enabled[0] == 0 && info.enabled[3] == 3);
    CHECK(info.block[0] == 0 && info.block[1] == 4);
    CHECK(info.tag[0] == 2 && info.width[0] == 2);
    CHECK(!strcmp(info.names[2], "CH3"));
    capture_info_free(&info);

    hello_free(&h);
}


// Every cut-off hello is rejected, and a register type the spec does not define is too.
static void test_hello_short(void)
{
    struct bytes b;
    struct hello h;
    bool all = true;

    build_hello(&b);
    for (size_t len = 0; len < b.len; len++) {
        all = all && hello_parse(b.data, len, &h) == -EPROTO;
    }
    CHECK(all);

    // The capture register's type byte: after type, versions, "MO", "0.3.0", uid, buffer,
    // counts, six names and the register count.
    b.data[1 + 2 + 3 + 6 + 5 + 4 + 3 + 6 * 4 + 1 + 1] = 0x07;
    CHECK(hello_parse(b.data, b.len, &h) == -EPROTO);
}


static void test_config(void)
{
    const uint8_t rate[4] = {0x20, 0xA1, 0x07, 0x00};
    const uint8_t reply[] = {MSG_CONFIG_REPLY, REG_C, 0x04, 3, 'l', 'o', 'w', 0xE8, 0x03, 0, 0};
    uint8_t msg[8];
    struct config_reply rep;

    CHECK(config_msg(msg, sizeof(msg), REG_SAMPLE_RATE, rate, 4) == 6);
    CHECK(msg[0] == MSG_CONFIG && msg[1] == REG_SAMPLE_RATE && msg[2] == 0x20 && msg[5] == 0);
    CHECK(config_msg(msg, 5, REG_SAMPLE_RATE, rate, 4) == -ENOSPC);

    CHECK(config_reply_parse(reply, sizeof(reply), &rep) == 0);
    CHECK(rep.reg == REG_C && rep.status == 0x04 && !strcmp(rep.text, "low"));
    CHECK(rep.value_len == 4 && rep.value[0] == 0xE8 && rep.value[1] == 0x03);
    CHECK(config_reply_parse(reply, 5, &rep) == -EPROTO);
    CHECK(!strcmp(config_status_text(0x07), "C too short"));
    CHECK(!strcmp(config_status_text(0x90), "device-specific"));
}


static void test_loss_report(void)
{
    struct bytes b = {.len = 0};
    struct loss_report rep;

    put(&b, MSG_LOSS_REPORT, 1);
    put(&b, 3, 4);
    put(&b, 0xFFFFFFFE, 4);
    put(&b, 999, 4);
    put(&b, 1, 4);
    put(&b, 7, 4);
    put(&b, 12, 4);
    put(&b, 0, 4);

    CHECK(loss_report_parse(b.data, b.len, 2, &rep) == 0);
    CHECK(rep.seq == 3 && rep.start_grid == 0xFFFFFFFE && rep.start_off == 999);
    CHECK(rep.end_grid == 1 && rep.end_off == 7);
    CHECK(loss_report_lost(&rep, 0) == 12 && loss_report_lost(&rep, 1) == 0);
    CHECK(loss_report_parse(b.data, b.len, 3, &rep) == -EPROTO);
}


// The spec's example (Mapping): channels 0, 1, 2, 3, 4, 6, 7 enabled, D = 3.
static void test_mapping(void)
{
    static const struct {
        uint8_t split;
        uint16_t block[4];
    } cases[] = {
        {0x00, {0, 3, 5, 7}},
        {0x44, {0, 2, 5, 7}},
        {0x40, {0, 5, 7, 7}},
    };
    char *names[8] = {"0", "1", "2", "3", "4", "5", "6", "7"};
    struct hello h = {.channels = 8, .data_channels = 3, .names = names};
    const uint8_t enabled = 0xDF;
    const uint8_t too_many = 0x46;
    struct capture_info info;

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        CHECK(capture_info_init(&info, &h, 1000, 100, &enabled, &cases[i].split) == 0);
        CHECK(info.n == 7);
        CHECK(!memcmp(info.block, cases[i].block, sizeof(cases[i].block)));
        capture_info_free(&info);
    }

    // Data channel 2 of the split {6} case is empty: no tag bits.
    CHECK(capture_info_init(&info, &h, 1000, 100, &enabled, &cases[2].split) == 0);
    CHECK(info.tag[0] == 3 && info.tag[1] == 1 && info.tag[2] == 0);
    capture_info_free(&info);

    CHECK(capture_info_init(&info, &h, 1000, 100, &enabled, &too_many) == -EINVAL);
    h.channels = 6;
    CHECK(capture_info_init(&info, &h, 1000, 100, &enabled, NULL) == -EINVAL);
}
