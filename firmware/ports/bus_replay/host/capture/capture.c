#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <bits/time.h>
#include <bits/types/clockid_t.h>

#include "capture/capture.h"
#include "capture/control.h"
#include "capture/dummy.h"
#include "capture/index.h"
#include "capture/info.h"
#include "capture/local.h"
#include "capture/loss.h"
#include "capture/recorder.h"
#include "capture/remote.h"
#include "capture/source.h"
#include "capture/thcfile.h"
#include "capture/wire.h"
#include "common/capture_info.h"
#include "common/record.h"
#include "common/status.h"


// At most this many items per capture_pull(), so the caller gets a turn.
#define PULL_MAX 64

// A configuration message or reply, for the capture file's header.
struct config_msg {
    uint16_t type;
    uint8_t *data;
    size_t len;
};

// Where a file session starts parsing a data channel (capture_seek()).
struct seek {
    bool on;
    uint64_t chunk;
    uint32_t payload;
    uint64_t grid;
};

/*
 * A live session has ops and src; a file session has reader, path and seek (one per data
 * channel). t0 is the monotonic time of the accepted start, which item times are counted
 * from.
 */
struct capture {
    const struct source_ops *ops;
    void *src;
    struct thc_reader *reader;
    char *path;
    struct seek *seek;
    struct capture_handler handler;

    uint8_t *hello_msg;
    size_t hello_len;
    struct hello hello;
    bool have_hello;
    struct config_msg *configs;
    size_t config_count;

    struct capture_info info;
    bool started;
    struct wire *wires;
    struct loss_tracker lt;
    uint64_t *records;
    uint64_t *lost;
    struct capture_status st;
    uint64_t t0;
    uint64_t start_time;
    uint64_t time;
    uint64_t overflow;

    struct recorder *rec;
    char *binding;
    char *host;
    struct index_builder *ib;
};


static int open_backend(struct capture **out, const struct source_ops *ops, void *src,
                        const struct capture_handler *handler, bool hello);
static int begin_recording(struct capture *cap, char *error, size_t size);
static int set_hello(struct capture *cap, const uint8_t *msg, size_t len);
static int config_add(struct capture *cap, uint16_t type, const uint8_t *data, size_t len);
static int configure(struct capture *cap, uint8_t reg, const uint8_t *value, size_t len,
                     char *error, size_t size);
static int session_init(struct capture *cap);
static void session_free(struct capture *cap);
static int handle(struct capture *cap, const struct source_item *item, uint64_t offset);
static void loss_report(struct capture *cap, const uint8_t *msg, size_t len);
static void on_record(void *ctx, const struct record *r);
static void on_heartbeat(void *ctx, const struct heartbeat *hb);
static int logical_index(const struct capture_info *info, uint16_t channel);
static uint64_t clock_ns(clockid_t id);


/*
 * Opens a session on a device backend (ops, src), which it takes over: it is closed with
 * the session, also on failure. Reads the device's hello. Returns errors from the backend,
 * -EPROTO for a hello that does not parse or a spec major this host does not know.
 */
int capture_open(struct capture **out, const struct source_ops *ops, void *src,
                 const struct capture_handler *handler)
{
    return open_backend(out, ops, src, handler, true);
}


int capture_open_dummy(struct capture **out, const struct dummy_options *opt,
                       const struct capture_handler *handler)
{
    void *src = NULL;
    int err = dummy_open(&src, opt);

    if (err < 0) {
        *out = NULL;
        return err;
    }

    return capture_open(out, &dummy_ops, src, handler);
}


// Opens a session on the device on this Pi (capture/local.h).
int capture_open_local(struct capture **out, const struct capture_handler *handler)
{
    void *src = NULL;
    int err = local_open(&src);

    if (err < 0) {
        *out = NULL;
        return err;
    }

    return capture_open(out, &local_ops, src, handler);
}


/*
 * Opens a session on a link (capture/remote.h). A link that runs a capture already is not
 * asked for a hello, which would end it: join it with remote_attach() and capture_attach().
 */
int capture_open_remote(struct capture **out, const struct remote_options *opt,
                        const struct capture_handler *handler)
{
    void *src = NULL;
    int err = remote_open(&src, opt);

    if (err < 0) {
        *out = NULL;
        return err;
    }

    return open_backend(out, &remote_ops, src, handler, !remote_running(src));
}


/*
 * Opens a capture file to be read back with capture_pull(). Returns the errors of
 * thc_reader_open(), and -EPROTO for a header whose hello or capture info does not parse.
 */
int capture_open_file(struct capture **out, const char *path,
                      const struct capture_handler *handler)
{
    struct capture *cap = calloc(1, sizeof(*cap));
    const struct thc_header *h = NULL;
    struct thc_info ti;
    int err = 0;

    *out = NULL;
    if (!cap) {
        return -ENOMEM;
    }
    if (handler) {
        cap->handler = *handler;
    }

    err = thc_reader_open(path, &cap->reader);
    if (err == 0) {
        h = thc_reader_header(cap->reader);
        err = set_hello(cap, h->hello, h->hello_len);
    }
    if (err == 0) {
        err = thc_info_parse(h->info, h->info_len, &ti);
    }
    if (err == 0 && (ti.channels != cap->hello.channels ||
                     ti.data_channels != cap->hello.data_channels)) {
        err = -EPROTO;
    }
    if (err == 0) {
        err = capture_info_init(&cap->info, &cap->hello, ti.rate, ti.c, ti.enabled, ti.split);
    }
    if (err == 0) {
        err = session_init(cap);
    }
    if (err == 0) {
        cap->path = strdup(path);
        cap->seek = calloc(cap->info.data_channels ? cap->info.data_channels : 1,
                           sizeof(*cap->seek));
        err = cap->path && cap->seek ? 0 : -ENOMEM;
    }
    if (err < 0) {
        capture_close(cap);
        return err;
    }

    cap->start_time = h->start_time;
    if (h->first_grid) {
        for (unsigned int d = 0; d < cap->info.data_channels; d++) {
            wire_start_at(&cap->wires[d], h->first_grid, true);
        }
    }
    if (h->loss_base) {
        struct loss_report rep;

        if (loss_report_parse(h->loss_base, h->loss_base_len, cap->info.n, &rep) == 0) {
            loss_tracker_base(&cap->lt, &rep, wire_tick(&cap->wires[0], rep.end_grid,
                                                        rep.end_off));
        }
    }
    *out = cap;

    return 0;
}


// Closes the session; a live capture still running is stopped first.
void capture_close(struct capture *cap)
{
    if (!cap) {
        return;
    }
    if (cap->ops && cap->st.running) {
        capture_stop(cap, THC_REASON_USER);
    }
    if (cap->rec) {
        struct thc_trailer t = {0};

        recorder_close(cap->rec, 0, &t);
    }
    if (cap->ops) {
        cap->ops->close(cap->src);
    }
    thc_reader_close(cap->reader);
    free(cap->path);
    free(cap->seek);
    session_free(cap);
    capture_info_free(&cap->info);
    if (cap->have_hello) {
        hello_free(&cap->hello);
    }
    for (size_t i = 0; i < cap->config_count; i++) {
        free(cap->configs[i].data);
    }
    free(cap->configs);
    free(cap->hello_msg);
    free(cap->binding);
    free(cap->host);
    free(cap);
}


// The device's hello: as last read from a device, or as recorded in the file.
int capture_hello(struct capture *cap, const struct hello **h)
{
    *h = cap->have_hello ? &cap->hello : NULL;

    return cap->have_hello ? 0 : -ENODATA;
}


// The hello as the device sent it, for a copy that outlives the session.
int capture_hello_msg(const struct capture *cap, const uint8_t **msg, size_t *len)
{
    *msg = cap->hello_msg;
    *len = cap->hello_len;

    return cap->have_hello ? 0 : -ENODATA;
}


/*
 * Records the capture to a new capture file at path. Call before capture_start(); the
 * file is created now, so a path that cannot be used fails here (-EEXIST if it exists).
 */
int capture_record(struct capture *cap, const char *path,
                   const struct capture_record_options *opt)
{
    int err = 0;

    if (!cap->ops || cap->started || cap->rec) {
        return -EINVAL;
    }
    if (opt && opt->binding) {
        cap->binding = strdup(opt->binding);
    }
    if (opt && opt->host) {
        cap->host = strdup(opt->host);
    }
    err = recorder_open(&cap->rec, path);

    return err;
}


/*
 * Resets the device, configures it and starts a capture (spec, Configuration, Capture
 * lifecycle). A rejected write returns -EINVAL with the device's reason in error.
 */
int capture_start(struct capture *cap, const struct capture_config *cfg, char *error,
                  size_t size)
{
    const struct reg *rate_reg = NULL;
    const struct reg *c_reg = NULL;
    const struct reg *enabled_reg = NULL;
    const struct reg *split_reg = NULL;
    const uint8_t *msg = NULL;
    size_t len = 0;
    uint8_t value[4];
    uint8_t *split_zero = NULL;
    const uint8_t *enabled = NULL;
    const uint8_t *split = NULL;
    size_t set_size = 0;
    uint32_t rate = 0;
    uint32_t c = 0;
    int err = 0;

    snprintf(error, size, "%s", "");
    if (!cap->ops || cap->started) {
        snprintf(error, size, "no device, or a capture already started");
        return -EINVAL;
    }

    err = cap->ops->hello(cap->src, &msg, &len);
    err = err ? err : set_hello(cap, msg, len);
    if (err < 0) {
        snprintf(error, size, "no hello from the device (err: %d)", err);
        return err;
    }
    rate_reg = hello_reg(&cap->hello, REG_SAMPLE_RATE);
    c_reg = hello_reg(&cap->hello, REG_C);
    enabled_reg = hello_reg(&cap->hello, REG_ENABLED);
    split_reg = hello_reg(&cap->hello, REG_SPLIT);
    set_size = ((size_t)cap->hello.channels + 7) / 8;
    if (!rate_reg || !c_reg || !enabled_reg || enabled_reg->size != set_size ||
        (split_reg && split_reg->size != set_size)) {
        snprintf(error, size, "the hello lacks a core register");
        return -EPROTO;
    }

    rate = cfg && cfg->rate ? cfg->rate : reg_u32(rate_reg);
    c = cfg && cfg->c ? cfg->c : reg_u32(c_reg);
    enabled = cfg && cfg->enabled ? cfg->enabled : enabled_reg->value;
    split_zero = calloc(set_size ? set_size : 1, 1);
    if (!split_zero) {
        return -ENOMEM;
    }
    split = cfg && cfg->split ? cfg->split : split_reg ? split_reg->value : split_zero;

    for (size_t i = 0; i < 4; i++) {
        value[i] = (uint8_t)(rate >> (8 * i));
    }
    err = configure(cap, REG_SAMPLE_RATE, value, 4, error, size);
    for (size_t i = 0; i < 4; i++) {
        value[i] = (uint8_t)(c >> (8 * i));
    }
    err = err ? err : configure(cap, REG_C, value, 4, error, size);
    err = err ? err : configure(cap, REG_ENABLED, enabled, set_size, error, size);
    if (err == 0 && split_reg) {
        err = configure(cap, REG_SPLIT, split, set_size, error, size);
    }
    value[0] = 1;
    err = err ? err : configure(cap, REG_CAPTURE, value, 1, error, size);
    if (err < 0) {
        free(split_zero);
        return err;
    }
    cap->t0 = clock_ns(CLOCK_MONOTONIC);
    cap->start_time = clock_ns(CLOCK_REALTIME);

    err = capture_info_init(&cap->info, &cap->hello, rate, c, enabled, split);
    free(split_zero);
    err = err ? err : session_init(cap);
    if (err == 0 && cap->rec) {
        err = begin_recording(cap, error, size);
    }
    if (err < 0) {
        cap->ops->stop(cap->src);
        return err;
    }
    cap->started = true;
    cap->st.running = true;

    return 0;
}


/*
 * Joins a capture that already runs (struct capture_attach), on a live session that has
 * not started one: the session then parses the stream as one started with those messages.
 * A recording set with capture_record() starts with it if the stream begins at the
 * capture's start; one that joins later can't be recorded (-EBUSY). -EPROTO for a hello or
 * a configuration that does not make a capture.
 */
int capture_attach(struct capture *cap, const struct capture_attach *a, char *error,
                   size_t size)
{
    const struct reg *rate_reg = NULL;
    const struct reg *c_reg = NULL;
    const struct reg *enabled_reg = NULL;
    const struct reg *split_reg = NULL;
    const uint8_t *enabled = NULL;
    const uint8_t *split = NULL;
    uint8_t *split_zero = NULL;
    size_t set_size = 0;
    uint32_t rate = 0;
    uint32_t c = 0;
    int err = 0;

    snprintf(error, size, "%s", "");
    if (!cap->ops || cap->started) {
        snprintf(error, size, "no device, or a capture already started");
        return -EINVAL;
    }
    if (cap->rec && a->first_grid) {
        snprintf(error, size, "a capture joined later can't be recorded");
        return -EBUSY;
    }
    err = set_hello(cap, a->hello, a->hello_len);
    if (err < 0) {
        snprintf(error, size, "the hello does not parse (err: %d)", err);
        return err;
    }
    rate_reg = hello_reg(&cap->hello, REG_SAMPLE_RATE);
    c_reg = hello_reg(&cap->hello, REG_C);
    enabled_reg = hello_reg(&cap->hello, REG_ENABLED);
    split_reg = hello_reg(&cap->hello, REG_SPLIT);
    set_size = ((size_t)cap->hello.channels + 7) / 8;
    if (!rate_reg || !c_reg || !enabled_reg || enabled_reg->size != set_size ||
        (split_reg && split_reg->size != set_size)) {
        snprintf(error, size, "the hello lacks a core register");
        return -EPROTO;
    }
    rate = reg_u32(rate_reg);
    c = reg_u32(c_reg);
    enabled = enabled_reg->value;
    split_zero = calloc(set_size ? set_size : 1, 1);
    if (!split_zero) {
        return -ENOMEM;
    }
    split = split_reg ? split_reg->value : split_zero;

    // The writes the device accepted, each followed by its reply, set the capture.
    for (size_t i = 0; i < cap->config_count; i++) {
        free(cap->configs[i].data);
    }
    cap->config_count = 0;
    for (size_t i = 0; i < a->config_count && err == 0; i++) {
        const struct recorder_msg *m = &a->configs[i];
        const struct recorder_msg *r = i + 1 < a->config_count ? &a->configs[i + 1] : NULL;
        struct config_reply rep;

        err = config_add(cap, m->type, m->data, m->len);
        if (err < 0 || m->type != THC_ITEM_CONFIG || m->len < 2 || m->data[0] != MSG_CONFIG ||
            !r || r->type != THC_ITEM_CONFIG_REPLY ||
            config_reply_parse(r->data, r->len, &rep) < 0 || rep.status != 0) {
            continue;
        }
        if (m->data[1] == REG_SAMPLE_RATE && m->len == 6) {
            rate = (uint32_t)thc_get_le(&m->data[2], 4);
        } else if (m->data[1] == REG_C && m->len == 6) {
            c = (uint32_t)thc_get_le(&m->data[2], 4);
        } else if (m->data[1] == REG_ENABLED && m->len == 2 + set_size) {
            enabled = &m->data[2];
        } else if (m->data[1] == REG_SPLIT && m->len == 2 + set_size) {
            split = &m->data[2];
        }
    }

    capture_info_free(&cap->info);
    err = err ? err : capture_info_init(&cap->info, &cap->hello, rate, c, enabled, split);
    free(split_zero);
    err = err ? err : session_init(cap);
    if (err < 0) {
        snprintf(error, size, "the configuration does not make a capture (err: %d)", err);
        return err;
    }
    if (a->first_grid) {
        for (unsigned int d = 0; d < cap->info.data_channels; d++) {
            wire_start_at(&cap->wires[d], a->first_grid, true);
        }
    }
    if (a->loss_base) {
        struct loss_report rep;

        if (loss_report_parse(a->loss_base, a->loss_base_len, cap->info.n, &rep) == 0) {
            loss_tracker_base(&cap->lt, &rep, wire_tick(&cap->wires[0], rep.end_grid,
                                                        rep.end_off));
        }
    }
    cap->t0 = clock_ns(CLOCK_MONOTONIC) - a->time;
    cap->start_time = clock_ns(CLOCK_REALTIME) - a->time;
    if (cap->rec) {
        err = begin_recording(cap, error, size);
        if (err < 0) {
            return err;
        }
    }
    cap->started = true;
    cap->st.running = true;

    return 0;
}


/*
 * Takes what has arrived, waiting up to timeout_ms for the first item, and hands it to the
 * handler. Returns the number of items taken (0 if none came), -ENODATA at the end of a
 * capture file, or an error from the backend.
 */
int capture_pull(struct capture *cap, int timeout_ms)
{
    int count = 0;

    if (!cap->started && !cap->reader) {
        return -EINVAL;
    }

    while (count < PULL_MAX) {
        struct source_item item;
        uint64_t offset = 0;
        int n = 0;

        if (cap->reader) {
            struct thc_chunk c;

            n = thc_reader_next(cap->reader, &c);
            if (n == 0) {
                return count ? count : -ENODATA;
            }
            item = (struct source_item){.type = c.type, .flags = c.flags, .channel = c.channel,
                                        .time = c.time, .data = c.payload, .len = c.len};
            offset = c.offset;
        } else {
            n = cap->ops->read(cap->src, &item, count ? 0 : timeout_ms);
            if (n == 0) {
                break;
            }
            item.time = item.time > cap->t0 ? item.time - cap->t0 : 0;
        }
        if (n < 0) {
            return n;
        }
        n = handle(cap, &item, offset);
        if (n < 0) {
            return n;
        }
        count++;
    }

    return count;
}


/*
 * For a capture file not read yet: each data channel is parsed from its indexed heartbeat
 * at or before tick (docs/capture-file.md, Index file), so a span late in a long file is
 * reached without parsing what comes before it. Every chunk is still read, and loss reports
 * taken, only data before those heartbeats is not parsed. Returns -ENOENT if the file has no
 * index that belongs to it; the session then parses from the start.
 */
int capture_seek(struct capture *cap, uint64_t tick)
{
    struct index_entry *e = NULL;
    size_t count = 0;
    uint32_t k = 0;
    char *ipath = NULL;
    const struct thc_header *h = NULL;
    int err = 0;

    if (!cap->reader || thc_reader_offset(cap->reader) != thc_reader_header(cap->reader)->length) {
        return -EINVAL;
    }
    h = thc_reader_header(cap->reader);
    ipath = index_path(cap->path);
    err = ipath ? index_check(ipath, h->crc, h->start_time) : -ENOMEM;
    err = err ? err : index_read(ipath, &k, &e, &count);
    free(ipath);
    if (err < 0) {
        return err == -ENOMEM ? err : -ENOENT;
    }

    for (size_t i = 0; i < count; i++) {
        struct seek *sk = e[i].channel < cap->info.data_channels ? &cap->seek[e[i].channel] : NULL;

        if (sk && e[i].grid * cap->info.c <= tick && (!sk->on || e[i].grid > sk->grid)) {
            *sk = (struct seek){.on = true, .chunk = e[i].chunk, .payload = e[i].payload,
                                .grid = e[i].grid};
        }
    }
    free(e);

    return 0;
}


/*
 * Ends a live capture with a reset, and the recording with a Stop event and its END chunk
 * (reason, docs/capture-file.md). Returns the recording's first error, 0 if none.
 */
int capture_stop(struct capture *cap, uint8_t reason)
{
    uint64_t time = 0;
    int err = 0;

    if (!cap->ops || !cap->st.running) {
        return 0;
    }
    time = clock_ns(CLOCK_MONOTONIC) - cap->t0;
    if (cap->rec) {
        recorder_event(cap->rec, THC_EV_STOP, 0, time, NULL, 0);
    }
    cap->ops->stop(cap->src);
    cap->st.running = false;

    if (cap->rec) {
        struct thc_trailer t;

        err = capture_trailer(cap, reason, &t);
        if (err == 0) {
            err = recorder_close(cap->rec, time, &t);
            thc_trailer_free(&t);
        } else {
            struct thc_trailer none = {0};

            recorder_close(cap->rec, time, &none);
        }
        cap->rec = NULL;
    }

    return err;
}


/*
 * Writes the index file of the capture file at path from a full parse of it, replacing
 * any there is.
 */
int capture_index_rebuild(const char *path)
{
    struct capture *cap = NULL;
    struct index_builder ib;
    const struct thc_header *h = NULL;
    struct thc_buf out = {0};
    char *ipath = index_path(path);
    FILE *f = NULL;
    int err = ipath ? capture_open_file(&cap, path, NULL) : -ENOMEM;

    if (err < 0) {
        free(ipath);
        return err;
    }
    h = thc_reader_header(cap->reader);
    err = index_builder_init(&ib, cap->info.data_channels,
                             index_interval(cap->info.rate, cap->info.c));
    if (err == 0) {
        cap->ib = &ib;
        while ((err = capture_pull(cap, 0)) > 0) {
        }
        err = err == -ENODATA ? 0 : err;
    }
    if (err == 0) {
        err = index_header(&out, h->crc, h->start_time, ib.k);
        err = err ? err : thc_buf_put(&out, ib.out.data, ib.out.len);
    }
    if (err == 0) {
        f = fopen(ipath, "wb");
        err = f ? 0 : -errno;
    }
    if (err == 0) {
        err = fwrite(out.data, 1, out.len, f) == out.len ? 0 : -EIO;
        err = fclose(f) == 0 ? err : -errno;
    }

    if (cap->ib) {
        index_builder_free(&ib);
    }
    thc_buf_free(&out);
    capture_close(cap);
    free(ipath);

    return err;
}


const struct capture_info *capture_info(const struct capture *cap)
{
    return cap->started || cap->reader ? &cap->info : NULL;
}


// Whether it runs, the latest tick, the loss reports, and every data channel's counters.
void capture_status(const struct capture *cap, struct capture_status *st)
{
    *st = cap->st;
    memset(&st->total, 0, sizeof(st->total));
    for (unsigned int d = 0; cap->wires && d < cap->info.data_channels; d++) {
        const struct data_stats *s = &cap->wires[d].stats;

        st->total.bytes += s->bytes;
        st->total.records += s->records;
        st->total.heartbeats += s->heartbeats;
        st->total.crc_errors += s->crc_errors;
        st->total.gap_errors += s->gap_errors;
        st->total.resyncs += s->resyncs;
        st->total.mismatches += s->mismatches;
        st->total.excused += s->excused;
    }
}


const struct data_stats *capture_data_stats(const struct capture *cap, unsigned int d)
{
    return cap->wires && d < cap->info.data_channels ? &cap->wires[d].stats : NULL;
}


// Records per logical channel, n entries.
const uint64_t *capture_channel_records(const struct capture *cap)
{
    return cap->records;
}


// Lost records per logical channel by the loss reports, n entries.
const uint64_t *capture_channel_lost(const struct capture *cap)
{
    return cap->lost;
}


// The latest item's time, in nanoseconds from the capture's start.
uint64_t capture_time(const struct capture *cap)
{
    return cap->time;
}


/*
 * DATA bytes dropped by the host: live, by the backend and by the recorder; from a file,
 * by its overflow events.
 */
uint64_t capture_overflow(const struct capture *cap)
{
    struct recorder_stats rs;

    if (cap->rec) {
        recorder_stats(cap->rec, &rs);
        return cap->overflow + rs.overflow_bytes;
    }

    return cap->overflow;
}


/*
 * Mismatches no loss explains from each data channel's latest good heartbeat on. After a
 * stop they may belong to a loss still open at the reset, which is never reported.
 */
uint64_t capture_tail_mismatches(const struct capture *cap)
{
    uint64_t count = 0;

    for (unsigned int d = 0; cap->wires && d < cap->info.data_channels; d++) {
        count += wire_tail_mismatches(&cap->wires[d]);
    }

    return count;
}


struct recorder *capture_recorder(const struct capture *cap)
{
    return cap->rec;
}


// The backend's source, for calls of its own (capture/remote.h); NULL for a file.
void *capture_source(const struct capture *cap)
{
    return cap->src;
}


/*
 * Where in its data channel's stream the heartbeat being handled starts (bytes from the
 * stream's start), valid in the heartbeat handler.
 */
uint64_t capture_stream_pos(const struct capture *cap, uint16_t data_channel)
{
    return data_channel < cap->info.data_channels ? cap->wires[data_channel].hb_pos : 0;
}


struct thc_reader *capture_reader(const struct capture *cap)
{
    return cap->reader;
}


// The END chunk's values for the session as it stands. Free with thc_trailer_free().
int capture_trailer(const struct capture *cap, uint8_t reason, struct thc_trailer *t)
{
    struct capture_status st;
    int err = thc_trailer_alloc(t, cap->info.data_channels, cap->info.n);

    if (err < 0) {
        return err;
    }
    capture_status(cap, &st);
    t->last_tick = st.now;
    t->reason = reason;
    t->loss_reports = st.losses;
    t->mismatches = st.total.mismatches;
    t->excused = st.total.excused;
    t->overflow = capture_overflow(cap);
    for (unsigned int d = 0; d < cap->info.data_channels; d++) {
        t->data[d] = cap->wires[d].stats;
    }
    for (unsigned int i = 0; i < cap->info.n; i++) {
        t->records[i] = cap->records[i];
        t->lost[i] = cap->lost[i];
    }

    return 0;
}


/*
 * A session on a device backend (ops, src), which it takes over: it is closed with the
 * session, also on failure. With hello, it reads the device's hello.
 */
static int open_backend(struct capture **out, const struct source_ops *ops, void *src,
                        const struct capture_handler *handler, bool hello)
{
    struct capture *cap = calloc(1, sizeof(*cap));
    const uint8_t *msg = NULL;
    size_t len = 0;
    int err = 0;

    *out = NULL;
    if (!cap) {
        ops->close(src);
        return -ENOMEM;
    }
    cap->ops = ops;
    cap->src = src;
    if (handler) {
        cap->handler = *handler;
    }

    if (hello) {
        err = ops->hello(src, &msg, &len);
        err = err ? err : set_hello(cap, msg, len);
    }
    if (err < 0) {
        capture_close(cap);
        return err;
    }
    *out = cap;

    return 0;
}


// The recording's header, from the hello and the configuration, once the capture runs.
static int begin_recording(struct capture *cap, char *error, size_t size)
{
    struct recorder_msg *msgs = calloc(cap->config_count ? cap->config_count : 1,
                                       sizeof(*msgs));
    struct recorder_header h = {
        .hello = cap->hello_msg,
        .hello_len = cap->hello_len,
        .configs = msgs,
        .config_count = cap->config_count,
        .info = &cap->info,
        .binding = cap->binding,
        .host = cap->host,
        .start_time = cap->start_time,
    };
    int err = 0;

    for (size_t i = 0; msgs && i < cap->config_count; i++) {
        msgs[i] = (struct recorder_msg){.type = cap->configs[i].type,
                                        .data = cap->configs[i].data,
                                        .len = cap->configs[i].len};
    }
    err = msgs ? recorder_begin(cap->rec, &h) : -ENOMEM;
    free(msgs);
    if (err < 0) {
        snprintf(error, size, "can't start recording (err: %d)", err);
    }

    return err;
}


static int set_hello(struct capture *cap, const uint8_t *msg, size_t len)
{
    uint8_t *copy = malloc(len ? len : 1);
    int err = 0;

    if (!copy) {
        return -ENOMEM;
    }
    memcpy(copy, msg, len);
    if (cap->have_hello) {
        hello_free(&cap->hello);
        cap->have_hello = false;
    }
    free(cap->hello_msg);
    cap->hello_msg = copy;
    cap->hello_len = len;

    err = hello_parse(msg, len, &cap->hello);
    if (err < 0) {
        return err;
    }
    cap->have_hello = true;

    // The host refuses a spec major it does not know (spec, Hello).
    return cap->hello.version_major == 1 ? 0 : -EPROTO;
}


static int config_add(struct capture *cap, uint16_t type, const uint8_t *data, size_t len)
{
    struct config_msg *bigger = realloc(cap->configs, (cap->config_count + 1) * sizeof(*bigger));
    uint8_t *copy = malloc(len ? len : 1);

    if (bigger) {
        cap->configs = bigger;
    }
    if (!bigger || !copy) {
        free(copy);
        return -ENOMEM;
    }
    memcpy(copy, data, len);
    cap->configs[cap->config_count++] = (struct config_msg){.type = type, .data = copy,
                                                            .len = len};

    return 0;
}


// Writes one register and keeps the message and its reply for the header.
static int configure(struct capture *cap, uint8_t reg, const uint8_t *value, size_t len,
                     char *error, size_t size)
{
    uint8_t msg[2 + 255];
    const uint8_t *reply = NULL;
    size_t reply_len = 0;
    struct config_reply rep;
    const struct reg *r = hello_reg(&cap->hello, reg);
    int n = config_msg(msg, sizeof(msg), reg, value, len);
    int err = n < 0 ? n : 0;

    err = err ? err : cap->ops->config(cap->src, msg, (size_t)n, &reply, &reply_len);
    err = err ? err : config_add(cap, THC_ITEM_CONFIG, msg, (size_t)n);
    err = err ? err : config_add(cap, THC_ITEM_CONFIG_REPLY, reply, reply_len);
    err = err ? err : config_reply_parse(reply, reply_len, &rep);
    if (err < 0) {
        snprintf(error, size, "configuration failed (err: %d)", err);
        return err;
    }
    if (rep.status != 0) {
        snprintf(error, size, "%s: 0x%02x %s%s%s", r ? r->name : "register", rep.status,
                 config_status_text(rep.status), rep.text[0] ? ": " : "", rep.text);
        return -EINVAL;
    }

    return 0;
}


static int session_init(struct capture *cap)
{
    struct wire_handler wh = {.record = on_record, .heartbeat = on_heartbeat, .ctx = cap};
    unsigned int n = cap->info.n;
    int err = 0;

    cap->wires = calloc(cap->info.data_channels, sizeof(*cap->wires));
    cap->records = calloc(n ? n : 1, sizeof(*cap->records));
    cap->lost = calloc(n ? n : 1, sizeof(*cap->lost));
    if (!cap->wires || !cap->records || !cap->lost) {
        return -ENOMEM;
    }
    for (unsigned int d = 0; d < cap->info.data_channels && err == 0; d++) {
        err = wire_init(&cap->wires[d], &cap->info, (uint16_t)d, &wh);
    }

    return err ? err : loss_tracker_init(&cap->lt, (uint16_t)n);
}


static void session_free(struct capture *cap)
{
    if (cap->wires) {
        for (unsigned int d = 0; d < cap->info.data_channels; d++) {
            wire_free(&cap->wires[d]);
        }
        free(cap->wires);
        cap->wires = NULL;
    }
    loss_tracker_free(&cap->lt);
    free(cap->records);
    free(cap->lost);
    cap->records = NULL;
    cap->lost = NULL;
}


// One item: recorded as it is, then parsed. offset is a file DATA chunk's file offset.
static int handle(struct capture *cap, const struct source_item *item, uint64_t offset)
{
    uint16_t ch = item->channel;
    int err = 0;

    if (item->time > cap->time) {
        cap->time = item->time;
    }

    if (item->type == THC_DATA) {
        if (cap->rec) {
            recorder_data(cap->rec, ch, item->time, item->data, item->len);
        }
        if (ch < cap->info.data_channels && cap->seek && cap->seek[ch].on) {
            struct seek *sk = &cap->seek[ch];

            // Skip data before the indexed heartbeat parsing starts from.
            if (offset < sk->chunk) {
                return 0;
            }
            sk->on = false;
            wire_start_at(&cap->wires[ch], sk->grid, true);
            if (offset == sk->chunk && sk->payload < item->len) {
                wire_feed(&cap->wires[ch], &item->data[sk->payload], item->len - sk->payload);
                return 0;
            }
        }
        if (ch < cap->info.data_channels) {
            if (cap->ib) {
                err = index_builder_chunk(cap->ib, ch, cap->wires[ch].stats.bytes, item->len,
                                          offset);
            }
            wire_feed(&cap->wires[ch], item->data, item->len);
        }
    } else if (item->type == THC_CONTROL) {
        if (cap->rec) {
            recorder_control(cap->rec, item->flags, item->time, item->data, item->len);
        }
        if (!(item->flags & THC_TO_DEVICE) && item->len > 0 &&
            item->data[0] == MSG_LOSS_REPORT) {
            loss_report(cap, item->data, item->len);
        }
    } else if (item->type == THC_EVENT && item->len >= 2) {
        uint16_t code = (uint16_t)thc_get_le(item->data, 2);

        if (cap->rec) {
            recorder_event(cap->rec, code, ch, item->time, &item->data[2], item->len - 2);
        }
        // Bytes dropped before they were parsed (by the backend live, or by the recording
        // in a file): the stream goes on at an arbitrary byte.
        if (code == THC_EV_OVERFLOW && item->len >= 10 && ch < cap->info.data_channels) {
            uint64_t bytes = thc_get_le(&item->data[2], 8);

            cap->overflow += bytes;
            if (bytes) {
                wire_break(&cap->wires[ch]);
            }
        }
        if (cap->handler.event) {
            cap->handler.event(cap->handler.ctx, code, ch, &item->data[2], item->len - 2);
        }
    }

    return err;
}


// A loss report: the counters' differences, the range to mark, and excused mismatches.
static void loss_report(struct capture *cap, const uint8_t *msg, size_t len)
{
    struct loss_report rep;
    struct loss loss;
    struct wire_span span;

    if (loss_report_parse(msg, len, cap->info.n, &rep) < 0) {
        return;
    }
    loss_tracker_take(&cap->lt, &rep, wire_tick(&cap->wires[0], rep.start_grid, rep.start_off),
                      wire_tick(&cap->wires[0], rep.end_grid, rep.end_off), &loss);
    cap->st.losses++;
    span.start = loss.start;
    span.end = loss.end;

    for (unsigned int d = 0; d < cap->info.data_channels; d++) {
        bool lost = false;

        for (unsigned int i = cap->info.block[d]; i < cap->info.block[d + 1]; i++) {
            lost = lost || loss.lost[i] > 0;
        }
        if (lost) {
            wire_add_loss_span(&cap->wires[d], &span);
        }
    }
    for (unsigned int i = 0; i < cap->info.n; i++) {
        cap->lost[i] += loss.lost[i];
    }
    if (cap->handler.loss) {
        cap->handler.loss(cap->handler.ctx, &loss);
    }
}


static void on_record(void *ctx, const struct record *r)
{
    struct capture *cap = ctx;
    int i = logical_index(&cap->info, r->channel);

    if (i >= 0) {
        cap->records[i]++;
    }
    if (r->tick > cap->st.now) {
        cap->st.now = r->tick;
    }
    if (cap->handler.record) {
        cap->handler.record(cap->handler.ctx, r);
    }
}


static void on_heartbeat(void *ctx, const struct heartbeat *hb)
{
    struct capture *cap = ctx;
    uint64_t pos = cap->wires[hb->data_channel].hb_pos;

    if (hb->tick > cap->st.now) {
        cap->st.now = hb->tick;
    }
    if (!(hb->flags & HEARTBEAT_UNVERIFIED)) {
        if (cap->rec) {
            recorder_heartbeat(cap->rec, hb->data_channel, hb->grid, pos);
        }
        if (cap->ib) {
            index_builder_heartbeat(cap->ib, hb->data_channel, hb->grid, pos);
        }
    }
    if (cap->handler.heartbeat) {
        cap->handler.heartbeat(cap->handler.ctx, hb);
    }
}


// A channel's logical index: its position among the enabled channels; -1 if not enabled.
static int logical_index(const struct capture_info *info, uint16_t channel)
{
    unsigned int lo = 0;
    unsigned int hi = info->n;

    while (lo < hi) {
        unsigned int mid = (lo + hi) / 2;

        if (info->enabled[mid] < channel) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    return lo < info->n && info->enabled[lo] == channel ? (int)lo : -1;
}


static uint64_t clock_ns(clockid_t id)
{
    struct timespec ts;

    clock_gettime(id, &ts);

    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
