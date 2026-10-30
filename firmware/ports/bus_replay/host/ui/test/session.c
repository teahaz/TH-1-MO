/*
 * The session on the dummy and on a capture file: commands and their results, the store
 * filling, views (a new capture's gen, an old view valid until released), recording, and
 * decoder groups, live and added later, which decode the store's history to the same
 * items.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "codec/item.h"
#include "ui/session/items.h"
#include "ui/session/session.h"
#include "ui/store/store.h"
#include "test/test.h"


#define WAIT_MS 20000

// The text of a group's items, for comparing two groups.
struct dump {
    FILE *f;
    char *text;
    size_t len;
    size_t count;
};


static bool wait_done(struct session *s, int64_t id, struct session_status *st);
static bool wait_records(struct session *s, uint64_t records);
static bool wait_decoded(struct session *s);
static uint32_t group_of(struct session *s, const char *label);
static void dump_item(void *ctx, const struct codec_item *it);
static void dump_group(struct session *s, const char *label, struct dump *d);
static void sleep_ms(unsigned int ms);
static void test_commands(void);
static void test_views(void);
static void test_groups(const char *dir);


int main(void)
{
    const char *dir = test_dir();

    test_commands();
    test_views();
    test_groups(dir);

    return test_result(__FILE__);
}


static bool wait_done(struct session *s, int64_t id, struct session_status *st)
{
    for (int i = 0; i < WAIT_MS / 5; i++) {
        session_status(s, st);
        if (id > 0 && st->done >= (uint64_t)id) {
            return true;
        }
        sleep_ms(5);
    }

    return false;
}


static bool wait_records(struct session *s, uint64_t records)
{
    for (int i = 0; i < WAIT_MS / 5; i++) {
        struct session_view *v = session_acquire(s);
        struct store_stats st = {0};

        if (v && v->store) {
            store_stats(v->store, &st);
        }
        session_release(s, v);
        if (st.total >= records) {
            return true;
        }
        sleep_ms(5);
    }

    return false;
}


// Every group has decoded up to the store's watermark.
static bool wait_decoded(struct session *s)
{
    for (int i = 0; i < WAIT_MS / 5; i++) {
        struct session_view *v = session_acquire(s);
        struct store_stats st = {0};
        struct session_decoder decs[SESSION_DECODERS_MAX];
        size_t n = session_decoders(s, decs, SESSION_DECODERS_MAX);
        bool all = true;

        if (v && v->store) {
            store_stats(v->store, &st);
        }
        session_release(s, v);
        for (size_t k = 0; k < n; k++) {
            all = all && decs[k].decoded >= st.watermark;
        }
        if (all && st.have_watermark) {
            return true;
        }
        sleep_ms(5);
    }

    return false;
}


static uint32_t group_of(struct session *s, const char *label)
{
    struct session_decoder decs[SESSION_DECODERS_MAX];
    size_t n = session_decoders(s, decs, SESSION_DECODERS_MAX);

    for (size_t i = 0; i < n; i++) {
        if (!strcmp(decs[i].label, label)) {
            return decs[i].group;
        }
    }

    return 0;
}


static void dump_item(void *ctx, const struct codec_item *it)
{
    struct dump *d = ctx;

    // The decoder's label differs between the groups compared: it is left out.
    fprintf(d->f, "%llu %llu %x %zu\n", (unsigned long long)it->start,
            (unsigned long long)it->end, it->flags, it->word_count);
    for (size_t i = 0; i < it->word_count; i++) {
        fprintf(d->f, " %llx", (unsigned long long)it->words[i].value);
    }
    fprintf(d->f, "\n");
    for (const struct codec_item *ch = it->child; ch; ch = ch->next) {
        fprintf(d->f, "  %llu %llu %x\n", (unsigned long long)ch->start,
                (unsigned long long)ch->end, ch->flags);
    }
    d->count++;
}


// Every item the group holds, oldest first, as text.
static void dump_group(struct session *s, const char *label, struct dump *d)
{
    struct session_view *v = session_acquire(s);

    memset(d, 0, sizeof(*d));
    d->f = open_memstream(&d->text, &d->len);
    if (v && v->items) {
        items_span(v->items, group_of(s, label), 0, UINT64_MAX, SIZE_MAX, dump_item, d);
    }
    session_release(s, v);
    fclose(d->f);
}


static void sleep_ms(unsigned int ms)
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};

    nanosleep(&ts, NULL);
}


// Commands run in order; what fails says why in the status.
static void test_commands(void)
{
    struct session_open o = {.backend = SESSION_DUMMY};
    struct session_status st;
    struct session *s = NULL;
    int64_t id = 0;

    CHECK(session_new(&s, NULL) == 0);
    session_status(s, &st);
    CHECK(st.state == SESSION_CLOSED && session_acquire(s) == NULL);

    id = session_start(s, NULL);
    CHECK(wait_done(s, id, &st) && st.result == -ENODEV && st.text[0]);
    id = session_open(s, &o);
    CHECK(wait_done(s, id, &st) && st.result == 0 && st.state == SESSION_READY);
    {
        struct session_view *v = session_acquire(s);

        CHECK(v && v->hello && v->hello->channels == 6 && !v->store);
        session_release(s, v);
    }
    id = session_decoder_add(s, "nope", "", NULL, 0);
    CHECK(wait_done(s, id, &st) && st.result == -ENOENT && strstr(st.text, "nope"));
    id = session_decoder_add(s, "ina219", "addr=0x43", "missing", 0);
    CHECK(wait_done(s, id, &st) && st.result == -ENOENT);
    id = session_decoder_add(s, "ina219", "addr=0x43", NULL, 0);
    CHECK(wait_done(s, id, &st) && st.result == -EINVAL);

    // A decoder that can't be made on this capture: the start goes on, the group says why.
    id = session_decoder_add(s, "i2c", "scl=0,sda=9,name=bad", NULL, 0);
    CHECK(wait_done(s, id, &st) && st.result == 0);
    id = session_start(s, NULL);
    CHECK(wait_done(s, id, &st) && st.result == 0 && st.state == SESSION_RUNNING);
    {
        struct session_decoder d[SESSION_DECODERS_MAX];

        CHECK(session_decoders(s, d, SESSION_DECODERS_MAX) == 1 && d[0].error < 0 &&
              d[0].text[0]);
    }
    id = session_decoder_add(s, "i2c", "scl=0,sda=1,name=bad", NULL, 0);
    CHECK(wait_done(s, id, &st) && st.result == -EEXIST);
    id = session_decoder_remove(s, "bad");
    CHECK(wait_done(s, id, &st) && st.result == 0);
    id = session_decoder_remove(s, "bad");
    CHECK(wait_done(s, id, &st) && st.result == -ENOENT);
    id = session_stop(s);
    CHECK(wait_done(s, id, &st) && st.result == 0 && st.state == SESSION_ENDED);
    id = session_close(s);
    CHECK(wait_done(s, id, &st) && st.result == 0);
    session_free(s);
}


// The store fills; a new capture is a new view, and the old one stays until released.
static void test_views(void)
{
    struct session_open o = {.backend = SESSION_DUMMY};
    struct session_status st;
    struct session_view *old = NULL;
    struct session_view *v = NULL;
    struct store_stats ss;
    struct session *s = NULL;
    uint64_t gen = 0;
    int64_t id = 0;

    CHECK(session_new(&s, NULL) == 0);
    session_open(s, &o);
    id = session_start(s, NULL);
    CHECK(wait_done(s, id, &st) && st.result == 0);
    CHECK(wait_records(s, 20000));
    old = session_acquire(s);
    // The dummy's default: the bench's four channels.
    CHECK(old && old->store && old->info && old->info->n == 4);
    gen = old ? old->gen : 0;

    id = session_start(s, NULL);
    CHECK(wait_done(s, id, &st) && st.result == 0 && st.state == SESSION_RUNNING);
    v = session_acquire(s);
    CHECK(v && v != old && v->gen > gen && st.gen == v->gen);
    session_release(s, v);
    if (old) {
        store_stats(old->store, &ss);
        CHECK(ss.total >= 20000 && ss.have_watermark);
    }
    session_release(s, old);
    CHECK(wait_records(s, 1000));
    for (int i = 0; i < WAIT_MS / 5 && st.capture.total.records == 0; i++) {
        sleep_ms(5);
        session_status(s, &st);
    }
    CHECK(st.capture.running && st.capture.total.records > 0 && st.capture.total.crc_errors == 0);
    session_free(s);
}


/*
 * Groups added before a capture and after it: one added later decodes the history in the
 * store to the same items. Then the capture's recording, read back from the file, decodes
 * to them too.
 */
static void test_groups(const char *dir)
{
    struct session_open o = {.backend = SESSION_DUMMY};
    struct session_status st;
    struct dump a, b, c, p;
    struct session *s = NULL;
    char path[600];
    int64_t id = 0;

    snprintf(path, sizeof(path), "%s/groups.thcapture", dir);
    CHECK(session_new(&s, NULL) == 0);
    session_open(s, &o);
    session_decoder_add(s, "i2c", "scl=0,sda=1,name=live", NULL, 0);
    session_decoder_add(s, "sht3x", "addr=0x44", "live", 0);
    session_decoder_add(s, "activity", "name=packets", NULL, 0.05);
    session_record(s, path);
    id = session_start(s, NULL);
    CHECK(wait_done(s, id, &st) && st.result == 0);
    CHECK(wait_records(s, 60000));
    id = session_stop(s);
    CHECK(wait_done(s, id, &st) && st.result == 0);
    CHECK(wait_decoded(s));

    id = session_decoder_add(s, "i2c", "scl=0,sda=1,name=later", NULL, 0);
    CHECK(wait_done(s, id, &st) && st.result == 0);
    id = session_decoder_add(s, "sht3x", "addr=0x44", "later", 0);
    CHECK(wait_done(s, id, &st) && st.result == 0);
    CHECK(wait_decoded(s));

    dump_group(s, "live", &a);
    dump_group(s, "later", &b);
    dump_group(s, "packets", &p);
    CHECK(a.count > 5 && a.count == b.count);
    CHECK(a.len == b.len && a.text && b.text && !memcmp(a.text, b.text, a.len));
    CHECK(p.count > 5);
    {
        struct session_decoder d[SESSION_DECODERS_MAX];
        size_t n = session_decoders(s, d, SESSION_DECODERS_MAX);

        CHECK(n == 3);
        for (size_t i = 0; i < n; i++) {
            CHECK(d[i].error == 0);
            if (!strcmp(d[i].label, "live")) {
                CHECK(d[i].stacked == 1 && d[i].hold == 1.0);
            }
        }
    }

    // The recording, read back: the same items.
    o = (struct session_open){.backend = SESSION_FILE, .path = path};
    id = session_open(s, &o);
    CHECK(wait_done(s, id, &st) && st.result == 0);
    id = session_start(s, NULL);
    CHECK(wait_done(s, id, &st) && st.result == 0);
    for (int i = 0; i < WAIT_MS / 5 && st.state != SESSION_ENDED; i++) {
        sleep_ms(5);
        session_status(s, &st);
    }
    CHECK(st.state == SESSION_ENDED);
    CHECK(wait_decoded(s));
    dump_group(s, "live", &c);
    CHECK(c.count == a.count && c.len == a.len && c.text && !memcmp(c.text, a.text, a.len));

    free(a.text);
    free(b.text);
    free(c.text);
    free(p.text);
    session_free(s);
}
