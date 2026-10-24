/*
 * Decoding a dummy capture file: the dummy's signals are known, so every item away from its
 * losses must be exactly what it sent. I2C: an SHT3x measurement every 20 ms, a write of 24
 * 00 to 0x44 then a read of six bytes, the last one NAKed by the master. UART: "TH-1
 * dummy\r\n" every 100 ms. Its losses fall over the end of an I2C measurement, which must
 * be marked.
 *
 * Also: parsing from an index entry (capture_seek()) gives the same items as parsing the
 * whole file, from a little after the seek point on.
 *
 * Files go to a new temporary directory.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "capture/capture.h"
#include "capture/dummy.h"
#include "capture/index.h"
#include "capture/thcfile.h"
#include "codec/codec.h"
#include "codec/i2c/i2c.h"
#include "codec/item.h"
#include "codec/uart/uart.h"
#include "common/capture_info.h"
#include "common/record.h"
#include "common/status.h"
#include "test/test.h"


#define SECONDS 4

// Items in order of release, and the capture's length in seconds.
struct items {
    struct codec_item **item;
    size_t count;
    size_t cap;
    double seconds;
};

struct run {
    struct codec *codec;
    struct items *out;
};


static void record_dummy(const char *path);
static void decode(const char *path, uint64_t seek, struct items *out);
static void on_record(void *ctx, const struct record *r);
static void on_heartbeat(void *ctx, const struct heartbeat *hb);
static void on_loss(void *ctx, const struct loss *loss);
static void collect(struct codec *c, struct items *out);
static void items_free(struct items *it);
static bool words_are(const struct codec_item *it, const uint8_t *v, size_t n);
static void check_items(const struct items *all);
static void check_seek(const struct items *all, const struct items *part, uint64_t from);
static bool same_item(const struct codec_item *a, const struct codec_item *b);


int main(void)
{
    struct items all = {0};
    struct items part = {0};
    char path[600];

    snprintf(path, sizeof(path), "%s/decode.thcapture", test_dir());

    record_dummy(path);
    decode(path, 0, &all);
    check_items(&all);
    // Half-way in.
    decode(path, 2 * 500000, &part);
    check_seek(&all, &part, 2 * 500000);
    items_free(&all);
    items_free(&part);

    return test_result(__FILE__);
}


// SECONDS of the dummy's four bench channels, as fast as it makes them.
static void record_dummy(const char *path)
{
    struct dummy_options opt = {.realtime = false};
    uint8_t enabled = 0x0F;
    struct capture_config cfg = {.enabled = &enabled};
    struct capture *cap = NULL;
    struct capture_status st;
    char *ipath = index_path(path);
    char error[256];

    unlink(path);
    if (ipath) {
        unlink(ipath);
    }
    free(ipath);
    CHECK(capture_open_dummy(&cap, &opt, NULL) == 0);
    if (!cap) {
        return;
    }
    CHECK(capture_record(cap, path, NULL) == 0);
    CHECK(capture_start(cap, &cfg, error, sizeof(error)) == 0);
    do {
        CHECK(capture_pull(cap, 0) >= 0);
        capture_status(cap, &st);
    } while (st.now < (uint64_t)SECONDS * capture_info(cap)->rate);
    CHECK(capture_stop(cap, THC_REASON_DURATION) == 0);
    capture_close(cap);
}


// Decodes the file, from its index entry at or before seek when seek is not 0.
static void decode(const char *path, uint64_t seek, struct items *out)
{
    struct run run = {.out = out};
    struct capture_handler h = {.record = on_record, .heartbeat = on_heartbeat,
                                .loss = on_loss, .ctx = &run};
    struct capture *cap = NULL;
    const struct capture_info *info = NULL;
    struct capture_status st;
    char error[CODEC_ERROR_LEN];
    int err = 0;

    CHECK(capture_open_file(&cap, path, &h) == 0);
    if (!cap) {
        return;
    }
    info = capture_info(cap);
    CHECK(codec_new(&run.codec, info, info->rate) == 0);
    CHECK(codec_add(run.codec, codec_find("i2c"), "scl=0,sda=1", NULL, NULL, error,
                    sizeof(error)) == 0);
    CHECK(codec_add(run.codec, codec_find("uart"), "tx=3,baud=115200", NULL, NULL, error,
                    sizeof(error)) == 0);
    if (seek) {
        CHECK(capture_seek(cap, seek) == 0);
    }
    while ((err = capture_pull(cap, 0)) > 0) {
        collect(run.codec, out);
    }
    CHECK(err == -ENODATA);
    capture_status(cap, &st);
    out->seconds = (double)st.now / info->rate;
    codec_finish(run.codec);
    collect(run.codec, out);
    CHECK(codec_error(run.codec) == 0);
    codec_free(run.codec);
    capture_close(cap);
}


static void on_record(void *ctx, const struct record *r)
{
    struct run *run = ctx;

    CHECK(codec_record(run->codec, r) == 0);
}


static void on_heartbeat(void *ctx, const struct heartbeat *hb)
{
    struct run *run = ctx;

    CHECK(codec_heartbeat(run->codec, hb) == 0);
}


static void on_loss(void *ctx, const struct loss *loss)
{
    struct run *run = ctx;

    codec_loss(run->codec, loss);
}


static void collect(struct codec *c, struct items *out)
{
    struct codec_item *it = NULL;

    while ((it = codec_next(c))) {
        if (out->count == out->cap) {
            size_t cap = out->cap ? 2 * out->cap : 256;
            struct codec_item **bigger = realloc(out->item, cap * sizeof(*bigger));

            if (!bigger) {
                codec_item_free(it);
                CHECK(false);
                return;
            }
            out->item = bigger;
            out->cap = cap;
        }
        out->item[out->count++] = it;
    }
}


static void items_free(struct items *it)
{
    for (size_t i = 0; i < it->count; i++) {
        codec_item_free(it->item[i]);
    }
    free(it->item);
    memset(it, 0, sizeof(*it));
}


static bool words_are(const struct codec_item *it, const uint8_t *v, size_t n)
{
    if (it->word_count != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (it->words[i].value != v[i]) {
            return false;
        }
    }

    return true;
}


static void check_items(const struct items *all)
{
    static const uint8_t cmd[] = {0x88, 0x24, 0x00};
    static const uint8_t data[] = {0x89, 0x63, 0x52, 0xE5, 0x8F, 0x3A, 0x25};
    static const char text[] = "TH-1 dummy\r\n";
    unsigned int writes = 0;
    unsigned int reads = 0;
    unsigned int bursts = 0;
    unsigned int lossy = 0;
    unsigned int wrong = 0;
    uint64_t last = 0;

    for (size_t i = 0; i < all->count; i++) {
        const struct codec_item *it = all->item[i];

        // In order of start, across the two decoders.
        CHECK(it->start >= last);
        last = it->start;
        if (it->flags & CODEC_ITEM_LOSS) {
            lossy++;
            continue;
        }
        if (it->kind == &i2c_transaction) {
            const struct codec_word *w = it->words;

            if (words_are(it, cmd, sizeof(cmd)) && !strcmp(it->values[I2C_STATUS].s, "ACK")) {
                writes++;
            } else if (words_are(it, data, sizeof(data)) &&
                       !strcmp(it->values[I2C_STATUS].s, "ACK") && w[6].lane == I2C_TARGET &&
                       (w[6].flags & I2C_WORD_NAK) && !(w[5].flags & I2C_WORD_NAK)) {
                reads++;
            } else {
                wrong++;
            }
        } else if (it->kind == &uart_burst) {
            if (words_are(it, (const uint8_t *)text, strlen(text)) &&
                !it->values[UART_ERRORS].set) {
                bursts++;
            } else {
                wrong++;
            }
        }
    }

    // A measurement every 20 ms, a line every 100 ms; each loss costs one or two items.
    CHECK(all->seconds >= SECONDS);
    CHECK(writes + lossy + 1 >= (unsigned int)(all->seconds * 50));
    CHECK(writes <= all->seconds * 50 + 1);
    CHECK(reads + lossy + 1 >= (unsigned int)(all->seconds * 50));
    CHECK(reads <= all->seconds * 50 + 1);
    CHECK(bursts + lossy + 1 >= (unsigned int)(all->seconds * 10));
    CHECK(bursts <= all->seconds * 10 + 1);
    CHECK(lossy >= 1);
    CHECK(wrong == 0);
    if (wrong || lossy == 0) {
        fprintf(stderr, "writes %u reads %u bursts %u lossy %u wrong %u\n", writes, reads,
                bursts, lossy, wrong);
    }
}


/*
 * Parsing from the seek point gives the items of a full parse that start well after it;
 * the first ones after the seek point may be missing or cut.
 */
static void check_seek(const struct items *all, const struct items *part, uint64_t from)
{
    uint64_t settled = from + 500000 / 10;
    size_t i = 0;
    size_t k = 0;
    size_t compared = 0;

    while (i < all->count && all->item[i]->start < settled) {
        i++;
    }
    while (k < part->count && part->item[k]->start < settled) {
        k++;
    }
    for (; i < all->count && k < part->count; i++, k++) {
        CHECK(same_item(all->item[i], part->item[k]));
        compared++;
    }
    CHECK(i == all->count && k == part->count);
    CHECK(compared > 100);
    // Nothing from long before the seek point.
    CHECK(part->count > 0 && part->item[0]->start + 500000 > from);
}


static bool same_item(const struct codec_item *a, const struct codec_item *b)
{
    if (a->kind != b->kind || a->start != b->start || a->end != b->end ||
        a->flags != b->flags || a->word_count != b->word_count) {
        return false;
    }
    for (size_t i = 0; i < a->word_count; i++) {
        if (a->words[i].value != b->words[i].value || a->words[i].flags != b->words[i].flags) {
            return false;
        }
    }

    return true;
}
