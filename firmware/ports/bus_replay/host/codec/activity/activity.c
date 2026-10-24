#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codec/activity/activity.h"
#include "codec/codec.h"
#include "codec/item.h"
#include "common/capture_info.h"
#include "common/record.h"


/*
 * gap and max in ticks (max 0: no limit). The open packet is open; changed marks the lines
 * that changed in it, by channel.
 */
struct activity {
    uint64_t gap;
    uint64_t max;
    struct codec_item *open;
    uint64_t last;
    uint64_t records;
    bool *changed;
};


static int create(struct codec_dec *d, const struct codec_args *args, char *error, size_t size);
static int add_lines(struct codec_dec *d, const char *list, char *error, size_t size);
static void destroy(struct codec_dec *d);
static void record(struct codec_dec *d, const struct record *r);
static void advance(struct codec_dec *d, uint64_t tick);
static void loss(struct codec_dec *d, uint64_t start, uint64_t end);
static void finish(struct codec_dec *d, uint64_t tick);
static void packet_close(struct codec_dec *d);


static const struct codec_field_desc packet_fields[] = {
    {.name = "records", .type = CODEC_UINT, .flags = CODEC_FIELD_HEADER, .width = 12,
     .unit = "records"},
    {.name = "lines", .type = CODEC_TEXT, .flags = CODEC_FIELD_HEADER, .width = 8},
    {.name = "length", .type = CODEC_REAL, .flags = CODEC_FIELD_HEADER, .digits = 3,
     .unit = "ms"},
};

const struct codec_kind activity_packet = {
    .name = "packet",
    .fields = packet_fields,
    .field_count = sizeof(packet_fields) / sizeof(packet_fields[0]),
};

static const struct codec_param params[] = {
    {"lines", "channels to watch, joined by +, for example 0+1+3 (default all captured)"},
    {"gap", "seconds with no change on any of them that end a packet (default 300e-6)"},
    {"max", "longest packet in seconds; a longer one is cut (default no limit)"},
    {NULL, NULL},
};

const struct codec_decoder activity_decoder = {
    .name = "activity",
    .summary = "packets of activity on any line, split by idle gaps",
    .params = params,
    .create = create,
    .destroy = destroy,
    .record = record,
    .advance = advance,
    .loss = loss,
    .finish = finish,
};


static int create(struct codec_dec *d, const struct codec_args *args, char *error, size_t size)
{
    const struct capture_info *info = codec_info(d);
    struct activity *s = NULL;
    double gap = 0;
    double max = 0;
    int err = codec_arg_real(args, "gap", 300e-6, &gap, error, size);

    err = err ? err : codec_arg_real(args, "max", 0, &max, error, size);
    if (err == 0 && (gap <= 0 || max < 0)) {
        snprintf(error, size, "activity: gap= must be above 0, max= at least 0");
        err = -EINVAL;
    }
    if (err < 0) {
        return err;
    }
    s = calloc(1, sizeof(*s));
    if (!s) {
        return -ENOMEM;
    }
    d->state = s;
    s->gap = (uint64_t)(gap * info->rate);
    s->max = (uint64_t)(max * info->rate);
    s->changed = calloc(info->channels ? info->channels : 1, sizeof(*s->changed));
    if (!s->changed) {
        return -ENOMEM;
    }

    if (codec_arg(args, "lines")) {
        return add_lines(d, codec_arg(args, "lines"), error, size);
    }
    for (unsigned int i = 0; i < info->n && err == 0; i++) {
        err = codec_dec_line(d, info->enabled[i]);
    }

    return err;
}


static int add_lines(struct codec_dec *d, const char *list, char *error, size_t size)
{
    const struct capture_info *info = codec_info(d);
    const char *p = list;
    int err = 0;

    while (*p && err == 0) {
        char *end = NULL;
        unsigned long ch = strtoul(p, &end, 10);

        if (end == p || (*end && *end != '+') || ch >= info->channels ||
            !channel_set_has(info->enabled_set, (unsigned int)ch)) {
            snprintf(error, size, "activity: lines=%s: give captured channels joined by +",
                     list);
            return -EINVAL;
        }
        err = codec_dec_line(d, (uint16_t)ch);
        p = *end ? end + 1 : end;
    }

    return err;
}


static void destroy(struct codec_dec *d)
{
    struct activity *s = d->state;

    codec_item_free(s->open);
    free(s->changed);
    free(s);
}


static void record(struct codec_dec *d, const struct record *r)
{
    struct activity *s = d->state;

    if (s->open && (r->tick - s->last > s->gap || (s->max && r->tick - s->open->start > s->max))) {
        packet_close(d);
    }
    if (!s->open) {
        s->open = codec_item_new(d, &activity_packet, r->tick);
        if (!s->open) {
            codec_failed(d, -ENOMEM);
            return;
        }
        s->records = 0;
        memset(s->changed, 0, codec_info(d)->channels * sizeof(*s->changed));
    }
    s->open->end = r->tick;
    s->last = r->tick;
    s->records++;
    s->changed[r->channel] = true;
}


// A packet is over once every line has been idle for the gap.
static void advance(struct codec_dec *d, uint64_t tick)
{
    struct activity *s = d->state;

    if (s->open && tick - s->last > s->gap) {
        packet_close(d);
    }
    if (s->open) {
        codec_horizon(d, s->open->start);
    }
}


static void loss(struct codec_dec *d, uint64_t start, uint64_t end)
{
    struct activity *s = d->state;

    (void)start;
    if (s->open && s->open->start <= end) {
        s->open->flags |= CODEC_ITEM_LOSS;
    }
}


static void finish(struct codec_dec *d, uint64_t tick)
{
    struct activity *s = d->state;

    (void)tick;
    if (s->open) {
        packet_close(d);
    }
}


static void packet_close(struct codec_dec *d)
{
    const struct capture_info *info = codec_info(d);
    struct activity *s = d->state;
    struct codec_item *p = s->open;
    char lines[256] = "";
    size_t len = 0;

    for (unsigned int ch = 0; ch < info->channels; ch++) {
        if (s->changed[ch] && len + 8 < sizeof(lines)) {
            len += (size_t)snprintf(&lines[len], sizeof(lines) - len, "%s%u", len ? "+" : "",
                                    ch);
        }
    }
    codec_set_uint(p, ACTIVITY_RECORDS, s->records);
    codec_set_text(p, ACTIVITY_LINES, lines);
    codec_set_real(p, ACTIVITY_LENGTH, (double)(p->end - p->start + 1) * 1e3 / info->rate);
    s->open = NULL;
    codec_emit(d, p);
}
