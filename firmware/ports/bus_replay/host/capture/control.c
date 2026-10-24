#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "capture/control.h"


struct reader {
    const uint8_t *data;
    size_t len;
    size_t i;
    bool short_read;
};


static int reg_parse(struct reader *r, uint16_t channels, struct reg *reg);
static void reg_free(struct reg *reg);
static const uint8_t *take(struct reader *r, size_t n);
static uint32_t take_le(struct reader *r, size_t n);
static char *take_str(struct reader *r);
static uint8_t *take_copy(struct reader *r, size_t n);
static uint32_t get_le(const uint8_t *p, size_t n);


/*
 * Parses a hello (spec, Hello). Every register is kept, with its allowed values and current
 * value as sent. Returns -EPROTO for a malformed hello or a register type the spec does not
 * define, -ENOMEM if out of memory.
 */
int hello_parse(const uint8_t *msg, size_t len, struct hello *h)
{
    struct reader r = {.data = msg, .len = len};
    const uint8_t *uid = NULL;
    int err = 0;

    memset(h, 0, sizeof(*h));
    if (take_le(&r, 1) != MSG_HELLO) {
        return -EPROTO;
    }
    h->version_major = (uint8_t)take_le(&r, 1);
    h->version_minor = (uint8_t)take_le(&r, 1);
    h->model = take_str(&r);
    h->firmware = take_str(&r);
    h->uid_len = (uint8_t)take_le(&r, 1);
    uid = take(&r, h->uid_len);
    if (uid) {
        memcpy(h->uid, uid, h->uid_len);
    }
    h->buffer = take_le(&r, 4);
    h->channels = (uint16_t)take_le(&r, 2);
    h->data_channels = (uint8_t)take_le(&r, 1);
    if (r.short_read) {
        err = -EPROTO;
        goto fail;
    }

    h->names = calloc(h->channels ? h->channels : 1, sizeof(*h->names));
    if (!h->names) {
        err = -ENOMEM;
        goto fail;
    }
    for (unsigned int i = 0; i < h->channels; i++) {
        h->names[i] = take_str(&r);
    }

    h->reg_count = take_le(&r, 1);
    h->regs = calloc(h->reg_count ? h->reg_count : 1, sizeof(*h->regs));
    if (!h->regs) {
        err = -ENOMEM;
        goto fail;
    }
    for (unsigned int i = 0; i < h->reg_count; i++) {
        err = reg_parse(&r, h->channels, &h->regs[i]);
        if (err < 0) {
            goto fail;
        }
    }

    if (r.short_read || !h->model || !h->firmware) {
        err = r.short_read ? -EPROTO : -ENOMEM;
        goto fail;
    }
    for (unsigned int i = 0; i < h->channels; i++) {
        if (!h->names[i]) {
            err = -ENOMEM;
            goto fail;
        }
    }

    return 0;

fail:
    hello_free(h);
    return err;
}


void hello_free(struct hello *h)
{
    free(h->model);
    free(h->firmware);
    if (h->names) {
        for (unsigned int i = 0; i < h->channels; i++) {
            free(h->names[i]);
        }
        free(h->names);
    }
    if (h->regs) {
        for (unsigned int i = 0; i < h->reg_count; i++) {
            reg_free(&h->regs[i]);
        }
        free(h->regs);
    }
    memset(h, 0, sizeof(*h));
}


// The register with ID id, or NULL if the hello has none.
const struct reg *hello_reg(const struct hello *h, uint8_t id)
{
    for (unsigned int i = 0; i < h->reg_count; i++) {
        if (h->regs[i].id == id) {
            return &h->regs[i];
        }
    }

    return NULL;
}


// A bool or u32 register's current value.
uint32_t reg_u32(const struct reg *r)
{
    return get_le(r->value, r->size < 4 ? r->size : 4);
}


// A u32 register's allowed range; -ENOENT if its allowed values are not a range.
int reg_range(const struct reg *r, uint32_t *min, uint32_t *max)
{
    if (r->type != REG_TYPE_U32 || r->kind != REG_KIND_RANGE) {
        return -ENOENT;
    }
    *min = get_le(r->allowed, 4);
    *max = get_le(&r->allowed[4], 4);

    return 0;
}


// A u32 register's divider; -ENOENT if its allowed values are not a divider.
int reg_divider(const struct reg *r, uint32_t *base, uint32_t *min, uint32_t *max)
{
    if (r->kind != REG_KIND_DIVIDER) {
        return -ENOENT;
    }
    *base = get_le(r->allowed, 4);
    *min = get_le(&r->allowed[4], 4);
    *max = get_le(&r->allowed[8], 4);

    return 0;
}


/*
 * Builds the configuration message writing value (len bytes, as the register's type
 * encodes it) to register reg (spec, Configuration). Returns its length, or -ENOSPC if it
 * does not fit in size bytes.
 */
int config_msg(uint8_t *out, size_t size, uint8_t reg, const uint8_t *value, size_t len)
{
    if (len + 2 > size || len + 2 > INT32_MAX) {
        return -ENOSPC;
    }
    out[0] = MSG_CONFIG;
    out[1] = reg;
    memcpy(&out[2], value, len);

    return (int)(len + 2);
}


int config_reply_parse(const uint8_t *msg, size_t len, struct config_reply *rep)
{
    struct reader r = {.data = msg, .len = len};
    size_t n = 0;
    const uint8_t *text = NULL;

    memset(rep, 0, sizeof(*rep));
    if (take_le(&r, 1) != MSG_CONFIG_REPLY) {
        return -EPROTO;
    }
    rep->reg = (uint8_t)take_le(&r, 1);
    rep->status = (uint8_t)take_le(&r, 1);
    n = take_le(&r, 1);
    text = take(&r, n);
    if (r.short_read) {
        return -EPROTO;
    }
    memcpy(rep->text, text, n);
    rep->text[n] = '\0';
    rep->value = &msg[r.i];
    rep->value_len = len - r.i;

    return 0;
}


const char *config_status_text(uint8_t status)
{
    static const char *const text[] = {
        "accepted",
        "unknown register",
        "register not writable",
        "wrong value length",
        "value not allowed",
        "capture already started",
        "sample rate not sustainable",
        "C too short",
        "no channel enabled",
        "split set too large",
    };

    if (status >= 0x80) {
        return "device-specific";
    }

    return status < sizeof(text) / sizeof(text[0]) ? text[status] : "unknown status";
}


// Parses a loss report for n logical channels (spec, Loss report).
int loss_report_parse(const uint8_t *msg, size_t len, uint16_t n, struct loss_report *rep)
{
    struct reader r = {.data = msg, .len = len};

    memset(rep, 0, sizeof(*rep));
    if (take_le(&r, 1) != MSG_LOSS_REPORT) {
        return -EPROTO;
    }
    rep->seq = take_le(&r, 4);
    rep->start_grid = take_le(&r, 4);
    rep->start_off = take_le(&r, 4);
    rep->end_grid = take_le(&r, 4);
    rep->end_off = take_le(&r, 4);
    rep->n = n;
    rep->lost = take(&r, (size_t)n * 4);

    return r.short_read ? -EPROTO : 0;
}


// Logical channel i's lost-records counter.
uint32_t loss_report_lost(const struct loss_report *rep, unsigned int i)
{
    return get_le(&rep->lost[4 * i], 4);
}


static int reg_parse(struct reader *r, uint16_t channels, struct reg *reg)
{
    size_t start = 0;

    reg->id = (uint8_t)take_le(r, 1);
    reg->type = (uint8_t)take_le(r, 1);
    reg->flags = (uint8_t)take_le(r, 1);
    reg->name = take_str(r);
    reg->unit = take_str(r);
    if (r->short_read) {
        return -EPROTO;
    }
    if (!reg->name || !reg->unit) {
        return -ENOMEM;
    }

    if (reg->type == REG_TYPE_BOOL) {
        reg->size = 1;
    } else if (reg->type == REG_TYPE_U32) {
        reg->size = 4;
    } else if (reg->type == REG_TYPE_CHANNEL_SET) {
        reg->size = ((size_t)channels + 7) / 8;
    } else {
        return -EPROTO;
    }

    reg->kind = (uint8_t)take_le(r, 1);
    start = r->i;
    if (reg->kind == REG_KIND_RANGE) {
        take(r, 2 * reg->size);
    } else if (reg->kind == REG_KIND_LIST) {
        take(r, take_le(r, 2) * reg->size);
    } else if (reg->kind == REG_KIND_DIVIDER) {
        if (reg->type != REG_TYPE_U32) {
            return -EPROTO;
        }
        take(r, 12);
    } else if (reg->kind != REG_KIND_ANY) {
        return -EPROTO;
    }
    if (r->short_read) {
        return -EPROTO;
    }

    // The allowed values are copied as sent, then the current value.
    r->i = start;
    reg->allowed_len = 0;
    if (reg->kind == REG_KIND_RANGE) {
        reg->allowed_len = 2 * reg->size;
    } else if (reg->kind == REG_KIND_LIST) {
        reg->allowed_len = 2 + get_le(&r->data[start], 2) * reg->size;
    } else if (reg->kind == REG_KIND_DIVIDER) {
        reg->allowed_len = 12;
    }
    reg->allowed = take_copy(r, reg->allowed_len);
    reg->value = take_copy(r, reg->size);
    if (r->short_read) {
        return -EPROTO;
    }
    if (!reg->allowed || !reg->value) {
        return -ENOMEM;
    }

    return 0;
}


static void reg_free(struct reg *reg)
{
    free(reg->name);
    free(reg->unit);
    free(reg->allowed);
    free(reg->value);
}


static const uint8_t *take(struct reader *r, size_t n)
{
    const uint8_t *p = &r->data[r->i];

    if (r->short_read || n > r->len - r->i) {
        r->short_read = true;
        return NULL;
    }
    r->i += n;

    return p;
}


// Little endian, up to 4 bytes; more bytes are skipped.
static uint32_t take_le(struct reader *r, size_t n)
{
    const uint8_t *p = take(r, n);

    return p ? get_le(p, n < 4 ? n : 4) : 0;
}


// A str (spec, Control messages), as a NUL-terminated copy; NULL if short or out of memory.
static char *take_str(struct reader *r)
{
    size_t n = take_le(r, 1);
    const uint8_t *p = take(r, n);
    char *s = NULL;

    if (!p) {
        return NULL;
    }
    s = malloc(n + 1);
    if (s) {
        memcpy(s, p, n);
        s[n] = '\0';
    }

    return s;
}


// n bytes as a copy (at least one byte is allocated); NULL if short or out of memory.
static uint8_t *take_copy(struct reader *r, size_t n)
{
    const uint8_t *p = take(r, n);
    uint8_t *copy = NULL;

    if (!p) {
        return NULL;
    }
    copy = malloc(n ? n : 1);
    if (copy) {
        memcpy(copy, p, n);
    }

    return copy;
}


static uint32_t get_le(const uint8_t *p, size_t n)
{
    uint32_t v = 0;

    for (size_t i = 0; i < n; i++) {
        v |= (uint32_t)p[i] << (8 * i);
    }

    return v;
}
