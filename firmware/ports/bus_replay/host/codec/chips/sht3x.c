/*
 * SHT3x, a temperature and humidity sensor: a command device at 0x44 or 0x45. A
 * measurement (single shot, or fetch data in periodic mode) reads two words, temperature
 * then humidity; the status register reads one.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "codec/chips/chip.h"
#include "codec/codec.h"
#include "codec/item.h"


// Fields after the common ones.
#define SHT_TEMP (CHIP_FIELDS + 0)
#define SHT_RH (CHIP_FIELDS + 1)
#define SHT_STATUS (CHIP_FIELDS + 2)
#define SHT_TEMP_RAW (CHIP_FIELDS + 3)
#define SHT_RH_RAW (CHIP_FIELDS + 4)

#define STATUS_TEXT_MAX 128


static void measurement(void *state, struct codec_item *it, bool write, const uint16_t *w,
                        size_t n, uint32_t bad);
static void status(void *state, struct codec_item *it, bool write, const uint16_t *w,
                   size_t n, uint32_t bad);


static const struct codec_field_desc fields[] = {
    CHIP_COMMON_FIELDS,
    {.name = "temp", .type = CODEC_REAL, .flags = CODEC_FIELD_HEADER, .digits = 2,
     .unit = "C"},
    {.name = "rh", .type = CODEC_REAL, .flags = CODEC_FIELD_HEADER, .digits = 2,
     .unit = "%RH"},
    {.name = "status", .type = CODEC_TEXT, .flags = CODEC_FIELD_HEADER},
    {.name = "temp_raw", .type = CODEC_UINT, .flags = CODEC_FIELD_HEX, .digits = 4},
    {.name = "rh_raw", .type = CODEC_UINT, .flags = CODEC_FIELD_HEX, .digits = 4},
};

static const char *const flags[] = {CHIP_COMMON_FLAGS, NULL};

static const struct codec_kind kind = {
    .name = "sht3x",
    .fields = fields,
    .field_count = sizeof(fields) / sizeof(fields[0]),
    .item_flags = flags,
};

static const struct chip_cmd cmds[] = {
    {0x2C06, "single shot high stretch", 2, measurement},
    {0x2C0D, "single shot medium stretch", 2, measurement},
    {0x2C10, "single shot low stretch", 2, measurement},
    {0x2400, "single shot high", 2, measurement},
    {0x240B, "single shot medium", 2, measurement},
    {0x2416, "single shot low", 2, measurement},
    {0x2032, "periodic 0.5 mps high", 0, NULL},
    {0x2024, "periodic 0.5 mps medium", 0, NULL},
    {0x202F, "periodic 0.5 mps low", 0, NULL},
    {0x2130, "periodic 1 mps high", 0, NULL},
    {0x2126, "periodic 1 mps medium", 0, NULL},
    {0x212D, "periodic 1 mps low", 0, NULL},
    {0x2236, "periodic 2 mps high", 0, NULL},
    {0x2220, "periodic 2 mps medium", 0, NULL},
    {0x222B, "periodic 2 mps low", 0, NULL},
    {0x2334, "periodic 4 mps high", 0, NULL},
    {0x2322, "periodic 4 mps medium", 0, NULL},
    {0x2329, "periodic 4 mps low", 0, NULL},
    {0x2737, "periodic 10 mps high", 0, NULL},
    {0x2721, "periodic 10 mps medium", 0, NULL},
    {0x272A, "periodic 10 mps low", 0, NULL},
    {0xE000, "fetch data", 2, measurement},
    {0x2B32, "art", 0, NULL},
    {0x3093, "break", 0, NULL},
    {0x30A2, "soft reset", 0, NULL},
    {0x306D, "heater on", 0, NULL},
    {0x3066, "heater off", 0, NULL},
    {0xF32D, "status", 1, status},
    {0x3041, "clear status", 0, NULL},
};

static const uint8_t addrs[] = {0x44, 0x45};

static const struct chip chip = {
    .label = "SHT3x",
    .addrs = addrs,
    .addr_count = sizeof(addrs),
    .kind = &kind,
    .pattern = CHIP_COMMAND,
    .cmds = cmds,
    .cmd_count = sizeof(cmds) / sizeof(cmds[0]),
};

static const struct codec_param params[] = {
    CHIP_COMMON_PARAMS,
    {NULL, NULL, 0, 0, NULL},
};

const struct codec_decoder sht3x_decoder = {
    .name = "sht3x",
    .summary = "SHT3x temperature and humidity sensor (commands)",
    .params = params,
    .data = &chip,
    CHIP_DECODER_OPS,
};


// Temperature: -45 + 175 * raw / 65535 C. Humidity: 100 * raw / 65535 %RH.
static void measurement(void *state, struct codec_item *it, bool write, const uint16_t *w,
                        size_t n, uint32_t bad)
{
    (void)state;
    (void)write;
    (void)bad;
    if (n < 2) {
        return;
    }
    codec_set_uint(it, SHT_TEMP_RAW, w[0]);
    codec_set_uint(it, SHT_RH_RAW, w[1]);
    codec_set_real(it, SHT_TEMP, -45 + 175.0 * w[0] / 65535);
    codec_set_real(it, SHT_RH, 100.0 * w[1] / 65535);
}


// The status register's flags that are set, by name.
static void status(void *state, struct codec_item *it, bool write, const uint16_t *w,
                   size_t n, uint32_t bad)
{
    static const struct {
        uint16_t bit;
        const char *name;
    } bits[] = {
        {0x8000, "alert"},     {0x2000, "heater"},      {0x0800, "rh alert"},
        {0x0400, "t alert"},   {0x0010, "reset seen"},  {0x0002, "command failed"},
        {0x0001, "write crc failed"},
    };
    char text[STATUS_TEXT_MAX] = "";

    (void)state;
    (void)write;
    (void)bad;
    if (n < 1) {
        return;
    }
    for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
        if (w[0] & bits[i].bit) {
            if (text[0]) {
                strncat(text, ", ", sizeof(text) - strlen(text) - 1);
            }
            strncat(text, bits[i].name, sizeof(text) - strlen(text) - 1);
        }
    }
    codec_set_text(it, SHT_STATUS, text[0] ? text : "clear");
}
