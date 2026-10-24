#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <bits/time.h>

#include "capture/control.h"
#include "capture/dummy.h"
#include "capture/info.h"
#include "capture/source.h"
#include "capture/thcfile.h"
#include "capture/wire.h"
#include "common/capture_info.h"
#include "common/record.h"


#define CHANNELS 6
#define DIVIDER_BASE 64000000
#define DIVIDER_MIN 128
#define DIVIDER_MAX 65535
#define C_MIN 2
#define C_MAX ((1U << 27) - 1)

#define STEP_S 0.05
#define IDLE_MS 5

#define I2C_PERIOD_S 20e-3
// A loss over the end of an I2C measurement: the last edges before the bus goes idle are
// dropped, so the next heartbeats disagree with the records until the report excuses them.
#define LOSS_AT_S 1.2016
#define LOSS_PERIOD_S 2.5
#define LOSS_LEN_S 0.5e-3

// A wave repeating every period ticks, toggling at offs within each period.
struct wave {
    double period;
    double *offs;
    unsigned int count;
    unsigned int cap;
    int level0;
};

struct edge {
    uint64_t tick;
    unsigned int k;
};

struct dummy {
    bool realtime;
    struct thc_buf hello;
    struct thc_buf reply;
    struct thc_buf msg;

    uint32_t rate;
    uint32_t c;
    uint8_t enabled;
    uint8_t split;
    bool on;

    struct hello h;
    struct capture_info info;
    struct wire_writer wr;
    uint64_t t0;
    uint64_t now;
    uint64_t next_grid;
    struct wave sig[CHANNELS];
    uint8_t level[CHANNELS];
    struct edge *edges;
    size_t edge_cap;

    uint64_t loss_at;
    uint64_t loss_len;
    uint64_t loss_period;
    bool loss_open;
    uint64_t loss_start;
    uint64_t loss_end;
    uint32_t lost[CHANNELS];
    uint32_t seq;

    // Loss reports waiting to be read: 4-byte length, then the message.
    struct thc_buf pending;
    size_t pending_at;
};


static int hello(void *src, const uint8_t **msg, size_t *len);
static int config(void *src, const uint8_t *msg, size_t len, const uint8_t **reply,
                  size_t *reply_len);
static int dummy_read(void *src, struct source_item *item, int timeout_ms);
static int stop(void *src);
static void dummy_close(void *src);
static void reset(struct dummy *d);
static int build_hello(struct dummy *d);
static void reg_entry(struct thc_buf *b, uint8_t id, uint8_t type, const char *name,
                      const char *unit);
static void put_str(struct thc_buf *b, const char *s);
static uint8_t write_reg(struct dummy *d, uint8_t reg, const uint8_t *value, size_t len);
static uint8_t start(struct dummy *d);
static void reg_value(struct dummy *d, uint8_t reg, struct thc_buf *b);
static int generate(struct dummy *d, uint64_t end);
static int edges_in(struct dummy *d, uint64_t from, uint64_t end, size_t *count);
static int emit_record(struct dummy *d, uint64_t tick, unsigned int k);
static int emit_heartbeat(struct dummy *d);
static int loss_advance(struct dummy *d, uint64_t tick);
static int wave_add(struct wave *s, double off);
static int signals(struct dummy *d);
static int periodic(struct wave *s, double rate, double period_s, const double *offs_s,
                    unsigned int count, int level0);
static int uart_signal(struct wave *s, double rate);
static int i2c_signals(struct wave *scl, struct wave *sda, double rate);
static int edge_cmp(const void *a, const void *b);
static uint64_t mono_ns(void);


const struct source_ops dummy_ops = {
    .hello = hello,
    .config = config,
    .read = dummy_read,
    .stop = stop,
    .close = dummy_close,
};

static const char *const names[CHANNELS] = {"CH1", "CH2", "CH3", "CH4", "CH5", "CH6"};


int dummy_open(void **src, const struct dummy_options *opt)
{
    struct dummy *d = calloc(1, sizeof(*d));

    *src = NULL;
    if (!d) {
        return -ENOMEM;
    }
    d->realtime = opt && opt->realtime;
    reset(d);
    *src = d;

    return 0;
}


// A reset: capture off, the registers at their defaults, then the hello.
static int hello(void *src, const uint8_t **msg, size_t *len)
{
    struct dummy *d = src;
    int err = 0;

    reset(d);
    err = build_hello(d);
    if (err < 0) {
        return err;
    }
    *msg = d->hello.data;
    *len = d->hello.len;

    return 0;
}


// A configuration message and its reply (spec, Configuration).
static int config(void *src, const uint8_t *msg, size_t len, const uint8_t **reply,
                  size_t *reply_len)
{
    struct dummy *d = src;
    uint8_t reg = len > 1 ? msg[1] : 0;
    uint8_t status = 0;
    int err = 0;

    if (len < 2 || msg[0] != MSG_CONFIG) {
        status = 0x03;
    } else {
        status = write_reg(d, reg, &msg[2], len - 2);
    }

    d->reply.len = 0;
    err = thc_buf_le(&d->reply, MSG_CONFIG_REPLY, 1);
    err = err ? err : thc_buf_le(&d->reply, reg, 1);
    err = err ? err : thc_buf_le(&d->reply, status, 1);
    put_str(&d->reply, "");
    if (err < 0) {
        return err;
    }
    if (status != 0x01) {
        reg_value(d, reg, &d->reply);
    }
    *reply = d->reply.data;
    *reply_len = d->reply.len;

    return 0;
}


static int dummy_read(void *src, struct source_item *item, int timeout_ms)
{
    struct dummy *d = src;
    uint64_t step = 0;
    uint64_t end = 0;
    int err = 0;

    if (d->pending_at < d->pending.len) {
        uint32_t n = (uint32_t)thc_get_le(&d->pending.data[d->pending_at], 4);

        *item = (struct source_item){
            .type = THC_CONTROL,
            .time = mono_ns(),
            .data = &d->pending.data[d->pending_at + 4],
            .len = n,
        };
        d->pending_at += 4 + (size_t)n;
        return 1;
    }
    d->pending.len = 0;
    d->pending_at = 0;

    if (!d->on) {
        if (timeout_ms > 0) {
            struct timespec ts = {.tv_sec = timeout_ms / 1000,
                                  .tv_nsec = (long)(timeout_ms % 1000) * 1000000};

            nanosleep(&ts, NULL);
        }
        return 0;
    }

    step = (uint64_t)(STEP_S * d->rate);
    step = step ? step : 1;
    if (d->realtime) {
        uint64_t at = (uint64_t)((double)(mono_ns() - d->t0) * d->rate / 1e9);

        if (at < d->now + d->rate / (1000 / IDLE_MS) && timeout_ms > 0) {
            int ms = timeout_ms < IDLE_MS ? timeout_ms : IDLE_MS;
            struct timespec ts = {.tv_nsec = (long)ms * 1000000};

            nanosleep(&ts, NULL);
            at = (uint64_t)((double)(mono_ns() - d->t0) * d->rate / 1e9);
        }
        if (at <= d->now) {
            return 0;
        }
        end = at - d->now > step ? d->now + step : at;
    } else {
        end = d->now + step;
    }

    wire_writer_clear(&d->wr);
    err = generate(d, end);
    if (err < 0) {
        return err;
    }
    if (d->wr.len == 0) {
        return 0;
    }
    *item = (struct source_item){
        .type = THC_DATA,
        .time = mono_ns(),
        .data = d->wr.out,
        .len = d->wr.len,
    };

    return 1;
}


static int stop(void *src)
{
    reset(src);

    return 0;
}


static void dummy_close(void *src)
{
    struct dummy *d = src;

    if (!d) {
        return;
    }
    reset(d);
    thc_buf_free(&d->hello);
    thc_buf_free(&d->reply);
    thc_buf_free(&d->msg);
    thc_buf_free(&d->pending);
    free(d->edges);
    free(d);
}


static void reset(struct dummy *d)
{
    if (d->on) {
        wire_writer_free(&d->wr);
        capture_info_free(&d->info);
        hello_free(&d->h);
    }
    for (unsigned int k = 0; k < CHANNELS; k++) {
        free(d->sig[k].offs);
        memset(&d->sig[k], 0, sizeof(d->sig[k]));
    }
    d->on = false;
    d->rate = 500000;
    d->c = 1000;
    d->enabled = 0x0F;
    d->split = 0;
    d->pending.len = 0;
    d->pending_at = 0;
    d->loss_open = false;
    d->seq = 0;
    memset(d->lost, 0, sizeof(d->lost));
}


// The hello (spec, Hello): six channels on one data channel.
static int build_hello(struct dummy *d)
{
    struct thc_buf *b = &d->hello;
    static const uint8_t uid[8] = {0};

    b->len = 0;
    thc_buf_le(b, MSG_HELLO, 1);
    thc_buf_le(b, 1, 1);
    thc_buf_le(b, 2, 1);
    put_str(b, "TH-1 MO bus replay (dummy)");
    put_str(b, "dummy");
    thc_buf_le(b, sizeof(uid), 1);
    thc_buf_put(b, uid, sizeof(uid));
    thc_buf_le(b, 131072, 4);
    thc_buf_le(b, CHANNELS, 2);
    thc_buf_le(b, 1, 1);
    for (unsigned int i = 0; i < CHANNELS; i++) {
        put_str(b, names[i]);
    }
    thc_buf_le(b, 5, 1);

    reg_entry(b, REG_CAPTURE, REG_TYPE_BOOL, "capture", "");
    thc_buf_le(b, REG_KIND_LIST, 1);
    thc_buf_le(b, 1, 2);
    thc_buf_le(b, 1, 1);
    reg_value(d, REG_CAPTURE, b);

    reg_entry(b, REG_SAMPLE_RATE, REG_TYPE_U32, "sample rate", "Hz");
    thc_buf_le(b, REG_KIND_DIVIDER, 1);
    thc_buf_le(b, DIVIDER_BASE, 4);
    thc_buf_le(b, DIVIDER_MIN, 4);
    thc_buf_le(b, DIVIDER_MAX, 4);
    reg_value(d, REG_SAMPLE_RATE, b);

    reg_entry(b, REG_C, REG_TYPE_U32, "C", "ticks");
    thc_buf_le(b, REG_KIND_RANGE, 1);
    thc_buf_le(b, C_MIN, 4);
    thc_buf_le(b, C_MAX, 4);
    reg_value(d, REG_C, b);

    reg_entry(b, REG_ENABLED, REG_TYPE_CHANNEL_SET, "enabled", "");
    thc_buf_le(b, REG_KIND_ANY, 1);
    reg_value(d, REG_ENABLED, b);

    reg_entry(b, REG_SPLIT, REG_TYPE_CHANNEL_SET, "split", "");
    thc_buf_le(b, REG_KIND_ANY, 1);

    return thc_buf_put(b, &d->split, 1);
}


static void reg_entry(struct thc_buf *b, uint8_t id, uint8_t type, const char *name,
                      const char *unit)
{
    thc_buf_le(b, id, 1);
    thc_buf_le(b, type, 1);
    thc_buf_le(b, REG_WRITABLE, 1);
    put_str(b, name);
    put_str(b, unit);
}


static void put_str(struct thc_buf *b, const char *s)
{
    size_t n = strlen(s);

    thc_buf_le(b, n, 1);
    thc_buf_put(b, s, n);
}


// One register write, checked as the spec says; the reply's status.
static uint8_t write_reg(struct dummy *d, uint8_t reg, const uint8_t *value, size_t len)
{
    uint32_t v = 0;
    size_t size = reg == REG_CAPTURE ? 1 : reg <= REG_C ? 4 : 1;

    if (reg > REG_SPLIT) {
        return 0x01;
    }
    if (d->on) {
        return 0x05;
    }
    if (len != size) {
        return 0x03;
    }
    v = (uint32_t)thc_get_le(value, len);

    switch (reg) {
    case REG_CAPTURE:
        return v == 1 ? start(d) : 0x04;
    case REG_SAMPLE_RATE:
        if (v == 0 || DIVIDER_BASE % v || DIVIDER_BASE / v < DIVIDER_MIN ||
            DIVIDER_BASE / v > DIVIDER_MAX) {
            return 0x04;
        }
        d->rate = v;
        return 0;
    case REG_C:
        if (v < C_MIN || v > C_MAX) {
            return 0x04;
        }
        d->c = v;
        return 0;
    default:
        if (v >> CHANNELS) {
            return 0x04;
        }
        if (reg == REG_ENABLED) {
            d->enabled = (uint8_t)v;
        } else {
            d->split = (uint8_t)v;
        }
        return 0;
    }
}


// The start checks, then the capture: its first heartbeat comes with the next read.
static uint8_t start(struct dummy *d)
{
    if (d->enabled == 0) {
        return 0x08;
    }
    // One data channel: no split channel is allowed.
    if (d->split != 0) {
        return 0x09;
    }
    if (build_hello(d) < 0 || hello_parse(d->hello.data, d->hello.len, &d->h) < 0) {
        return 0x80;
    }
    if (capture_info_init(&d->info, &d->h, d->rate, d->c, &d->enabled, &d->split) < 0) {
        hello_free(&d->h);
        return 0x80;
    }
    if (wire_writer_init(&d->wr, &d->info, 0) < 0 || signals(d) < 0) {
        wire_writer_free(&d->wr);
        capture_info_free(&d->info);
        hello_free(&d->h);
        return 0x80;
    }

    d->on = true;
    d->t0 = mono_ns();
    d->now = 0;
    d->next_grid = 0;
    d->loss_at = (uint64_t)(LOSS_AT_S * d->rate);
    d->loss_len = (uint64_t)(LOSS_LEN_S * d->rate);
    d->loss_len = d->loss_len ? d->loss_len : 1;
    d->loss_period = (uint64_t)(LOSS_PERIOD_S * d->rate);

    return 0;
}


static void reg_value(struct dummy *d, uint8_t reg, struct thc_buf *b)
{
    if (reg == REG_CAPTURE) {
        thc_buf_le(b, d->on, 1);
    } else if (reg == REG_SAMPLE_RATE) {
        thc_buf_le(b, d->rate, 4);
    } else if (reg == REG_C) {
        thc_buf_le(b, d->c, 4);
    } else if (reg == REG_ENABLED) {
        thc_buf_le(b, d->enabled, 1);
    } else if (reg == REG_SPLIT) {
        thc_buf_le(b, d->split, 1);
    }
}


/*
 * Writes the stream from d->now up to end (exclusive): every record and heartbeat in tick
 * order, records of a tick before its heartbeat.
 */
static int generate(struct dummy *d, uint64_t end)
{
    size_t count = 0;
    size_t i = 0;
    int err = edges_in(d, d->now, end, &count);

    while (err == 0) {
        uint64_t grid_tick = d->next_grid * d->c;
        uint64_t edge_tick = i < count ? d->edges[i].tick : UINT64_MAX;
        unsigned int toggles = 0;
        size_t j = i;

        if (grid_tick < end && grid_tick < edge_tick) {
            err = emit_heartbeat(d);
            continue;
        }
        if (i == count) {
            break;
        }

        // Edges of one channel in one tick: only an odd number changes its level.
        while (j < count && d->edges[j].tick == edge_tick && d->edges[j].k == d->edges[i].k) {
            toggles++;
            j++;
        }
        if (toggles % 2) {
            err = emit_record(d, edge_tick, d->edges[i].k);
        }
        i = j;
    }
    d->now = end;

    return err;
}


// The signals' edges in [from, end), sorted; tick 0 is the first heartbeat's.
static int edges_in(struct dummy *d, uint64_t from, uint64_t end, size_t *count)
{
    size_t n = 0;

    for (unsigned int k = 0; k < d->info.n; k++) {
        const struct wave *s = &d->sig[k];
        double p0 = floor((double)from / s->period) - 1;

        if (s->count == 0) {
            continue;
        }
        for (double p = p0 < 0 ? 0 : p0; p * s->period < (double)end; p++) {
            for (unsigned int j = 0; j < s->count; j++) {
                uint64_t e = (uint64_t)ceil(p * s->period + s->offs[j]);

                if (e < from || e >= end || e == 0) {
                    continue;
                }
                if (n == d->edge_cap) {
                    size_t grown = d->edge_cap ? 2 * d->edge_cap : 4096;
                    struct edge *bigger = realloc(d->edges, grown * sizeof(*bigger));

                    if (!bigger) {
                        return -ENOMEM;
                    }
                    d->edges = bigger;
                    d->edge_cap = grown;
                }
                d->edges[n++] = (struct edge){.tick = e, .k = k};
            }
        }
    }
    qsort(d->edges, n, sizeof(*d->edges), edge_cmp);
    *count = n;

    return 0;
}


// Logical channel k changes level at tick: a record, or a drop inside a loss.
static int emit_record(struct dummy *d, uint64_t tick, unsigned int k)
{
    struct record r = {.tick = tick, .channel = d->info.enabled[k]};
    int err = 0;

    d->level[k] ^= 1;
    r.level = d->level[k];
    if (tick >= d->loss_at && tick < d->loss_at + d->loss_len) {
        d->lost[k]++;
        if (!d->loss_open) {
            d->loss_open = true;
            d->loss_start = tick;
        }
        d->loss_end = tick;
        return 0;
    }

    err = loss_advance(d, tick);
    return err ? err : wire_write_record(&d->wr, &r);
}


// The heartbeat of the next grid point, with every channel's level.
static int emit_heartbeat(struct dummy *d)
{
    uint8_t levels[(CHANNELS + 7) / 8] = {0};
    uint64_t grid = d->next_grid;
    int err = 0;

    for (unsigned int k = 0; k < d->info.n; k++) {
        levels[k / 8] |= (uint8_t)(d->level[k] << (k % 8));
    }
    err = loss_advance(d, grid * d->c);
    err = err ? err : wire_write_heartbeat(&d->wr, grid, levels);
    d->next_grid++;

    return err;
}


/*
 * Before an entry at tick is written: a loss whose window has passed is over, and its
 * report is due (spec, Loss report). Windows without a drop pass without one.
 */
static int loss_advance(struct dummy *d, uint64_t tick)
{
    struct thc_buf *b = &d->pending;
    int err = 0;

    while (tick >= d->loss_at + d->loss_len) {
        if (d->loss_open) {
            size_t at = b->len;

            err = thc_buf_le(b, 0, 4);
            err = err ? err : thc_buf_le(b, MSG_LOSS_REPORT, 1);
            err = err ? err : thc_buf_le(b, d->seq++, 4);
            err = err ? err : thc_buf_le(b, (uint32_t)(d->loss_start / d->c), 4);
            err = err ? err : thc_buf_le(b, (uint32_t)(d->loss_start % d->c), 4);
            err = err ? err : thc_buf_le(b, (uint32_t)(d->loss_end / d->c), 4);
            err = err ? err : thc_buf_le(b, (uint32_t)(d->loss_end % d->c), 4);
            for (unsigned int k = 0; k < d->info.n && err == 0; k++) {
                err = thc_buf_le(b, d->lost[k], 4);
            }
            if (err < 0) {
                return err;
            }
            for (size_t i = 0; i < 4; i++) {
                b->data[at + i] = (uint8_t)((b->len - at - 4) >> (8 * i));
            }
            d->loss_open = false;
        }
        d->loss_at += d->loss_period;
    }

    return 0;
}


static int wave_add(struct wave *s, double off)
{
    if (s->count == s->cap) {
        unsigned int grown = s->cap ? 2 * s->cap : 64;
        double *bigger = realloc(s->offs, grown * sizeof(*bigger));

        if (!bigger) {
            return -ENOMEM;
        }
        s->offs = bigger;
        s->cap = grown;
    }
    s->offs[s->count++] = off;

    return 0;
}


// Each enabled channel's signal, by its channel number, and its level at tick 0.
static int signals(struct dummy *d)
{
    static const double clock[] = {0, 0.5e-3};
    static const double pwm[] = {1e-3, 3.5e-3};
    struct wave scl = {0};
    struct wave sda = {0};
    double rate = d->rate;
    int err = i2c_signals(&scl, &sda, rate);

    for (unsigned int k = 0; k < d->info.n && err == 0; k++) {
        struct wave *s = &d->sig[k];

        switch (d->info.enabled[k]) {
        case 0:
            *s = scl;
            scl = (struct wave){0};
            break;
        case 1:
            *s = sda;
            sda = (struct wave){0};
            break;
        case 2:
            err = periodic(s, rate, 1, NULL, 0, 1);
            break;
        case 3:
            err = uart_signal(s, rate);
            break;
        case 4:
            err = periodic(s, rate, 1e-3, clock, 2, 0);
            break;
        default:
            err = periodic(s, rate, 10e-3, pwm, 2, 0);
            break;
        }
        d->level[k] = (uint8_t)s->level0;
    }
    free(scl.offs);
    free(sda.offs);

    return err;
}


// A signal repeating every period_s, toggling at offs_s within it, from level0.
static int periodic(struct wave *s, double rate, double period_s, const double *offs_s,
                    unsigned int count, int level0)
{
    int err = 0;

    s->period = period_s * rate;
    s->level0 = level0;
    for (unsigned int i = 0; i < count && err == 0; i++) {
        err = wave_add(s, offs_s[i] * rate);
    }

    return err;
}


// "TH-1 dummy\r\n" at 115200 8N1 every 100 ms, idle high.
static int uart_signal(struct wave *s, double rate)
{
    static const char msg[] = "TH-1 dummy\r\n";
    const double bit = 1.0 / 115200;
    double t = 1e-3;
    int level = 1;
    int err = 0;

    s->period = 100e-3 * rate;
    s->level0 = 1;
    for (const char *c = msg; *c && err == 0; c++) {
        // Start bit, eight data bits LSB first, stop bit.
        for (int b = -1; b <= 8 && err == 0; b++) {
            int v = b < 0 ? 0 : b == 8 ? 1 : (*c >> b) & 1;

            if (v != level) {
                err = wave_add(s, t * rate);
                level = v;
            }
            t += bit;
        }
    }

    return err;
}


// One SHT3x measurement every 20 ms at 100 kHz: write 0x2400, then read six bytes.
static int i2c_signals(struct wave *scl, struct wave *sda, double rate)
{
    static const uint8_t cmd[] = {0x88, 0x24, 0x00};
    static const uint8_t data[] = {0x89, 0x63, 0x52, 0xe5, 0x8f, 0x3a, 0x25};
    const double bit = 10e-6;
    double t = 0.0002;
    int level[2] = {1, 1};
    struct wave *sig[2] = {sda, scl};
    int err = 0;

// Line 0 is SDA, 1 is SCL; each change is an edge at the current time.
#define SET(line, v)                                                                          \
    do {                                                                                      \
        if ((v) != level[line] && err == 0) {                                                 \
            err = wave_add(sig[line], t * rate);                                            \
            level[line] = (v);                                                                \
        }                                                                                     \
    } while (0)

    for (int frame = 0; frame < 2; frame++) {
        const uint8_t *bytes = frame ? data : cmd;
        unsigned int n = frame ? sizeof(data) : sizeof(cmd);

        // Start, the bytes, each with its ACK (the master NAKs the last byte it reads),
        // stop.
        SET(0, 0);
        t += bit / 2;
        SET(1, 0);
        t += bit / 4;
        for (unsigned int i = 0; i < n; i++) {
            int nak = frame && i == n - 1;

            for (int b = 7; b >= -1; b--) {
                SET(0, b >= 0 ? (bytes[i] >> b) & 1 : nak);
                t += bit / 4;
                SET(1, 1);
                t += bit / 2;
                SET(1, 0);
                t += bit / 4;
            }
        }
        SET(0, 0);
        t += bit / 4;
        SET(1, 1);
        t += bit / 2;
        SET(0, 1);
        t += frame ? 0 : 0.0005;
    }
#undef SET

    scl->period = sda->period = I2C_PERIOD_S * rate;
    scl->level0 = sda->level0 = 1;

    return err;
}


static int edge_cmp(const void *a, const void *b)
{
    const struct edge *x = a;
    const struct edge *y = b;

    if (x->tick != y->tick) {
        return x->tick < y->tick ? -1 : 1;
    }

    return (int)x->k - (int)y->k;
}


static uint64_t mono_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
