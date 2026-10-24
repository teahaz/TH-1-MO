/*
 * bus-replay info: what a capture file holds. A complete file's numbers come from its END
 * chunk, unless --parse asks for them from a parse; any other file is parsed to get them.
 * A missing or stale index file is rebuilt.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "capture/capture.h"
#include "capture/control.h"
#include "capture/index.h"
#include "capture/thcfile.h"
#include "common/capture_info.h"
#include "common/status.h"
#include "modes/modes.h"


// Everything printed, from the trailer or from a parse.
struct summary {
    const char *path;
    const struct thc_header *header;
    const struct hello *hello;
    const struct capture_info *info;
    bool complete;
    enum thc_state state;
    const struct thc_trailer *trailer;
    const char *index;
};


static void print_text(const struct summary *s);
static void print_json(const struct summary *s);
static void json_str(FILE *out, const char *s, size_t len);
static const char *reason_text(uint8_t reason);
static const char *state_text(enum thc_state state);
static void start_text(uint64_t ns, char *buf, size_t size);
static void usage(FILE *out);


int info_main(int argc, char **argv)
{
    struct capture *cap = NULL;
    struct thc_reader *reader = NULL;
    struct thc_trailer parsed = {0};
    struct summary s = {0};
    const char *path = NULL;
    char *ipath = NULL;
    bool json = false;
    bool parse = false;
    int err = 0;

    for (int i = 1; i < argc; i++) {
        if (arg_is(argv[i], "-h", "--help")) {
            usage(stdout);
            return 0;
        } else if (arg_is(argv[i], "-j", "--json")) {
            json = true;
        } else if (arg_is(argv[i], "-p", "--parse")) {
            parse = true;
        } else if (!path && argv[i][0] != '-') {
            path = argv[i];
        } else {
            usage(stderr);
            return 2;
        }
    }
    if (!path) {
        usage(stderr);
        return 2;
    }

    err = capture_open_file(&cap, path, NULL);
    if (err < 0) {
        fprintf(stderr, "bus-replay info: can't read %s: %s\n", path,
                err == -EPROTO ? "not a capture file, or its header is damaged" :
                err == -ENOTSUP ? "a format version this host does not know" :
                                   strerror(-err));
        return 1;
    }
    reader = capture_reader(cap);
    s.path = path;
    s.header = thc_reader_header(reader);
    capture_hello(cap, &s.hello);
    s.info = capture_info(cap);
    s.complete = thc_reader_complete(reader);

    if (s.complete && !parse) {
        s.trailer = thc_reader_trailer(reader);
        s.state = THC_ENDED;
    } else {
        while ((err = capture_pull(cap, 0)) > 0) {
        }
        if (err != -ENODATA) {
            fprintf(stderr, "bus-replay info: reading %s failed (err: %d)\n", path, err);
            capture_close(cap);
            return 1;
        }
        err = capture_trailer(cap, 0xFF, &parsed);
        if (err < 0) {
            capture_close(cap);
            return 1;
        }
        parsed.time = capture_time(cap);
        s.state = thc_reader_state(reader);
        // How it ended is only in the END chunk.
        if (s.complete) {
            parsed.reason = thc_reader_trailer(reader)->reason;
            parsed.time = thc_reader_trailer(reader)->time;
            s.state = THC_ENDED;
        }
        s.trailer = &parsed;
    }

    ipath = index_path(path);
    err = ipath ? index_check(ipath, s.header->crc, s.header->start_time) : -ENOMEM;
    if (err == 0) {
        s.index = "ok";
    } else if (err == -ENOENT || err == -ESTALE) {
        s.index = capture_index_rebuild(path) == 0 ?
                      (err == -ENOENT ? "rebuilt (was missing)" : "rebuilt (was stale)") :
                      "missing, and rebuilding it failed";
    } else {
        s.index = "unknown";
    }
    free(ipath);

    if (json) {
        print_json(&s);
    } else {
        print_text(&s);
    }

    thc_trailer_free(&parsed);
    capture_close(cap);

    return 0;
}


static void print_text(const struct summary *s)
{
    const struct thc_header *h = s->header;
    const struct thc_trailer *t = s->trailer;
    const struct capture_info *info = s->info;
    char start[64];

    start_text(h->start_time, start, sizeof(start));
    printf("file        %s\n", s->path);
    printf("state       %s\n", s->complete ? "complete" : state_text(s->state));
    printf("format      %u.%u, spec %u.%u\n", h->major, h->minor, h->spec_major,
           h->spec_minor);
    printf("device      %s, firmware %s, uid ", s->hello->model, s->hello->firmware);
    for (unsigned int i = 0; i < s->hello->uid_len; i++) {
        printf("%02x", s->hello->uid[i]);
    }
    printf("\n");
    if (h->binding) {
        printf("binding     %.*s\n", (int)h->binding_len, (const char *)h->binding);
    }
    if (h->host) {
        printf("host        %.*s\n", (int)h->host_len, (const char *)h->host);
    }
    printf("started     %s\n", start);
    if (h->first_grid) {
        printf("first grid  %llu (recording started during the capture)\n",
               (unsigned long long)h->first_grid);
    }
    printf("duration    %.3f s, last tick %llu (%.3f s of capture)\n", (double)t->time / 1e9,
           (unsigned long long)t->last_tick, (double)t->last_tick / info->rate);
    if (s->complete) {
        printf("ended       %s\n", reason_text(t->reason));
    }
    printf("capture     %u Hz, C %u, %u of %u channels, %u data channel%s\n", info->rate,
           info->c, info->n, info->channels, info->data_channels,
           info->data_channels == 1 ? "" : "s");
    printf("loss        %llu reports, %llu mismatches, %llu excused, %llu bytes dropped by "
           "the host\n",
           (unsigned long long)t->loss_reports, (unsigned long long)t->mismatches,
           (unsigned long long)t->excused, (unsigned long long)t->overflow);

    printf("\n%-8s %-12s %12s %12s\n", "channel", "name", "records", "lost");
    for (unsigned int i = 0; i < info->n; i++) {
        unsigned int ch = info->enabled[i];

        printf("%-8u %-12s %12llu %12llu\n", ch, info->names[ch],
               (unsigned long long)t->records[i], (unsigned long long)t->lost[i]);
    }

    printf("\n%-8s %12s %12s %12s %8s %8s %8s\n", "data", "bytes", "records", "heartbeats",
           "crc", "gap", "resyncs");
    for (unsigned int d = 0; d < t->data_channels; d++) {
        const struct data_stats *ds = &t->data[d];

        printf("%-8u %12llu %12llu %12llu %8llu %8llu %8llu\n", d,
               (unsigned long long)ds->bytes, (unsigned long long)ds->records,
               (unsigned long long)ds->heartbeats, (unsigned long long)ds->crc_errors,
               (unsigned long long)ds->gap_errors, (unsigned long long)ds->resyncs);
    }
    printf("\nindex       %s\n", s->index);
}


static void print_json(const struct summary *s)
{
    const struct thc_header *h = s->header;
    const struct thc_trailer *t = s->trailer;
    const struct capture_info *info = s->info;

    printf("{\"file\":");
    json_str(stdout, s->path, strlen(s->path));
    printf(",\"complete\":%s,\"state\":", s->complete ? "true" : "false");
    json_str(stdout, state_text(s->state), strlen(state_text(s->state)));
    printf(",\"format\":\"%u.%u\",\"spec\":\"%u.%u\"", h->major, h->minor, h->spec_major,
           h->spec_minor);
    printf(",\"device\":{\"model\":");
    json_str(stdout, s->hello->model, strlen(s->hello->model));
    printf(",\"firmware\":");
    json_str(stdout, s->hello->firmware, strlen(s->hello->firmware));
    printf(",\"uid\":\"");
    for (unsigned int i = 0; i < s->hello->uid_len; i++) {
        printf("%02x", s->hello->uid[i]);
    }
    printf("\"}");
    if (h->binding) {
        printf(",\"binding\":");
        json_str(stdout, (const char *)h->binding, h->binding_len);
    }
    if (h->host) {
        printf(",\"host\":");
        json_str(stdout, (const char *)h->host, h->host_len);
    }
    printf(",\"start_time_ns\":%llu,\"first_grid\":%llu,\"duration_ns\":%llu,"
           "\"last_tick\":%llu",
           (unsigned long long)h->start_time, (unsigned long long)h->first_grid,
           (unsigned long long)t->time, (unsigned long long)t->last_tick);
    if (s->complete) {
        printf(",\"end_reason\":");
        json_str(stdout, reason_text(t->reason), strlen(reason_text(t->reason)));
    }
    printf(",\"rate\":%u,\"c\":%u,\"channel_count\":%u,\"data_channel_count\":%u", info->rate,
           info->c, info->channels, info->data_channels);
    printf(",\"loss_reports\":%llu,\"mismatches\":%llu,\"excused\":%llu,\"overflow_bytes\":%llu",
           (unsigned long long)t->loss_reports, (unsigned long long)t->mismatches,
           (unsigned long long)t->excused, (unsigned long long)t->overflow);

    printf(",\"channels\":[");
    for (unsigned int i = 0; i < info->n; i++) {
        unsigned int ch = info->enabled[i];

        printf("%s{\"channel\":%u,\"name\":", i ? "," : "", ch);
        json_str(stdout, info->names[ch], strlen(info->names[ch]));
        printf(",\"records\":%llu,\"lost\":%llu}", (unsigned long long)t->records[i],
               (unsigned long long)t->lost[i]);
    }
    printf("],\"data_channels\":[");
    for (unsigned int d = 0; d < t->data_channels; d++) {
        const struct data_stats *ds = &t->data[d];

        printf("%s{\"bytes\":%llu,\"records\":%llu,\"heartbeats\":%llu,\"crc_errors\":%llu,"
               "\"gap_errors\":%llu,\"resyncs\":%llu}",
               d ? "," : "", (unsigned long long)ds->bytes, (unsigned long long)ds->records,
               (unsigned long long)ds->heartbeats, (unsigned long long)ds->crc_errors,
               (unsigned long long)ds->gap_errors, (unsigned long long)ds->resyncs);
    }
    printf("],\"index\":");
    json_str(stdout, s->index, strlen(s->index));
    printf("}\n");
}


static void json_str(FILE *out, const char *s, size_t len)
{
    fputc('"', out);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];

        if (c == '"' || c == '\\') {
            fprintf(out, "\\%c", c);
        } else if (c < 0x20) {
            fprintf(out, "\\u%04x", c);
        } else {
            fputc(c, out);
        }
    }
    fputc('"', out);
}


static const char *reason_text(uint8_t reason)
{
    switch (reason) {
    case THC_REASON_USER:
        return "stopped by the user";
    case THC_REASON_DURATION:
        return "the requested duration was reached";
    case THC_REASON_SIGNAL:
        return "the host was signalled to quit";
    case THC_REASON_ERROR:
        return "a data or control error";
    case THC_REASON_OFF:
        return "recording turned off, the capture went on";
    default:
        return "unknown";
    }
}


static const char *state_text(enum thc_state state)
{
    switch (state) {
    case THC_ENDED:
        return "complete";
    case THC_EOF:
        return "incomplete: no END chunk";
    case THC_CUT:
        return "incomplete: the last chunk is cut short";
    case THC_BAD_CRC:
        return "incomplete: a chunk fails its CRC, read up to it";
    default:
        return "incomplete";
    }
}


static void start_text(uint64_t ns, char *buf, size_t size)
{
    time_t sec = (time_t)(ns / 1000000000ULL);
    struct tm tm;
    size_t n = 0;

    if (!localtime_r(&sec, &tm)) {
        snprintf(buf, size, "%llu ns", (unsigned long long)ns);
        return;
    }
    n = strftime(buf, size, "%Y-%m-%d %H:%M:%S", &tm);
    snprintf(&buf[n], size - n, ".%03u %s", (unsigned int)(ns / 1000000 % 1000),
             tm.tm_zone ? tm.tm_zone : "");
}


static void usage(FILE *out)
{
    fprintf(out,
            "usage: bus-replay info FILE [-j] [-p]\n"
            "\n"
            "Shows what a .thcapture file holds: the device, the capture's settings, when it\n"
            "ran and why it ended, and per channel the records and losses. A file cut short\n"
            "(a killed capture, or one still running) is read up to its last good chunk.\n"
            "\n"
            "  -j, --json   the same as one JSON object, for scripts\n"
            "  -p, --parse  count everything by reading the whole file, instead of taking the\n"
            "               totals written at its end; slower, and a check of those totals\n"
            "  -h, --help   this help\n");
}
