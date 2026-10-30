/*
 * INA219, a current and power monitor: a register pointer device with 16-bit registers,
 * big-endian, at 0x40 to 0x4F.
 *
 * Current and power are in amperes and watts once the current LSB is known: from the
 * calibration register (written or read in the capture, else cal=) and the shunt (shunt=,
 * in ohms), as current LSB = 0.04096 / (calibration * shunt). Until then they are raw
 * register counts, flagged uncalibrated.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "codec/chips/chip.h"
#include "codec/codec.h"
#include "codec/item.h"


// Fields after the common ones.
#define INA_RESET (CHIP_FIELDS + 0)
#define INA_RANGE (CHIP_FIELDS + 1)
#define INA_PGA (CHIP_FIELDS + 2)
#define INA_BADC (CHIP_FIELDS + 3)
#define INA_SADC (CHIP_FIELDS + 4)
#define INA_MODE (CHIP_FIELDS + 5)
#define INA_SHUNT (CHIP_FIELDS + 6)
#define INA_BUS (CHIP_FIELDS + 7)
#define INA_CNVR (CHIP_FIELDS + 8)
#define INA_OVF (CHIP_FIELDS + 9)
#define INA_CURRENT (CHIP_FIELDS + 10)
#define INA_POWER (CHIP_FIELDS + 11)
#define INA_COUNT (CHIP_FIELDS + 12)
#define INA_CAL (CHIP_FIELDS + 13)
#define INA_LSB (CHIP_FIELDS + 14)

#define INA_OVF_FLAG (1U << CHIP_FLAG_SHIFT)
#define INA_UNCALIBRATED (1U << (CHIP_FLAG_SHIFT + 1))

// The calibration register's full scale constant, and its bit 0, which is not used.
#define CAL_SCALE 0.04096
#define CAL_MASK 0xFFFE

/*
 * shunt: ohms (0 not given). cal: the calibration register as seen in the capture, else as
 * given (0 not known).
 */
struct ina219 {
    double shunt;
    uint64_t cal;
};


static int create(void *state, const struct codec_args *args, char *error, size_t size);
static void config(void *state, struct codec_item *it, bool write, uint64_t v);
static void shunt(void *state, struct codec_item *it, bool write, uint64_t v);
static void bus(void *state, struct codec_item *it, bool write, uint64_t v);
static void power(void *state, struct codec_item *it, bool write, uint64_t v);
static void current(void *state, struct codec_item *it, bool write, uint64_t v);
static void calibration(void *state, struct codec_item *it, bool write, uint64_t v);
static double current_lsb(const struct ina219 *s);
static const char *adc_text(unsigned int adc);


static const struct codec_field_desc fields[] = {
    CHIP_COMMON_FIELDS,
    {.name = "reset", .type = CODEC_BOOL},
    {.name = "range", .type = CODEC_UINT, .flags = CODEC_FIELD_HEADER, .unit = "V"},
    {.name = "pga", .type = CODEC_TEXT, .flags = CODEC_FIELD_HEADER},
    {.name = "bus_adc", .type = CODEC_TEXT, .flags = CODEC_FIELD_HEADER},
    {.name = "shunt_adc", .type = CODEC_TEXT, .flags = CODEC_FIELD_HEADER},
    {.name = "mode", .type = CODEC_TEXT, .flags = CODEC_FIELD_HEADER},
    {.name = "shunt", .type = CODEC_REAL, .flags = CODEC_FIELD_HEADER, .digits = 2,
     .unit = "mV"},
    {.name = "bus", .type = CODEC_REAL, .flags = CODEC_FIELD_HEADER, .digits = 3,
     .unit = "V"},
    {.name = "cnvr", .type = CODEC_BOOL},
    {.name = "ovf", .type = CODEC_BOOL},
    {.name = "current", .type = CODEC_REAL, .flags = CODEC_FIELD_HEADER, .unit = "A"},
    {.name = "power", .type = CODEC_REAL, .flags = CODEC_FIELD_HEADER, .unit = "W"},
    {.name = "count", .type = CODEC_INT, .flags = CODEC_FIELD_HEADER, .unit = "lsb"},
    {.name = "calibration", .type = CODEC_UINT, .flags = CODEC_FIELD_HEADER},
    {.name = "current_lsb", .type = CODEC_REAL, .unit = "A"},
};

static const char *const flags[] = {CHIP_COMMON_FLAGS, "ovf", "uncalibrated", NULL};

static const struct codec_kind kind = {
    .name = "ina219",
    .fields = fields,
    .field_count = sizeof(fields) / sizeof(fields[0]),
    .item_flags = flags,
};

static const struct chip_reg regs[] = {
    {0x00, "config", config},
    {0x01, "shunt", shunt},
    {0x02, "bus", bus},
    {0x03, "power", power},
    {0x04, "current", current},
    {0x05, "calibration", calibration},
};

static const uint8_t addrs[] = {
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
    0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F,
};

static const struct chip chip = {
    .label = "INA219",
    .addrs = addrs,
    .addr_count = sizeof(addrs),
    .kind = &kind,
    .pattern = CHIP_REGISTER,
    .regs = regs,
    .reg_count = sizeof(regs) / sizeof(regs[0]),
    .reg_bytes = 2,
    .state_size = sizeof(struct ina219),
    .create = create,
};

static const struct codec_param params[] = {
    CHIP_COMMON_PARAMS,
    {"shunt", "shunt resistor in ohms, for current and power in A and W", CODEC_PARAM_REAL, 0,
     NULL},
    {"cal", "calibration register, when the capture does not show it being set",
     CODEC_PARAM_UINT, 0, NULL},
    {NULL, NULL, 0, 0, NULL},
};

const struct codec_decoder ina219_decoder = {
    .name = "ina219",
    .summary = "INA219 current and power monitor (registers)",
    .params = params,
    .data = &chip,
    CHIP_DECODER_OPS,
};


static int create(void *state, const struct codec_args *args, char *error, size_t size)
{
    struct ina219 *s = state;
    int err = 0;

    err = codec_arg_real(args, "shunt", 0, &s->shunt, error, size);
    if (err == 0) {
        err = codec_arg_uint(args, "cal", 0, &s->cal, error, size);
    }
    if (err == 0 && (s->shunt < 0 || s->cal > 0xFFFF)) {
        snprintf(error, size, "ina219: shunt= must be positive and cal= at most 0xFFFF");
        err = -EINVAL;
    }
    s->cal &= CAL_MASK;

    return err;
}


// Configuration: reset, bus range, PGA gain, ADC settings, operating mode.
static void config(void *state, struct codec_item *it, bool write, uint64_t v)
{
    static const char *const pga[] = {"/1 40 mV", "/2 80 mV", "/4 160 mV", "/8 320 mV"};
    static const char *const modes[] = {
        "power-down", "shunt triggered", "bus triggered", "shunt+bus triggered",
        "adc off", "shunt continuous", "bus continuous", "shunt+bus continuous",
    };
    char text[32];

    (void)state;
    (void)write;
    codec_set_bool(it, INA_RESET, v & 0x8000);
    codec_set_uint(it, INA_RANGE, v & 0x2000 ? 32 : 16);
    codec_set_text(it, INA_PGA, pga[(v >> 11) & 3]);
    snprintf(text, sizeof(text), "bus %s", adc_text((v >> 7) & 0xF));
    codec_set_text(it, INA_BADC, text);
    snprintf(text, sizeof(text), "shunt %s", adc_text((v >> 3) & 0xF));
    codec_set_text(it, INA_SADC, text);
    codec_set_text(it, INA_MODE, modes[v & 7]);
}


// Shunt voltage: signed, 10 uV per bit at every gain.
static void shunt(void *state, struct codec_item *it, bool write, uint64_t v)
{
    (void)state;
    (void)write;
    codec_set_real(it, INA_SHUNT, (int16_t)v * 0.01);
}


// Bus voltage: bits 15 to 3 at 4 mV per bit, then CNVR (bit 1) and OVF (bit 0).
static void bus(void *state, struct codec_item *it, bool write, uint64_t v)
{
    (void)state;
    (void)write;
    codec_set_real(it, INA_BUS, (double)(v >> 3) * 0.004);
    codec_set_bool(it, INA_CNVR, v & 2);
    codec_set_bool(it, INA_OVF, v & 1);
    if (v & 1) {
        it->flags |= INA_OVF_FLAG;
    }
}


// Power: unsigned, 20 times the current LSB per bit.
static void power(void *state, struct codec_item *it, bool write, uint64_t v)
{
    double lsb = current_lsb(state);

    (void)write;
    if (lsb > 0) {
        codec_set_real(it, INA_POWER, (double)v * 20 * lsb);
        codec_set_real(it, INA_LSB, lsb);
    } else {
        codec_set_int(it, INA_COUNT, (int64_t)v);
        it->flags |= INA_UNCALIBRATED;
    }
}


// Current: signed, the current LSB per bit.
static void current(void *state, struct codec_item *it, bool write, uint64_t v)
{
    double lsb = current_lsb(state);

    (void)write;
    if (lsb > 0) {
        codec_set_real(it, INA_CURRENT, (int16_t)v * lsb);
        codec_set_real(it, INA_LSB, lsb);
    } else {
        codec_set_int(it, INA_COUNT, (int16_t)v);
        it->flags |= INA_UNCALIBRATED;
    }
}


// Calibration: kept for current and power from here on.
static void calibration(void *state, struct codec_item *it, bool write, uint64_t v)
{
    struct ina219 *s = state;

    (void)write;
    s->cal = v & CAL_MASK;
    codec_set_uint(it, INA_CAL, s->cal);
    if (current_lsb(s) > 0) {
        codec_set_real(it, INA_LSB, current_lsb(s));
    }
}


// Amperes per bit of the current register; 0 while not known.
static double current_lsb(const struct ina219 *s)
{
    return s->cal && s->shunt > 0 ? CAL_SCALE / ((double)s->cal * s->shunt) : 0;
}


// An ADC setting: 9 to 12 bits, or 12 bits averaged over 2 to 128 samples.
static const char *adc_text(unsigned int adc)
{
    static const char *const bits[] = {"9 bit", "10 bit", "11 bit", "12 bit"};
    static const char *const samples[] = {
        "12 bit", "2 samples", "4 samples", "8 samples",
        "16 samples", "32 samples", "64 samples", "128 samples",
    };

    return adc & 8 ? samples[adc & 7] : bits[adc & 3];
}
