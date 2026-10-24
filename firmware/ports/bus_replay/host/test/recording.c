/*
 * Capture files from the dummy.
 *
 * What the session parsed live is what reading the file back gives, record for record, and
 * what the END chunk says; the index written live is the one a rebuild makes. A file cut
 * short anywhere, or with a damaged chunk, reads up to the damage; a capture stopped with
 * SIGINT ends complete, one killed with SIGKILL stays readable.
 *
 * Files go to a new temporary directory; the SIGINT and SIGKILL runs use the binary
 * (test_bin()).
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "capture/capture.h"
#include "capture/dummy.h"
#include "capture/index.h"
#include "capture/thcfile.h"
#include "common/capture_info.h"
#include "common/record.h"
#include "common/status.h"
#include "test/test.h"


#define SECONDS 6

// What a session handed over, and a hash of every record in order.
struct tally {
    uint64_t records;
    uint64_t heartbeats;
    uint64_t losses;
    uint64_t lost;
    uint64_t hash;
};

// Everything a parse ends with, for comparing two of them.
struct result {
    struct tally tally;
    struct capture_status st;
    uint64_t channel_records[8];
    uint64_t channel_lost[8];
    unsigned int n;
};


static void test_roundtrip(const char *dir, uint8_t enabled, const char *name);
static void test_index(const char *path);
static void test_cut(const char *dir, const char *path);
static void test_signal(const char *dir, int sig);
static int read_file(const char *path, struct result *res, enum thc_state *state);
static void finish(struct capture *cap, struct tally *t, struct result *res);
static void on_record(void *ctx, const struct record *r);
static void on_heartbeat(void *ctx, const struct heartbeat *hb);
static void on_loss(void *ctx, const struct loss *loss);
static bool same_result(const struct result *a, const struct result *b);
static void remove_capture(const char *path);
static int copy_bytes(const char *path, const uint8_t *data, size_t len);
static uint8_t *slurp(const char *path, size_t *len);
static void sleep_ms(unsigned int ms);


int main(void)
{
    const char *dir = test_dir();
    char path[600];

    // The bench's four channels, then all six (a clock and a PWM besides).
    test_roundtrip(dir, 0x0F, "four.thcapture");
    test_roundtrip(dir, 0x3F, "six.thcapture");
    snprintf(path, sizeof(path), "%s/four.thcapture", dir);
    test_index(path);
    test_cut(dir, path);
    test_signal(dir, SIGINT);
    test_signal(dir, SIGKILL);

    return test_result(__FILE__);
}


// SECONDS of the dummy, live and from the file it was recorded to.
static void test_roundtrip(const char *dir, uint8_t enabled, const char *name)
{
    struct dummy_options opt = {.realtime = false};
    struct capture_config cfg = {.enabled = &enabled};
    struct tally live_tally = {0};
    struct capture_handler h = {.record = on_record, .heartbeat = on_heartbeat,
                                .loss = on_loss, .ctx = &live_tally};
    struct capture *cap = NULL;
    struct thc_reader *reader = NULL;
    const struct thc_trailer *t = NULL;
    struct result live = {0};
    struct result file = {0};
    struct capture_status st;
    enum thc_state state = THC_READING;
    char path[600];
    char error[256];

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    remove_capture(path);

    CHECK(capture_open_dummy(&cap, &opt, &h) == 0);
    CHECK(capture_record(cap, path, NULL) == 0);
    CHECK(capture_start(cap, &cfg, error, sizeof(error)) == 0);
    do {
        CHECK(capture_pull(cap, 0) >= 0);
        capture_status(cap, &st);
    } while (st.now < (uint64_t)SECONDS * capture_info(cap)->rate);
    finish(cap, &live_tally, &live);
    CHECK(capture_stop(cap, THC_REASON_DURATION) == 0);
    capture_close(cap);

    // The dummy's losses drop records, and the mismatches they cause are excused.
    CHECK(live.tally.records > 10000);
    CHECK(live.tally.losses >= 2);
    CHECK(live.tally.lost > 0);
    CHECK(live.st.total.excused > 0);
    CHECK(live.st.total.mismatches == 0);
    CHECK(live.st.total.crc_errors == 0);
    CHECK(live.st.total.gap_errors == 0);

    CHECK(read_file(path, &file, &state) == 0);
    CHECK(state == THC_ENDED);
    CHECK(same_result(&live, &file));

    CHECK(thc_reader_open(path, &reader) == 0);
    if (!reader) {
        return;
    }
    CHECK(thc_reader_complete(reader));
    t = thc_reader_trailer(reader);
    if (t) {
        CHECK(t->reason == THC_REASON_DURATION);
        CHECK(t->n == live.n);
        CHECK(t->data_channels == 1);
        CHECK(t->data[0].records == live.tally.records);
        CHECK(t->data[0].heartbeats == live.tally.heartbeats);
        CHECK(t->data[0].bytes == live.st.total.bytes);
        CHECK(t->loss_reports == live.tally.losses);
        CHECK(t->excused == live.st.total.excused);
        CHECK(t->mismatches == 0);
        CHECK(t->overflow == 0);
        for (unsigned int i = 0; i < t->n && i < 8; i++) {
            CHECK(t->records[i] == live.channel_records[i]);
            CHECK(t->lost[i] == live.channel_lost[i]);
        }
    }
    thc_reader_close(reader);
}


// The index written live: it matches its file, points at heartbeats, and a rebuild agrees.
static void test_index(const char *path)
{
    struct index_entry *live = NULL;
    struct index_entry *rebuilt = NULL;
    size_t live_count = 0;
    size_t rebuilt_count = 0;
    uint32_t k = 0;
    struct thc_reader *reader = NULL;
    const struct thc_header *h = NULL;
    struct thc_chunk c;
    char *ipath = index_path(path);
    size_t e = 0;

    CHECK(thc_reader_open(path, &reader) == 0);
    if (!reader || !ipath) {
        free(ipath);
        return;
    }
    h = thc_reader_header(reader);
    CHECK(index_check(ipath, h->crc, h->start_time) == 0);
    CHECK(index_check(ipath, h->crc ^ 1, h->start_time) == -ESTALE);
    CHECK(index_read(ipath, &k, &live, &live_count) == 0);
    CHECK(k == 500);
    // About one entry a second, from grid 0.
    CHECK(live_count >= SECONDS && live_count <= SECONDS + 2);
    CHECK(live_count > 0 && live[0].grid == 0);

    // Each entry's byte is the first of a heartbeat's all-zero marker.
    while (e < live_count && thc_reader_next(reader, &c) > 0) {
        while (e < live_count && live[e].chunk == c.offset) {
            CHECK(c.type == THC_DATA);
            CHECK(live[e].payload < c.len && c.payload[live[e].payload] == 0);
            CHECK(live[e].grid % k == 0);
            e++;
        }
    }
    CHECK(e == live_count);
    thc_reader_close(reader);

    unlink(ipath);
    CHECK(capture_index_rebuild(path) == 0);
    CHECK(index_read(ipath, &k, &rebuilt, &rebuilt_count) == 0);
    CHECK(rebuilt_count == live_count);
    CHECK(rebuilt && live && memcmp(rebuilt, live, live_count * sizeof(*live)) == 0);

    free(live);
    free(rebuilt);
    free(ipath);
}


/*
 * The recording cut short at points from the end of the header to the
 * footer, and with one byte changed, reads without error up to where it is damaged.
 */
static void test_cut(const char *dir, const char *path)
{
    struct result full = {0};
    struct result part = {0};
    enum thc_state state = THC_READING;
    uint64_t last = 0;
    size_t len = 0;
    size_t header = 0;
    uint8_t *data = slurp(path, &len);
    char cut[600];

    snprintf(cut, sizeof(cut), "%s/cut.thcapture", dir);
    CHECK(data != NULL && len > THC_FIXED_LEN);
    if (!data || len <= THC_FIXED_LEN) {
        free(data);
        return;
    }
    header = (size_t)thc_get_le(&data[12], 4);
    CHECK(read_file(path, &full, &state) == 0);

    for (unsigned int i = 0; i <= 20; i++) {
        size_t at = header + (len - header) * i / 20;

        if (i == 20) {
            at = len - 1;
        }
        remove_capture(cut);
        CHECK(copy_bytes(cut, data, at) == 0);
        part = (struct result){0};
        CHECK(read_file(cut, &part, &state) == 0);
        CHECK(state != THC_ENDED || at >= len - THC_FOOTER_LEN);
        CHECK(part.tally.records <= full.tally.records);
        CHECK(part.tally.records >= last);
        last = part.tally.records;
    }
    // Cut inside the footer: every chunk, END included, is there.
    CHECK(last == full.tally.records);

    // A damaged byte in the middle: reading stops at that chunk.
    data[len / 2] ^= 0x55;
    remove_capture(cut);
    CHECK(copy_bytes(cut, data, len) == 0);
    part = (struct result){0};
    CHECK(read_file(cut, &part, &state) == 0);
    CHECK(state == THC_BAD_CRC);
    CHECK(part.tally.records > 0 && part.tally.records < full.tally.records);

    remove_capture(cut);
    free(data);
}


/*
 * A capture stopped by SIGINT, or killed by SIGKILL. The stop, drain included, must come
 * before the dummy's second loss (capture/dummy.c, LOSS_AT_S and LOSS_PERIOD_S): a loss
 * still open at the stop is never reported, and its mismatch would end the capture with
 * errors.
 */
static void test_signal(const char *dir, int sig)
{
    struct result res = {0};
    enum thc_state state = THC_READING;
    struct thc_reader *reader = NULL;
    char path[600];
    pid_t pid = 0;
    int status = 0;

    snprintf(path, sizeof(path), "%s/signal.thcapture", dir);
    remove_capture(path);

    pid = fork();
    if (pid == 0) {
        int null = open("/dev/null", O_WRONLY);

        dup2(null, STDERR_FILENO);
        execl(test_bin(), "bus-replay", "capture", "--dummy", "--out", path,
              "--duration", "0", (char *)NULL);
        _exit(127);
    }
    CHECK(pid > 0);
    if (pid <= 0) {
        return;
    }
    sleep_ms(2600);
    kill(pid, sig);
    CHECK(waitpid(pid, &status, 0) == pid);

    CHECK(read_file(path, &res, &state) == 0);
    CHECK(res.tally.records > 1000);
    CHECK(thc_reader_open(path, &reader) == 0);
    if (sig == SIGINT) {
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        CHECK(state == THC_ENDED);
        CHECK(reader && thc_reader_complete(reader));
        if (reader && thc_reader_trailer(reader)) {
            CHECK(thc_reader_trailer(reader)->reason == THC_REASON_USER);
            CHECK(thc_reader_trailer(reader)->data[0].records == res.tally.records);
        }
    } else {
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
        CHECK(state == THC_EOF || state == THC_CUT);
        CHECK(reader && !thc_reader_complete(reader));
    }
    thc_reader_close(reader);
    remove_capture(path);
}


static int read_file(const char *path, struct result *res, enum thc_state *state)
{
    struct tally t = {0};
    struct capture_handler h = {.record = on_record, .heartbeat = on_heartbeat,
                                .loss = on_loss, .ctx = &t};
    struct capture *cap = NULL;
    int err = capture_open_file(&cap, path, &h);

    if (err < 0) {
        return err;
    }
    while ((err = capture_pull(cap, 0)) > 0) {
    }
    *state = thc_reader_state(capture_reader(cap));
    finish(cap, &t, res);
    capture_close(cap);

    return err == -ENODATA ? 0 : err;
}


static void finish(struct capture *cap, struct tally *t, struct result *res)
{
    const struct capture_info *info = capture_info(cap);

    res->tally = *t;
    capture_status(cap, &res->st);
    res->n = info->n;
    for (unsigned int i = 0; i < info->n && i < 8; i++) {
        res->channel_records[i] = capture_channel_records(cap)[i];
        res->channel_lost[i] = capture_channel_lost(cap)[i];
    }
}


static void on_record(void *ctx, const struct record *r)
{
    struct tally *t = ctx;

    t->records++;
    // FNV-1a over the record's fields.
    t->hash = (t->hash ^ r->tick) * 0x100000001B3ULL;
    t->hash = (t->hash ^ r->channel) * 0x100000001B3ULL;
    t->hash = (t->hash ^ r->level) * 0x100000001B3ULL;
}


static void on_heartbeat(void *ctx, const struct heartbeat *hb)
{
    struct tally *t = ctx;

    (void)hb;
    t->heartbeats++;
}


static void on_loss(void *ctx, const struct loss *loss)
{
    struct tally *t = ctx;

    t->losses++;
    for (unsigned int i = 0; i < loss->n; i++) {
        t->lost += loss->lost[i];
    }
}


static bool same_result(const struct result *a, const struct result *b)
{
    bool same = a->n == b->n && memcmp(&a->tally, &b->tally, sizeof(a->tally)) == 0 &&
                a->st.now == b->st.now && a->st.losses == b->st.losses &&
                memcmp(&a->st.total, &b->st.total, sizeof(a->st.total)) == 0;

    for (unsigned int i = 0; same && i < a->n && i < 8; i++) {
        same = a->channel_records[i] == b->channel_records[i] &&
               a->channel_lost[i] == b->channel_lost[i];
    }

    return same;
}


static void remove_capture(const char *path)
{
    char *ipath = index_path(path);

    unlink(path);
    if (ipath) {
        unlink(ipath);
    }
    free(ipath);
}


static int copy_bytes(const char *path, const uint8_t *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    size_t n = 0;

    if (!f) {
        return -errno;
    }
    n = fwrite(data, 1, len, f);
    fclose(f);

    return n == len ? 0 : -EIO;
}


static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *data = NULL;
    long size = 0;

    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0) {
        data = malloc((size_t)size);
        if (data && fread(data, 1, (size_t)size, f) != (size_t)size) {
            free(data);
            data = NULL;
        }
    }
    fclose(f);
    *len = data ? (size_t)size : 0;

    return data;
}


static void sleep_ms(unsigned int ms)
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000};

    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {
    }
}
