/*
 * The wire format: random records written by the wire writer and parsed back are unchanged,
 * for several C values and channel counts, so the record width and the tag bits vary. Then
 * a failed check byte, a break in the stream, a mismatch excused by a loss report,
 * whichever arrives first, and mismatches in the tail.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "capture/control.h"
#include "capture/info.h"
#include "capture/wire.h"
#include "common/capture_info.h"
#include "common/record.h"
#include "test/test.h"


#define CHANNELS_MAX 64
#define DATA_MAX 4
#define LEVEL_BYTES (CHANNELS_MAX / 8)

struct config {
    uint16_t channels;
    uint8_t data_channels;
    uint64_t enabled;
    uint64_t split;
    uint32_t c;
    unsigned int grids;
};

struct list {
    struct record *rec;
    size_t count;
    size_t cap;
};

/*
 * A generated capture: per data channel its bytes and the records in them, and the levels
 * at every grid point. drop_grid, when not 0, drops the first record of logical channel 0
 * in the interval before that grid point, as a device out of buffer room would.
 */
struct capture {
    struct capture_info info;
    uint64_t first_grid;
    struct wire_writer wr[DATA_MAX];
    struct list expect[DATA_MAX];
    size_t check_at[DATA_MAX][16];
    uint8_t *levels;
    unsigned int nb;
    uint64_t drop_grid;
    struct record dropped;
};

// What the parser handed over.
struct got {
    struct list rec;
    const struct capture *cap;
    uint64_t next_grid;
    unsigned int hb_bad;
    unsigned int hb_unverified;
};


static void make_info(struct capture_info *info, const struct config *cfg);
static void generate(struct capture *cap, const struct config *cfg, uint64_t first_grid,
                     uint64_t drop_grid);
static void capture_free(struct capture *cap);
static void parse(struct wire *w, const struct capture *cap, uint16_t d, struct got *got,
                  const uint8_t *data, size_t len);
static void on_record(void *ctx, const struct record *r);
static void on_heartbeat(void *ctx, const struct heartbeat *hb);
static bool same(const struct record *a, const struct record *b, size_t n);
static void push(struct list *l, const struct record *r);
static int by_tick(const void *a, const void *b);
static uint64_t rnd(void);
static void test_round_trip(void);
static void test_start_at(void);
static void test_crc_error(void);
static void test_break(void);
static void test_excused(void);
static void test_tail(void);


static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;


int main(void)
{
    test_round_trip();
    test_start_at();
    test_crc_error();
    test_break();
    test_excused();
    test_tail();

    return test_result(__FILE__);
}


static void test_round_trip(void)
{
    static const struct config configs[] = {
        {1, 1, 0x1, 0, 1, 600},
        {1, 1, 0x1, 0, 1000, 300},
        {6, 1, 0x0F, 0, 1000, 300},
        {6, 1, 0x3F, 0, 1000, 300},
        {40, 1, 0xFFFFFFFFFFULL & ~((1ULL << 5) | (1ULL << 17) | (1ULL << 33)), 0, 100000, 50},
        {8, 3, 0xDF, 0x44, 50, 300},
        {8, 3, 0xDF, 0x40, 50, 300},
        {6, 1, 0x3F, 0, (1U << 27) - 1, 20},
    };

    for (size_t i = 0; i < sizeof(configs) / sizeof(configs[0]); i++) {
        const struct config *cfg = &configs[i];
        struct capture cap;
        bool ok = true;

        generate(&cap, cfg, 0, 0);
        for (uint16_t d = 0; d < cfg->data_channels; d++) {
            struct wire w;
            struct got got;

            parse(&w, &cap, d, &got, cap.wr[d].out, cap.wr[d].len);
            ok = ok && got.rec.count == cap.expect[d].count;
            ok = ok && same(got.rec.rec, cap.expect[d].rec, got.rec.count);
            ok = ok && w.stats.records == cap.expect[d].count;
            ok = ok && w.stats.heartbeats == cfg->grids + 1 && got.next_grid == cfg->grids + 1;
            ok = ok && got.hb_bad == 0 && got.hb_unverified == 0;
            ok = ok && w.stats.crc_errors == 0 && w.stats.gap_errors == 0;
            ok = ok && w.stats.mismatches == 0 && w.stats.resyncs == 0;
            ok = ok && w.stats.bytes == cap.wr[d].len;
            ok = ok && wire_watermark(&w).tick == (uint64_t)cfg->grids * cfg->c;
            if (!ok) {
                fprintf(stderr, "config %zu, data channel %u: W %u, t %u\n", i, d, w.w, w.t);
            }
            free(got.rec.rec);
            wire_free(&w);
        }
        CHECK(ok);
        capture_free(&cap);
    }
}


// A stream that starts at grid count 1000, its first check byte not checkable.
static void test_start_at(void)
{
    const struct config cfg = {6, 1, 0x0F, 0, 1000, 50};
    struct capture cap;
    struct wire w;
    struct got got;

    generate(&cap, &cfg, 1000, 0);
    cap.wr[0].out[cap.check_at[0][0]] ^= 0x5A;
    parse(&w, &cap, 0, &got, cap.wr[0].out, cap.wr[0].len);

    CHECK(got.rec.count == cap.expect[0].count);
    CHECK(same(got.rec.rec, cap.expect[0].rec, got.rec.count));
    CHECK(got.next_grid == 1000 + cfg.grids + 1 && got.hb_bad == 0);
    CHECK(got.hb_unverified == 1 && w.stats.crc_errors == 0);
    CHECK(wire_tick(&w, 1003, 7) == 1003 * 1000 + 7);

    free(got.rec.rec);
    wire_free(&w);
    capture_free(&cap);
}


/*
 * Heartbeat 5's check byte fails: counted, and the parser takes up the stream again. What
 * it reads before that heartbeat and after heartbeat 8 is unchanged.
 */
static void test_crc_error(void)
{
    const struct config cfg = {6, 1, 0x0F, 0, 1000, 12};
    struct capture cap;
    struct wire w;
    struct got got;
    size_t head = 0;
    size_t tail = 0;
    size_t got_tail = 0;

    generate(&cap, &cfg, 0, 0);
    cap.wr[0].out[cap.check_at[0][5]] ^= 0x01;
    parse(&w, &cap, 0, &got, cap.wr[0].out, cap.wr[0].len);

    while (head < cap.expect[0].count && cap.expect[0].rec[head].tick <= 5 * cfg.c) {
        head++;
    }
    tail = head;
    while (tail < cap.expect[0].count && cap.expect[0].rec[tail].tick <= 8 * cfg.c) {
        tail++;
    }
    got_tail = got.rec.count;
    while (got_tail > 0 && got.rec.rec[got_tail - 1].tick > 8 * cfg.c) {
        got_tail--;
    }

    CHECK(w.stats.crc_errors == 1 && w.stats.resyncs == 1);
    CHECK(got.rec.count >= head && same(got.rec.rec, cap.expect[0].rec, head));
    CHECK(got.rec.count - got_tail == cap.expect[0].count - tail);
    CHECK(same(&got.rec.rec[got_tail], &cap.expect[0].rec[tail], cap.expect[0].count - tail));
    CHECK(got.next_grid == cfg.grids + 1);

    free(got.rec.rec);
    wire_free(&w);
    capture_free(&cap);
}


// Bytes go missing between heartbeats 3 and 6 (a host overflow); the rest reads unchanged.
static void test_break(void)
{
    const struct config cfg = {6, 1, 0x3F, 0, 1000, 12};
    struct capture cap;
    struct wire w;
    struct got got;
    size_t cut = 0;
    size_t resume = 0;
    size_t tail = 0;
    size_t got_tail = 0;

    generate(&cap, &cfg, 0, 0);
    cut = cap.check_at[0][3] + 1 + 7;
    resume = cap.check_at[0][6] - 5;
    parse(&w, &cap, 0, &got, cap.wr[0].out, cut);
    wire_break(&w);
    wire_feed(&w, &cap.wr[0].out[resume], cap.wr[0].len - resume);

    while (tail < cap.expect[0].count && cap.expect[0].rec[tail].tick <= 8 * cfg.c) {
        tail++;
    }
    got_tail = got.rec.count;
    while (got_tail > 0 && got.rec.rec[got_tail - 1].tick > 8 * cfg.c) {
        got_tail--;
    }

    CHECK(w.stats.resyncs == 1 && w.stats.crc_errors == 0);
    CHECK(got.rec.count - got_tail == cap.expect[0].count - tail);
    CHECK(same(&got.rec.rec[got_tail], &cap.expect[0].rec[tail], cap.expect[0].count - tail));
    CHECK(got.next_grid == cfg.grids + 1);

    free(got.rec.rec);
    wire_free(&w);
    capture_free(&cap);
}


/*
 * A record dropped before heartbeat 4 leaves its level wrong there: a mismatch, excused by
 * a loss report covering the drop, whether the report comes after the heartbeat or before.
 */
static void test_excused(void)
{
    const struct config cfg = {6, 1, 0x0F, 0, 1000, 10};
    struct capture cap;
    struct loss_report rep = {0};
    struct wire_span span;
    struct wire w;
    struct got got;

    generate(&cap, &cfg, 0, 4);
    rep.start_grid = rep.end_grid = (uint32_t)(cap.dropped.tick / cfg.c);
    rep.start_off = rep.end_off = (uint32_t)(cap.dropped.tick % cfg.c);

    parse(&w, &cap, 0, &got, cap.wr[0].out, cap.wr[0].len);
    CHECK(got.rec.count == cap.expect[0].count);
    CHECK(w.stats.mismatches == 1 && w.stats.excused == 0);
    CHECK(wire_add_loss(&w, &rep, &span) == 0);
    CHECK(span.start == cap.dropped.tick && span.end == cap.dropped.tick);
    CHECK(w.stats.mismatches == 0 && w.stats.excused == 1);
    free(got.rec.rec);
    wire_free(&w);

    // The report first: the parser holds it until the heartbeat.
    parse(&w, &cap, 0, &got, NULL, 0);
    CHECK(wire_add_loss(&w, &rep, &span) == 0);
    wire_feed(&w, cap.wr[0].out, cap.wr[0].len);
    CHECK(w.stats.mismatches == 0 && w.stats.excused == 1);
    free(got.rec.rec);
    wire_free(&w);

    // A report for another interval excuses nothing.
    parse(&w, &cap, 0, &got, cap.wr[0].out, cap.wr[0].len);
    rep.start_grid = rep.end_grid = 6;
    CHECK(wire_add_loss(&w, &rep, &span) == 0);
    CHECK(w.stats.mismatches == 1 && w.stats.excused == 0);
    free(got.rec.rec);
    wire_free(&w);

    capture_free(&cap);
}


/*
 * A mismatch with good heartbeats after it is not in the tail; one at the last heartbeat
 * (a loss still open at the end) is.
 */
static void test_tail(void)
{
    const struct config cfg = {6, 1, 0x0F, 0, 1000, 10};
    struct capture cap;
    struct wire w;
    struct got got;

    generate(&cap, &cfg, 0, 4);
    parse(&w, &cap, 0, &got, cap.wr[0].out, cap.wr[0].len);
    CHECK(w.stats.mismatches == 1 && wire_tail_mismatches(&w) == 0);
    free(got.rec.rec);
    wire_free(&w);
    capture_free(&cap);

    generate(&cap, &cfg, 0, 10);
    parse(&w, &cap, 0, &got, cap.wr[0].out, cap.wr[0].len);
    CHECK(w.stats.mismatches == 1 && wire_tail_mismatches(&w) == 1);
    free(got.rec.rec);
    wire_free(&w);
    capture_free(&cap);
}


// The capture info of a device with cfg's channels, from a hello with made-up names.
static void make_info(struct capture_info *info, const struct config *cfg)
{
    static char name_buf[CHANNELS_MAX][8];
    char *names[CHANNELS_MAX];
    struct hello h = {.channels = cfg->channels, .data_channels = cfg->data_channels,
                      .names = names};
    uint8_t enabled[LEVEL_BYTES] = {0};
    uint8_t split[LEVEL_BYTES] = {0};
    int err = 0;

    for (unsigned int ch = 0; ch < cfg->channels; ch++) {
        snprintf(name_buf[ch], sizeof(name_buf[ch]), "ch%u", ch);
        names[ch] = name_buf[ch];
        channel_set_put(enabled, ch, (cfg->enabled >> ch) & 1);
        channel_set_put(split, ch, (cfg->split >> ch) & 1);
    }
    err = capture_info_init(info, &h, 500000, cfg->c, enabled, split);
    CHECK(err == 0);
}


/*
 * Random level changes, 0 to 3 per logical channel per interval at distinct ticks, written
 * in tick order with a heartbeat at every grid point.
 */
static void generate(struct capture *cap, const struct config *cfg, uint64_t first_grid,
                     uint64_t drop_grid)
{
    struct record *batch = NULL;
    size_t batch_count = 0;
    uint64_t *changed = NULL;

    memset(cap, 0, sizeof(*cap));
    make_info(&cap->info, cfg);
    cap->first_grid = first_grid;
    cap->drop_grid = drop_grid;
    cap->nb = (cap->info.n + 7) / 8;
    cap->levels = calloc((size_t)(cfg->grids + 1) * (cap->nb ? cap->nb : 1), 1);
    batch = calloc((size_t)cap->info.n * 3 + 1, sizeof(*batch));
    changed = calloc(4, sizeof(*changed));
    for (uint16_t d = 0; d < cfg->data_channels; d++) {
        CHECK(wire_writer_init(&cap->wr[d], &cap->info, d) == 0);
    }

    for (unsigned int i = 0; i < cap->info.n; i++) {
        channel_set_put(cap->levels, i, rnd() & 1);
    }
    for (unsigned int g = 0; g <= cfg->grids; g++) {
        uint8_t *now = &cap->levels[(size_t)g * cap->nb];
        uint64_t grid = first_grid + g;

        if (g > 0) {
            // The interval before grid point g: changes, then the heartbeat.
            memcpy(now, now - cap->nb, cap->nb);
            batch_count = 0;
            for (unsigned int i = 0; i < cap->info.n; i++) {
                unsigned int m = (unsigned int)(rnd() % 4);
                unsigned int k = 0;

                if (drop_grid && grid == drop_grid && i == 0) {
                    m = 1;
                }
                if (m > cfg->c) {
                    m = cfg->c;
                }
                while (k < m) {
                    uint64_t t = (grid - 1) * cfg->c + 1 + rnd() % cfg->c;
                    bool dup = false;

                    for (unsigned int j = 0; j < k; j++) {
                        dup = dup || changed[j] == t;
                    }
                    if (!dup) {
                        changed[k++] = t;
                    }
                }
                qsort(changed, m, sizeof(*changed), by_tick);
                for (k = 0; k < m; k++) {
                    bool level = !channel_set_has(now, i);

                    channel_set_put(now, i, level);
                    batch[batch_count++] = (struct record){
                        .tick = changed[k], .channel = cap->info.enabled[i], .level = level};
                }
            }
            qsort(batch, batch_count, sizeof(*batch), by_tick);

            for (size_t r = 0; r < batch_count; r++) {
                uint16_t d = 0;
                unsigned int i = 0;

                while (cap->info.enabled[i] != batch[r].channel) {
                    i++;
                }
                while (i >= cap->info.block[d + 1]) {
                    d++;
                }
                if (drop_grid && grid == drop_grid && i == 0 && !cap->dropped.tick) {
                    cap->dropped = batch[r];
                    continue;
                }
                CHECK(wire_write_record(&cap->wr[d], &batch[r]) == 0);
                push(&cap->expect[d], &batch[r]);
            }
        }

        for (uint16_t d = 0; d < cfg->data_channels; d++) {
            CHECK(wire_write_heartbeat(&cap->wr[d], grid, now) == 0);
            if (g < 16) {
                cap->check_at[d][g] = cap->wr[d].len - 1;
            }
        }
    }

    free(batch);
    free(changed);
}


static void capture_free(struct capture *cap)
{
    for (uint16_t d = 0; d < cap->info.data_channels; d++) {
        wire_writer_free(&cap->wr[d]);
        free(cap->expect[d].rec);
    }
    free(cap->levels);
    capture_info_free(&cap->info);
}


// Sets w up for data channel d and feeds it data in chunks of random length.
static void parse(struct wire *w, const struct capture *cap, uint16_t d, struct got *got,
                  const uint8_t *data, size_t len)
{
    const struct wire_handler handler = {
        .record = on_record, .heartbeat = on_heartbeat, .ctx = got};
    size_t at = 0;

    memset(got, 0, sizeof(*got));
    got->cap = cap;
    got->next_grid = cap->first_grid;
    CHECK(wire_init(w, &cap->info, d, &handler) == 0);
    wire_start_at(w, cap->first_grid, cap->first_grid != 0);
    while (at < len) {
        size_t n = 1 + rnd() % 300;

        if (n > len - at) {
            n = len - at;
        }
        wire_feed(w, &data[at], n);
        at += n;
    }
}


static void on_record(void *ctx, const struct record *r)
{
    struct got *got = ctx;

    push(&got->rec, r);
}


// Heartbeats come in grid order (resync may skip some), each with the generated levels.
static void on_heartbeat(void *ctx, const struct heartbeat *hb)
{
    struct got *got = ctx;
    const struct capture *cap = got->cap;
    const uint8_t *want = NULL;
    uint64_t g = hb->grid - cap->first_grid;

    if (hb->grid < got->next_grid || hb->tick != hb->grid * cap->info.c ||
        hb->n != cap->info.n) {
        got->hb_bad++;
        return;
    }
    want = &cap->levels[g * cap->nb];
    for (unsigned int i = 0; i < hb->n; i++) {
        if (heartbeat_level(hb, i) != channel_set_has(want, i)) {
            got->hb_bad++;
            break;
        }
    }
    if (hb->flags & HEARTBEAT_UNVERIFIED) {
        got->hb_unverified++;
    }
    got->next_grid = hb->grid + 1;
}


static bool same(const struct record *a, const struct record *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i].tick != b[i].tick || a[i].channel != b[i].channel ||
            a[i].level != b[i].level) {
            return false;
        }
    }

    return true;
}


static void push(struct list *l, const struct record *r)
{
    if (l->count == l->cap) {
        l->cap = l->cap ? 2 * l->cap : 256;
        l->rec = realloc(l->rec, l->cap * sizeof(*l->rec));
        if (!l->rec) {
            abort();
        }
    }
    l->rec[l->count++] = *r;
}


// Orders records, or plain ticks, by tick: both start with a uint64_t.
static int by_tick(const void *a, const void *b)
{
    uint64_t ta = *(const uint64_t *)a;
    uint64_t tb = *(const uint64_t *)b;

    return ta < tb ? -1 : ta > tb;
}


// xorshift64*: the same numbers on every run.
static uint64_t rnd(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;

    return rng_state * 0x2545F4914F6CDD1DULL;
}
