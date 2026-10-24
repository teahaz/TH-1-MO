/*
 * bus-replay export: a capture file's raw records, not decoded, in tick order: CSV
 * (tick,channel,level) or VCD, which waveform viewers such as PulseView and GTKWave open.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "capture/capture.h"
#include "codec/merge.h"
#include "common/capture_info.h"
#include "common/record.h"
#include "modes/modes.h"


/*
 * level is each channel's level as last written (VCD), -1 for not known; known is its level
 * as far as the input has got, also before --from.
 */
struct exporting {
    FILE *out;
    bool vcd;
    const struct capture_info *info;
    struct merge merge;
    uint64_t from;
    uint64_t to;
    bool started;
    bool have_time;
    bool timed;
    uint64_t time;
    int8_t *level;
    int8_t *known;
    int err;
};


static void on_record(void *ctx, const struct record *r);
static void on_heartbeat(void *ctx, const struct heartbeat *hb);
static void merged_record(void *ctx, const struct record *r);
static void merged_level(void *ctx, uint64_t tick, uint16_t channel, uint8_t level);
static void merged_advance(void *ctx, uint64_t tick);
static void change(struct exporting *x, uint64_t tick, uint16_t channel, int8_t level);
static void vcd_header(struct exporting *x);
static void vcd_start(struct exporting *x, uint64_t tick);
static void vcd_time(struct exporting *x, uint64_t tick);
static void vcd_id(char *buf, unsigned int i);
static uint64_t ns(const struct exporting *x, uint64_t tick);
static int parse_time(const char *s, uint32_t rate, uint64_t *tick);
static void usage(FILE *out);


int export_main(int argc, char **argv)
{
    struct exporting x = {.out = stdout, .to = UINT64_MAX};
    struct capture_handler handler = {.record = on_record, .heartbeat = on_heartbeat,
                                      .ctx = &x};
    struct merge_handler mh = {.record = merged_record, .level = merged_level,
                               .advance = merged_advance, .ctx = &x};
    struct capture *cap = NULL;
    const char *path = NULL;
    const char *from = NULL;
    const char *to = NULL;
    int err = 0;

    for (int i = 1; i < argc; i++) {
        if (arg_is(argv[i], "-h", "--help")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(argv[i], "--csv")) {
            x.vcd = false;
        } else if (!strcmp(argv[i], "--vcd")) {
            x.vcd = true;
        } else if (!strcmp(argv[i], "--from") && i + 1 < argc) {
            from = argv[++i];
        } else if (!strcmp(argv[i], "--to") && i + 1 < argc) {
            to = argv[++i];
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

    err = capture_open_file(&cap, path, &handler);
    if (err < 0) {
        fprintf(stderr, "bus-replay export: can't read %s: %s\n", path,
                err == -EPROTO ? "not a capture file, or its header is damaged" :
                                 strerror(-err));
        return 1;
    }
    x.info = capture_info(cap);
    if ((from && parse_time(from, x.info->rate, &x.from) < 0) ||
        (to && parse_time(to, x.info->rate, &x.to) < 0)) {
        fprintf(stderr, "bus-replay export: --from and --to take seconds\n");
        capture_close(cap);
        return 2;
    }
    x.level = malloc(x.info->channels ? x.info->channels : 1);
    x.known = malloc(x.info->channels ? x.info->channels : 1);
    mh.ctx = &x;
    err = x.level && x.known ? merge_init(&x.merge, x.info, &mh) : -ENOMEM;
    if (err < 0) {
        free(x.level);
        free(x.known);
        capture_close(cap);
        return 1;
    }
    memset(x.level, -1, x.info->channels ? x.info->channels : 1);
    memset(x.known, -1, x.info->channels ? x.info->channels : 1);
    if (x.from > 0) {
        capture_seek(cap, x.from);
    }

    if (x.vcd) {
        vcd_header(&x);
    } else {
        fprintf(x.out, "tick,channel,level\n");
    }
    while ((err = capture_pull(cap, 0)) > 0 && x.err == 0) {
        if (x.to != UINT64_MAX && x.merge.have_released && x.merge.released > x.to) {
            break;
        }
    }
    merge_flush(&x.merge);
    if (x.vcd && !x.started && x.have_time) {
        vcd_start(&x, x.from);
    }
    fflush(x.out);
    if (err < 0 && err != -ENODATA) {
        fprintf(stderr, "bus-replay export: reading %s failed (err: %d)\n", path, err);
    }
    err = err < 0 && err != -ENODATA ? err : x.err;

    merge_free(&x.merge);
    free(x.level);
    free(x.known);
    capture_close(cap);

    return err ? 1 : 0;
}


static void on_record(void *ctx, const struct record *r)
{
    struct exporting *x = ctx;
    int err = merge_record(&x->merge, r);

    if (err < 0 && !x->err) {
        x->err = err;
    }
}


static void on_heartbeat(void *ctx, const struct heartbeat *hb)
{
    struct exporting *x = ctx;
    int err = merge_heartbeat(&x->merge, hb);

    if (err < 0 && !x->err) {
        x->err = err;
    }
}


static void merged_record(void *ctx, const struct record *r)
{
    struct exporting *x = ctx;

    if (!x->vcd) {
        if (r->tick >= x->from && r->tick <= x->to) {
            fprintf(x->out, "%" PRIu64 ",%u,%u\n", r->tick, r->channel, r->level);
        }
        return;
    }
    change(x, r->tick, r->channel, (int8_t)r->level);
}


/*
 * A heartbeat's level: in a VCD, the first level of a channel, or one a loss corrected. It
 * does not start the VCD itself, so the first heartbeat's levels all go into its $dumpvars.
 */
static void merged_level(void *ctx, uint64_t tick, uint16_t channel, uint8_t level)
{
    struct exporting *x = ctx;

    if (!x->vcd) {
        return;
    }
    if (x->started) {
        change(x, tick, channel, (int8_t)level);
    } else {
        x->known[channel] = (int8_t)level;
        x->have_time = true;
    }
}


// Everything up to tick is known: a VCD starts once that reaches --from.
static void merged_advance(void *ctx, uint64_t tick)
{
    struct exporting *x = ctx;

    if (x->vcd && !x->started && tick >= x->from && tick <= x->to) {
        vcd_start(x, tick);
    }
}


static void change(struct exporting *x, uint64_t tick, uint16_t channel, int8_t level)
{
    char id[8];

    x->known[channel] = level;
    x->have_time = true;
    if (tick < x->from || tick > x->to) {
        return;
    }
    if (!x->started) {
        vcd_start(x, tick);
    }
    if (x->level[channel] == level) {
        return;
    }
    vcd_time(x, tick);
    for (unsigned int i = 0; i < x->info->n; i++) {
        if (x->info->enabled[i] == channel) {
            vcd_id(id, i);
            fprintf(x->out, "%d%s\n", level, id);
        }
    }
    x->level[channel] = level;
}


static void vcd_header(struct exporting *x)
{
    const struct capture_info *info = x->info;
    char id[8];

    fprintf(x->out, "$version bus-replay $end\n$timescale 1 ns $end\n");
    fprintf(x->out, "$scope module capture $end\n");
    for (unsigned int i = 0; i < info->n; i++) {
        vcd_id(id, i);
        fprintf(x->out, "$var wire 1 %s %s $end\n", id, info->names[info->enabled[i]]);
    }
    fprintf(x->out, "$upscope $end\n$enddefinitions $end\n");
}


// The levels at the first time written: what is known by then, x where nothing is.
static void vcd_start(struct exporting *x, uint64_t tick)
{
    char id[8];

    x->started = true;
    vcd_time(x, tick < x->from ? x->from : tick);
    fprintf(x->out, "$dumpvars\n");
    for (unsigned int i = 0; i < x->info->n; i++) {
        uint16_t ch = x->info->enabled[i];

        vcd_id(id, i);
        if (x->known[ch] < 0) {
            fprintf(x->out, "x%s\n", id);
        } else {
            fprintf(x->out, "%d%s\n", x->known[ch], id);
        }
        x->level[ch] = x->known[ch];
    }
    fprintf(x->out, "$end\n");
}


static void vcd_time(struct exporting *x, uint64_t tick)
{
    uint64_t t = ns(x, tick);

    if (!x->timed || x->time != t) {
        fprintf(x->out, "#%" PRIu64 "\n", t);
        x->time = t;
        x->timed = true;
    }
}


// VCD identifier i: printable characters from '!', several once the 94 single ones run out.
static void vcd_id(char *buf, unsigned int i)
{
    size_t n = 0;

    do {
        buf[n++] = (char)('!' + i % 94);
        i /= 94;
    } while (i && n < 7);
    buf[n] = '\0';
}


static uint64_t ns(const struct exporting *x, uint64_t tick)
{
    return (uint64_t)((double)tick * 1e9 / x->info->rate + 0.5);
}


static int parse_time(const char *s, uint32_t rate, uint64_t *tick)
{
    char *end = NULL;
    double v = strtod(s, &end);

    if (!*s || *end || v < 0) {
        return -EINVAL;
    }
    *tick = (uint64_t)(v * rate);

    return 0;
}


static void usage(FILE *out)
{
    fprintf(out,
            "usage: bus-replay export FILE [--csv|--vcd] [--from S] [--to S]\n"
            "\n"
            "Writes a capture's raw records, not decoded, to stdout: every level change on\n"
            "every captured channel.\n"
            "\n"
            "  --csv        tick,channel,level per line (the default); a tick is one sample,\n"
            "               so seconds = tick / sample rate (see info)\n"
            "  --vcd        a VCD file, for a waveform viewer such as GTKWave or PulseView\n"
            "  --from S     start S seconds into the capture (decimals allowed)\n"
            "  --to S       stop S seconds into the capture\n"
            "  -h, --help   this help\n");
}
