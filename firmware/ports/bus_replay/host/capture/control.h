/*
 * Control messages (protocol/spec.tex, Control messages): the hello with its registers,
 * configuration writes and their replies, and loss reports. Only the messages themselves;
 * the control binding that carries them (SPI) is separate, so capture files can use this
 * without hardware.
 */

#ifndef CAPTURE_CONTROL_H
#define CAPTURE_CONTROL_H

#include <stddef.h>
#include <stdint.h>


#define MSG_HELLO 0x01
#define MSG_CONFIG 0x02
#define MSG_CONFIG_REPLY 0x03
#define MSG_LOSS_REPORT 0x04

#define REG_CAPTURE 0x00
#define REG_SAMPLE_RATE 0x01
#define REG_C 0x02
#define REG_ENABLED 0x03
#define REG_SPLIT 0x04

#define REG_TYPE_BOOL 0x00
#define REG_TYPE_U32 0x01
#define REG_TYPE_CHANNEL_SET 0x02

#define REG_KIND_ANY 0x00
#define REG_KIND_RANGE 0x01
#define REG_KIND_LIST 0x02
#define REG_KIND_DIVIDER 0x03

#define REG_WRITABLE 0x01

// The longest str in a message: its length is one byte.
#define STR_MAX 255

/*
 * A register entry of the hello. value is the current value, size bytes as the type
 * encodes it; allowed is the data after the kind byte, as sent.
 */
struct reg {
    uint8_t id;
    uint8_t type;
    uint8_t flags;
    uint8_t kind;
    char *name;
    char *unit;
    size_t size;
    uint8_t *value;
    uint8_t *allowed;
    size_t allowed_len;
};

// The hello, parsed. names has channels entries. Free with hello_free().
struct hello {
    uint8_t version_major;
    uint8_t version_minor;
    char *model;
    char *firmware;
    uint8_t uid[STR_MAX];
    uint8_t uid_len;
    uint32_t buffer;
    uint16_t channels;
    uint8_t data_channels;
    char **names;
    unsigned int reg_count;
    struct reg *regs;
};

// A configuration reply. value points into the message it was parsed from.
struct config_reply {
    uint8_t reg;
    uint8_t status;
    char text[STR_MAX + 1];
    const uint8_t *value;
    size_t value_len;
};

/*
 * A loss report. Positions are as sent (grid count mod 2^32, offset); lost points into the
 * message: n counters of 4 bytes, read with loss_report_lost().
 */
struct loss_report {
    uint32_t seq;
    uint32_t start_grid;
    uint32_t start_off;
    uint32_t end_grid;
    uint32_t end_off;
    uint16_t n;
    const uint8_t *lost;
};


int hello_parse(const uint8_t *msg, size_t len, struct hello *h);
void hello_free(struct hello *h);
const struct reg *hello_reg(const struct hello *h, uint8_t id);
uint32_t reg_u32(const struct reg *r);
int reg_range(const struct reg *r, uint32_t *min, uint32_t *max);
int reg_divider(const struct reg *r, uint32_t *base, uint32_t *min, uint32_t *max);

int config_msg(uint8_t *out, size_t size, uint8_t reg, const uint8_t *value, size_t len);
int config_reply_parse(const uint8_t *msg, size_t len, struct config_reply *rep);
const char *config_status_text(uint8_t status);

int loss_report_parse(const uint8_t *msg, size_t len, uint16_t n, struct loss_report *rep);
uint32_t loss_report_lost(const struct loss_report *rep, unsigned int i);

#endif
