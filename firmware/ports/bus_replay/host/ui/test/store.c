/*
 * The store on hand-built records, heartbeats and losses: three-state levels (late losses
 * too), lane columns, next and previous changes, positions and merged reads, dropping past
 * the memory limit, truncated spans, the watermark over two data channels, a writer and a
 * reader at once, and the dummy's capture fed through batches. A stream whose every
 * heartbeat disagrees with its records stays within the memory limit.
 */

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "capture/capture.h"
#include "capture/control.h"
#include "capture/dummy.h"
#include "capture/info.h"
#include "common/capture_info.h"
#include "common/record.h"
#include "common/status.h"
#include "ui/store/store.h"
#include "test/test.h"


#define RATE 1000
#define C 10
#define CHANNELS 4
#define SPAN_MAX 64

// A store over CHANNELS channels, all enabled, split into data channels.
struct fix {
    struct capture_info info;
    struct store *s;
    struct store_batch *b;
};


static void fix_open(struct fix *f, uint8_t data_channels, size_t limit);
static void fix_close(struct fix *f);
static void rec(struct fix *f, uint16_t ch, uint64_t tick, uint8_t level);
static void beat(struct fix *f, uint16_t dc, uint64_t tick, uint8_t levels);
static void loss(struct fix *f, uint64_t start, uint64_t end, uint32_t lost0, uint32_t lost1);
static int flush(struct fix *f);
static void test_levels(void);
static void test_late_loss(void);
static void test_columns(void);
static void test_next_prev(void);
static void test_read(void);
static void test_drop(void);
static void test_mismatches(void);
// Every heartbeat contradicts the records, so each is kept: still within the limit.
static void test_mismatches(void)
{
    const size_t limit = 4U << 20;
    const uint64_t steps = 600000;
    struct store_stats st;
    size_t most = 0;
    struct fix f;
    uint64_t t = 0;

    fix_open(&f, 1, limit);
    for (uint64_t i = 0; i < steps; i++) {
        t = 4 * i + 1;
        rec(&f, 0, t, 1);
        beat(&f, 0, t + 1, 0x00);
        if (i % 1000 == 999 || i == steps - 1) {
            CHECK(flush(&f) == 0);
            store_stats(f.s, &st);
            most = st.bytes > most ? st.bytes : most;
        }
    }
    store_stats(f.s, &st);
    CHECK(most <= limit);
    CHECK(st.dropped > 0 && st.total == steps);
    CHECK(store_level(f.s, 0, t) == STORE_HIGH);
    CHECK(store_level(f.s, 0, t + 1) == STORE_LOW);
    CHECK(store_level(f.s, 0, st.oldest) != STORE_UNKNOWN);
    fix_close(&f);
}


static void test_records(void);
static void test_watermark(void);
static void test_threads(void);
static void test_dummy(void);


int main(void)
{
    test_levels();
    test_late_loss();
    test_columns();
    test_next_prev();
    test_read();
    test_drop();
    test_mismatches();
    test_records();
    test_watermark();
    test_threads();
    test_dummy();

    return test_result(__FILE__);
}


static void fix_open(struct fix *f, uint8_t data_channels, size_t limit)
{
    static char *names[CHANNELS] = {"CH1", "CH2", "CH3", "CH4"};
    struct hello h = {.channels = CHANNELS, .data_channels = data_channels, .names = names};
    struct store_options opt = {.limit = limit};
    uint8_t enabled = (1U << CHANNELS) - 1;

    memset(f, 0, sizeof(*f));
    CHECK(capture_info_init(&f->info, &h, RATE, C, &enabled, NULL) == 0);
    CHECK(store_new(&f->s, &f->info, &opt) == 0);
    CHECK(store_batch_new(&f->b, &f->info) == 0);
}


static void fix_close(struct fix *f)
{
    store_batch_free(f->b);
    store_free(f->s);
    capture_info_free(&f->info);
}


static void rec(struct fix *f, uint16_t ch, uint64_t tick, uint8_t level)
{
    struct record r = {.tick = tick, .channel = ch, .level = level};

    store_batch_record(f->b, &r);
}


// levels: bit i for logical channel i.
static void beat(struct fix *f, uint16_t dc, uint64_t tick, uint8_t levels)
{
    struct heartbeat hb = {.tick = tick, .data_channel = dc, .n = CHANNELS, .levels = &levels};

    store_batch_heartbeat(f->b, &hb);
}


static void loss(struct fix *f, uint64_t start, uint64_t end, uint32_t lost0, uint32_t lost1)
{
    uint32_t lost[CHANNELS] = {lost0, lost1, 0, 0};
    struct loss l = {.start = start, .end = end, .n = CHANNELS, .lost = lost};

    store_batch_loss(f->b, &l);
}


static int flush(struct fix *f)
{
    int err = store_write(f->s, f->b);

    store_batch_clear(f->b);
    return err;
}


// Known from the first record or heartbeat; a mismatching heartbeat; a loss and after it.
static void test_levels(void)
{
    struct store_loss out[4];
    uint32_t lost[4 * CHANNELS];
    struct store_stats st;
    struct fix f;
    size_t n = 0;

    fix_open(&f, 1, 0);
    CHECK(store_level(f.s, 0, 0) == -ERANGE);
    CHECK(store_level(f.s, 9, 0) == -EINVAL);

    rec(&f, 0, 10, 1);
    rec(&f, 0, 20, 0);
    beat(&f, 0, 30, 0x02);
    CHECK(flush(&f) == 0);
    CHECK(store_level(f.s, 0, 5) == STORE_UNKNOWN);
    CHECK(store_level(f.s, 0, 9) == STORE_UNKNOWN);
    CHECK(store_level(f.s, 0, 10) == STORE_HIGH);
    CHECK(store_level(f.s, 0, 19) == STORE_HIGH);
    CHECK(store_level(f.s, 0, 20) == STORE_LOW);
    CHECK(store_level(f.s, 0, 30) == STORE_LOW);
    CHECK(store_level(f.s, 1, 29) == STORE_UNKNOWN);
    CHECK(store_level(f.s, 1, 30) == STORE_HIGH);
    CHECK(store_level(f.s, 0, 31) == -ERANGE);

    // The records say low, the heartbeat high: the heartbeat wins from the end of its tick.
    beat(&f, 0, 40, 0x03);
    CHECK(flush(&f) == 0);
    CHECK(store_level(f.s, 0, 39) == STORE_LOW);
    CHECK(store_level(f.s, 0, 40) == STORE_HIGH);

    // A loss on channel 1: unknown from its start until a heartbeat after its end.
    loss(&f, 50, 60, 0, 3);
    beat(&f, 0, 55, 0x01);
    beat(&f, 0, 70, 0x01);
    CHECK(flush(&f) == 0);
    CHECK(store_level(f.s, 1, 49) == STORE_HIGH);
    CHECK(store_level(f.s, 1, 50) == STORE_UNKNOWN);
    CHECK(store_level(f.s, 1, 69) == STORE_UNKNOWN);
    CHECK(store_level(f.s, 1, 70) == STORE_LOW);
    CHECK(store_level(f.s, 0, 55) == STORE_HIGH);

    // A record after a loss makes the level known too.
    loss(&f, 75, 78, 2, 0);
    rec(&f, 0, 76, 0);
    rec(&f, 0, 80, 0);
    beat(&f, 0, 90, 0x00);
    CHECK(flush(&f) == 0);
    CHECK(store_level(f.s, 0, 79) == STORE_UNKNOWN);
    CHECK(store_level(f.s, 0, 80) == STORE_LOW);

    CHECK(store_losses(f.s, 0, 100, out, lost, 4, &n) == 0);
    CHECK(n == 2);
    CHECK(out[0].start == 50 && out[0].end == 60 && out[0].lost == 3);
    CHECK(lost[0] == 0 && lost[1] == 3);
    CHECK(out[1].start == 75 && lost[CHANNELS] == 2 && lost[CHANNELS + 1] == 0);
    CHECK(store_losses(f.s, 61, 74, out, NULL, 4, &n) == 0 && n == 0);
    store_stats(f.s, &st);
    CHECK(st.losses == 2 && st.records == 4 && st.watermark == 90 && st.have_watermark);
    fix_close(&f);
}


// A loss reported after the heartbeats past its end: one held back makes the level known.
static void test_late_loss(void)
{
    struct fix f;

    fix_open(&f, 1, 0);
    rec(&f, 0, 5, 1);
    beat(&f, 0, 80, 0x01);
    beat(&f, 0, 90, 0x01);
    rec(&f, 0, 95, 0);
    beat(&f, 0, 100, 0x00);
    CHECK(flush(&f) == 0);
    CHECK(store_level(f.s, 0, 84) == STORE_HIGH);

    loss(&f, 82, 85, 4, 0);
    CHECK(flush(&f) == 0);
    CHECK(store_level(f.s, 0, 81) == STORE_HIGH);
    CHECK(store_level(f.s, 0, 82) == STORE_UNKNOWN);
    CHECK(store_level(f.s, 0, 89) == STORE_UNKNOWN);
    CHECK(store_level(f.s, 0, 90) == STORE_HIGH);
    CHECK(store_level(f.s, 0, 95) == STORE_LOW);
    // Channel 1 lost nothing.
    CHECK(store_level(f.s, 1, 84) == STORE_LOW);

    // A late loss with nothing after it yet stays unknown until something comes.
    loss(&f, 98, 120, 1, 0);
    CHECK(flush(&f) == 0);
    CHECK(store_level(f.s, 0, 100) == STORE_UNKNOWN);
    beat(&f, 0, 130, 0x01);
    CHECK(flush(&f) == 0);
    CHECK(store_level(f.s, 0, 129) == STORE_UNKNOWN);
    CHECK(store_level(f.s, 0, 130) == STORE_HIGH);
    fix_close(&f);
}


static void test_columns(void)
{
    uint64_t edges[6] = {0, 10, 20, 30, 40, 60};
    uint16_t chans[2] = {0, 1};
    struct store_column col[10];
    struct fix f;

    fix_open(&f, 1, 0);
    beat(&f, 0, 0, 0x00);
    rec(&f, 0, 12, 1);
    rec(&f, 0, 13, 0);
    rec(&f, 0, 14, 1);
    rec(&f, 0, 25, 0);
    loss(&f, 32, 33, 0, 1);
    beat(&f, 0, 35, 0x00);
    beat(&f, 0, 45, 0x00);
    CHECK(flush(&f) == 0);

    CHECK(store_columns(f.s, chans, 2, edges, 5, col) == 0);
    // Channel 0: low, three changes ending high, one change to low, then flat.
    CHECK(col[0].start_level == STORE_UNKNOWN && col[0].end_level == STORE_LOW);
    CHECK(col[0].changes == 0 && col[0].flags == 0);
    CHECK(col[1].start_level == STORE_LOW && col[1].end_level == STORE_HIGH);
    CHECK(col[1].changes == 3);
    CHECK(col[2].changes == 1 && col[2].end_level == STORE_LOW);
    CHECK(col[3].changes == 0 && col[3].flags == 0);
    // Past the watermark (45): not held.
    CHECK(col[4].flags == STORE_COL_NO_DATA);
    // Channel 1: the loss and its unknown level, known again at the heartbeat at 35.
    CHECK(col[5 + 3].flags == (STORE_COL_LOSS | STORE_COL_UNKNOWN));
    CHECK(col[5 + 3].start_level == STORE_LOW && col[5 + 3].end_level == STORE_LOW);
    CHECK(col[5 + 2].flags == 0);

    // A column of no ticks; edges out of order.
    edges[2] = 10;
    CHECK(store_columns(f.s, chans, 1, edges, 2, col) == 0);
    CHECK(col[1].changes == 0 && col[1].start_level == col[1].end_level);
    edges[2] = 5;
    CHECK(store_columns(f.s, chans, 1, edges, 2, col) == -EINVAL);
    chans[1] = 7;
    CHECK(store_columns(f.s, chans, 2, edges, 1, col) == -EINVAL);
    fix_close(&f);
}


static void test_next_prev(void)
{
    uint16_t chans[2] = {1, 0};
    struct record r;
    struct fix f;

    fix_open(&f, 1, 0);
    rec(&f, 0, 10, 1);
    rec(&f, 1, 10, 1);
    rec(&f, 1, 15, 0);
    rec(&f, 0, 30, 0);
    beat(&f, 0, 20, 0x01);
    CHECK(flush(&f) == 0);

    // A record not after the channel's last one is left out; the rest is taken.
    rec(&f, 0, 30, 1);
    beat(&f, 0, 40, 0x00);
    CHECK(flush(&f) == -EINVAL);
    CHECK(store_level(f.s, 0, 30) == STORE_LOW);
    CHECK(store_next(f.s, chans, 2, 0, &r) == 0 && r.tick == 10 && r.channel == 0);
    CHECK(store_next(f.s, chans, 2, 10, &r) == 0 && r.tick == 15 && r.channel == 1);
    CHECK(store_next(f.s, chans, 1, 15, &r) == -ENOENT);
    CHECK(store_next(f.s, chans + 1, 1, 15, &r) == 0 && r.tick == 30 && r.level == 0);
    CHECK(store_prev(f.s, chans, 2, 30, &r) == 0 && r.tick == 15);
    CHECK(store_prev(f.s, chans, 2, 15, &r) == 0 && r.tick == 10 && r.channel == 0);
    CHECK(store_prev(f.s, chans, 2, 10, &r) == -ENOENT);

    // Nothing past the watermark.
    rec(&f, 1, 50, 1);
    CHECK(flush(&f) == 0);
    CHECK(store_next(f.s, chans, 1, 30, &r) == -ENOENT);
    fix_close(&f);
}


static void test_read(void)
{
    uint64_t pos[CHANNELS] = {0};
    struct record out[16];
    struct store_channel_stats cs;
    struct fix f;
    uint64_t p = 0;
    size_t n = 0;

    fix_open(&f, 1, 0);
    for (uint64_t t = 1; t <= 6; t++) {
        rec(&f, t % 2, t * 10, t % 3 == 0);
    }
    beat(&f, 0, 45, 0x00);
    CHECK(flush(&f) == 0);

    CHECK(store_read(f.s, 0, &p, out, 2, &n) == 0 && n == 2 && p == 2);
    CHECK(out[0].tick == 20 && out[1].tick == 40);
    CHECK(store_read(f.s, 0, &p, out, 16, &n) == 0 && n == 1 && p == 3 && out[0].tick == 60);
    CHECK(store_read(f.s, 0, &p, out, 16, &n) == 0 && n == 0);
    CHECK(store_channel_stats(f.s, 0, &cs) == 0 && cs.next == p && cs.first == 0);
    p = 4;
    CHECK(store_read(f.s, 0, &p, out, 16, &n) == -ERANGE);

    // Merged: up to the watermark only, in tick order.
    CHECK(store_read_merged(f.s, pos, out, 16, &n) == 0 && n == 4);
    CHECK(out[0].tick == 10 && out[1].tick == 20 && out[2].tick == 30 && out[3].tick == 40);
    CHECK(pos[0] == 2 && pos[1] == 2);
    beat(&f, 0, 100, 0x00);
    CHECK(flush(&f) == 0);
    CHECK(store_read_merged(f.s, pos, out, 16, &n) == 0 && n == 2 && out[1].tick == 60);
    fix_close(&f);
}


// A limit of nothing keeps only the blocks being written.
static void test_drop(void)
{
    const uint64_t count = 3 * STORE_BLOCK_RECORDS + 10;
    uint16_t ch = 0;
    struct store_channel_stats cs;
    struct store_column col;
    struct store_stats st;
    struct record out[4];
    uint64_t edges[2];
    struct fix f;
    uint64_t p = 0;
    size_t n = 0;

    fix_open(&f, 1, 1);
    for (uint64_t i = 0; i < count; i++) {
        rec(&f, 0, 2 * i + 1, i % 2 == 0);
    }
    rec(&f, 1, 5, 1);
    beat(&f, 0, 2 * count + 1, 0x02);
    CHECK(flush(&f) == 0);

    store_stats(f.s, &st);
    CHECK(st.records == 10 + 1 && st.total == count + 1 && st.dropped == count - 10);
    // The two blocks being written, and a little kept beside them.
    CHECK(st.bytes >= 2 * STORE_BLOCK_RECORDS * sizeof(uint64_t));
    CHECK(st.bytes < 2 * STORE_BLOCK_RECORDS * sizeof(uint64_t) + 65536);
    CHECK(st.oldest == 2 * (3 * STORE_BLOCK_RECORDS - 1) + 1);
    CHECK(store_channel_stats(f.s, 0, &cs) == 0 && cs.first == 3 * STORE_BLOCK_RECORDS);
    CHECK(store_read(f.s, 0, &p, out, 4, &n) == -ERANGE);
    p = cs.first;
    CHECK(store_read(f.s, 0, &p, out, 4, &n) == 0 && n == 4 && out[0].tick == 2 * cs.first + 1);

    // The level at the oldest tick held is the last dropped record's.
    CHECK(store_level(f.s, 0, st.oldest) == STORE_LOW);
    CHECK(store_level(f.s, 0, st.oldest - 1) == -ERANGE);
    CHECK(store_level(f.s, 1, st.oldest) == STORE_HIGH);
    edges[0] = st.oldest - 10;
    edges[1] = st.oldest + 10;
    CHECK(store_columns(f.s, &ch, 1, edges, 1, &col) == 0);
    CHECK(col.flags & STORE_COL_NO_DATA);
    CHECK(col.start_level == STORE_UNKNOWN);
    fix_close(&f);
}


static void test_records(void)
{
    struct record out[3];
    struct fix f;
    bool more = false;
    size_t n = 0;

    fix_open(&f, 1, 0);
    for (uint64_t t = 1; t <= 5; t++) {
        rec(&f, 2, t * 10, t % 2);
    }
    CHECK(flush(&f) == 0);
    CHECK(store_records(f.s, 2, 15, 50, out, 3, &n, &more) == 0 && n == 3 && more);
    CHECK(out[0].tick == 20 && out[2].tick == 40 && out[0].channel == 2);
    CHECK(store_records(f.s, 2, out[2].tick + 1, 50, out, 3, &n, &more) == 0);
    CHECK(n == 1 && !more && out[0].tick == 50);
    CHECK(store_records(f.s, 2, 51, 40, out, 3, &n, &more) == 0 && n == 0 && !more);
    fix_close(&f);
}


// The watermark is the oldest of the data channels' last heartbeats.
static void test_watermark(void)
{
    struct store_channel_stats cs;
    struct store_stats st;
    struct fix f;

    fix_open(&f, 2, 0);
    CHECK(f.info.block[1] == 2);
    beat(&f, 0, 100, 0x00);
    CHECK(flush(&f) == 0);
    store_stats(f.s, &st);
    CHECK(!st.have_watermark);
    CHECK(store_channel_stats(f.s, 0, &cs) == 0 && cs.have_arrived && cs.arrived == 100);
    CHECK(store_channel_stats(f.s, 3, &cs) == 0 && !cs.have_arrived);

    rec(&f, 3, 40, 1);
    beat(&f, 1, 50, 0x08);
    CHECK(flush(&f) == 0);
    store_stats(f.s, &st);
    CHECK(st.have_watermark && st.watermark == 50 && st.newest == 40);
    CHECK(store_level(f.s, 2, 50) == STORE_LOW);
    CHECK(store_level(f.s, 2, 60) == -ERANGE);
    CHECK(store_level(f.s, 3, 45) == STORE_HIGH);
    // Channel 0's level is known from its data channel's heartbeat at 100 only.
    CHECK(store_level(f.s, 0, 60) == STORE_UNKNOWN);
    CHECK(store_level(f.s, 0, 100) == STORE_LOW);
    fix_close(&f);
}


struct shared {
    struct fix *f;
    volatile bool done;
    unsigned int reads;
    bool ok;
};


static void *reader(void *arg)
{
    struct shared *sh = arg;
    uint64_t pos[CHANNELS] = {0};
    uint16_t chans[CHANNELS] = {0, 1, 2, 3};
    uint64_t edges[201];
    struct store_column col[CHANNELS * 200];
    struct record out[256];
    uint64_t last_total = 0;

    while (!sh->done) {
        struct store_stats st;
        size_t n = 0;

        store_stats(sh->f->s, &st);
        if (st.total < last_total || st.records > st.total) {
            sh->ok = false;
        }
        last_total = st.total;
        for (size_t x = 0; x <= 200; x++) {
            edges[x] = st.watermark * x / 200;
        }
        if (store_columns(sh->f->s, chans, CHANNELS, edges, 200, col) < 0) {
            sh->ok = false;
        }
        if (store_read_merged(sh->f->s, pos, out, 256, &n) < 0) {
            sh->ok = false;
        }
        for (size_t i = 1; i < n; i++) {
            if (out[i].tick < out[i - 1].tick) {
                sh->ok = false;
            }
        }
        sh->reads++;
    }

    return NULL;
}


// A writer and a reader at once: the reader sees counts grow and merged reads in order.
static void test_threads(void)
{
    struct shared sh = {.ok = true};
    struct store_stats st;
    pthread_t th;
    struct fix f;
    uint64_t t = 0;

    fix_open(&f, 1, 0);
    sh.f = &f;
    CHECK(pthread_create(&th, NULL, reader, &sh) == 0);
    for (unsigned int batch = 0; batch < 2000; batch++) {
        for (unsigned int i = 0; i < 100; i++) {
            t++;
            rec(&f, t % CHANNELS, t, (t / CHANNELS) % 2);
        }
        beat(&f, 0, t, 0x00);
        if (batch % 100 == 0) {
            loss(&f, t - 50, t - 40, 1, 1);
        }
        CHECK(flush(&f) == 0);
    }
    sh.done = true;
    pthread_join(th, NULL);
    CHECK(sh.ok);
    CHECK(sh.reads > 0);
    store_stats(f.s, &st);
    CHECK(st.total == t && st.watermark == t);
    fix_close(&f);
}


// The handler's context: the batch, once the capture has started and the store exists.
struct feed {
    struct store_batch *b;
};


static void on_record(void *ctx, const struct record *r)
{
    struct feed *fd = ctx;

    if (fd->b) {
        store_batch_record(fd->b, r);
    }
}


static void on_heartbeat(void *ctx, const struct heartbeat *hb)
{
    struct feed *fd = ctx;

    if (fd->b) {
        store_batch_heartbeat(fd->b, hb);
    }
}


static void on_loss(void *ctx, const struct loss *l)
{
    struct feed *fd = ctx;

    if (fd->b) {
        store_batch_loss(fd->b, l);
    }
}


// Three seconds of the dummy, a batch per pull: the store holds what the session counted.
static void test_dummy(void)
{
    struct dummy_options opt = {.realtime = false};
    struct feed fd = {0};
    struct capture_handler h = {.record = on_record, .heartbeat = on_heartbeat,
                                .loss = on_loss, .ctx = &fd};
    struct capture_config cfg = {0};
    const struct capture_info *info = NULL;
    struct capture *cap = NULL;
    struct store *s = NULL;
    struct capture_status cst;
    struct store_stats st;
    char error[256];
    int err = 0;

    CHECK(capture_open_dummy(&cap, &opt, &h) == 0);
    CHECK(capture_start(cap, &cfg, error, sizeof(error)) == 0);
    info = capture_info(cap);
    CHECK(store_new(&s, info, NULL) == 0);
    CHECK(store_batch_new(&fd.b, info) == 0);
    do {
        CHECK(capture_pull(cap, 0) >= 0);
        err = store_write(s, fd.b);
        CHECK(err == 0);
        store_batch_clear(fd.b);
        capture_status(cap, &cst);
    } while (err == 0 && cst.now < 3ULL * info->rate);

    store_stats(s, &st);
    CHECK(st.total > 1000);
    CHECK(st.losses == cst.losses);
    CHECK(st.have_watermark);
    for (uint16_t i = 0; i < info->n; i++) {
        struct store_channel_stats cs;

        CHECK(store_channel_stats(s, info->enabled[i], &cs) == 0);
        CHECK(cs.next == capture_channel_records(cap)[i]);
    }
    CHECK(capture_stop(cap, THC_REASON_DURATION) == 0);
    store_free(s);
    store_batch_free(fd.b);
    capture_close(cap);
}
