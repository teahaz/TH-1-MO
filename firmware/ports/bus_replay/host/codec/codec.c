#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codec/codec.h"
#include "codec/item.h"
#include "codec/merge.h"
#include "common/capture_info.h"
#include "common/record.h"


/*
 * hold: ticks an item is kept after its end, so a loss reported late can still mark it.
 * level is per channel, -1 before a heartbeat or record gives it. pending holds emitted
 * top-level items not yet released, by start.
 */
struct codec {
    const struct capture_info *info;
    uint64_t hold;
    struct merge merge;
    struct codec_dec **decs;
    size_t dec_count;
    int8_t *level;

    struct codec_item **pending;
    size_t pend_head;
    size_t pend_len;
    size_t pend_cap;
    uint64_t next_id;

    uint64_t tick;
    bool finished;
    int error;
};


static void on_record(void *ctx, const struct record *r);
static void on_level(void *ctx, uint64_t tick, uint16_t channel, uint8_t level);
static void on_advance(void *ctx, uint64_t tick);
static bool reads(const struct codec_dec *d, uint16_t channel);
static bool lost_lines(const struct codec *c, const struct codec_dec *d, const struct loss *loss);
static void assign_ids(struct codec *c, struct codec_item *item);
static int pending_insert(struct codec *c, struct codec_item *item);
static int args_parse(const char *text, struct codec_args *args);
static void args_free(struct codec_args *args);
static int args_check(const struct codec_decoder *def, const struct codec_args *args,
                      char *error, size_t size);
static int lower_find(const struct codec *c, const struct codec_decoder *def, const char *on,
                      struct codec_dec **lower, char *error, size_t size);
static char *label(const struct codec *c, const char *want);
static void dec_free(struct codec_dec *d);


const struct codec_decoder *codec_find(const char *name)
{
    for (unsigned int i = 0; codec_decoders[i]; i++) {
        if (!strcmp(codec_decoders[i]->name, name)) {
            return codec_decoders[i];
        }
    }

    return NULL;
}


/*
 * A session over a capture described by info, which must outlive it. hold is in ticks (a
 * second of capture is a sensible value; 0 releases items as soon as they can be ordered).
 */
int codec_new(struct codec **out, const struct capture_info *info, uint64_t hold)
{
    struct merge_handler mh = {.record = on_record, .level = on_level, .advance = on_advance};
    struct codec *c = calloc(1, sizeof(*c));
    int err = 0;

    *out = NULL;
    if (!c) {
        return -ENOMEM;
    }
    c->info = info;
    c->hold = hold;
    c->level = malloc(info->channels ? info->channels : 1);
    if (!c->level) {
        free(c);
        return -ENOMEM;
    }
    memset(c->level, -1, info->channels ? info->channels : 1);
    mh.ctx = c;
    err = merge_init(&c->merge, info, &mh);
    if (err < 0) {
        free(c->level);
        free(c);
        return err;
    }
    *out = c;

    return 0;
}


void codec_free(struct codec *c)
{
    if (!c) {
        return;
    }
    for (size_t i = 0; i < c->dec_count; i++) {
        dec_free(c->decs[i]);
    }
    for (size_t i = 0; i < c->pend_len; i++) {
        codec_item_free(c->pending[c->pend_head + i]);
    }
    merge_free(&c->merge);
    free(c->pending);
    free(c->decs);
    free(c->level);
    free(c);
}


/*
 * Adds a decoder with its arguments ("key=value,..."). A stacked decoder goes on lower; with
 * lower NULL, on the decoder its on= argument names, else on the last decoder added of the
 * kind it reads. Returns -EINVAL with a message in error for arguments it does not take.
 */
int codec_add(struct codec *c, const struct codec_decoder *def, const char *args,
              struct codec_dec *lower, struct codec_dec **out, char *error, size_t size)
{
    struct codec_args a = {0};
    struct codec_dec **bigger = NULL;
    struct codec_dec *d = NULL;
    int err = 0;

    if (out) {
        *out = NULL;
    }
    err = args_parse(args ? args : "", &a);
    if (err < 0) {
        snprintf(error, size, "arguments must be key=value pairs, separated by commas");
        return err;
    }
    err = args_check(def, &a, error, size);
    if (err == 0 && def->input && !lower) {
        err = lower_find(c, def, codec_arg(&a, "on"), &lower, error, size);
    }
    if (err == 0 && lower && (!def->input || strcmp(lower->def->name, def->input))) {
        snprintf(error, size, "%s cannot go on a %s decoder", def->name, lower->def->name);
        err = -EINVAL;
    }
    if (err == 0) {
        snprintf(error, size, "out of memory");
    }
    d = err ? NULL : calloc(1, sizeof(*d));
    bigger = d ? realloc(c->decs, (c->dec_count + 1) * sizeof(*bigger)) : NULL;
    if (bigger) {
        c->decs = bigger;
    }
    if (err == 0 && (!d || !bigger)) {
        err = -ENOMEM;
    }
    if (err == 0) {
        d->codec = c;
        d->def = def;
        d->lower = lower;
        d->name = label(c, codec_arg(&a, "name") ? codec_arg(&a, "name") : def->name);
        err = d->name ? def->create(d, &a, error, size) : -ENOMEM;
    }
    args_free(&a);
    if (err < 0) {
        dec_free(d);
        return err;
    }
    c->decs[c->dec_count++] = d;
    if (out) {
        *out = d;
    }

    return 0;
}


size_t codec_count(const struct codec *c)
{
    return c->dec_count;
}


struct codec_dec *codec_dec_at(const struct codec *c, size_t i)
{
    return i < c->dec_count ? c->decs[i] : NULL;
}


int codec_record(struct codec *c, const struct record *r)
{
    return merge_record(&c->merge, r);
}


int codec_heartbeat(struct codec *c, const struct heartbeat *hb)
{
    return merge_heartbeat(&c->merge, hb);
}


/*
 * A loss: the decoders that read a line with lost records hear of it, and their items still
 * held that overlap it are marked.
 */
void codec_loss(struct codec *c, const struct loss *loss)
{
    for (size_t i = 0; i < c->dec_count; i++) {
        struct codec_dec *d = c->decs[i];

        if (!lost_lines(c, d, loss)) {
            continue;
        }
        for (size_t k = 0; k < c->pend_len; k++) {
            struct codec_item *it = c->pending[c->pend_head + k];

            if (it->dec == d && it->start <= loss->end && it->end >= loss->start) {
                it->flags |= CODEC_ITEM_LOSS;
            }
        }
        if (d->def->loss) {
            d->def->loss(d, loss->start, loss->end);
        }
    }
}


// The end of the input: everything held is handed over, and every item can be released.
void codec_finish(struct codec *c)
{
    merge_flush(&c->merge);
    for (size_t i = 0; i < c->dec_count; i++) {
        if (c->decs[i]->def->finish) {
            c->decs[i]->def->finish(c->decs[i], c->tick);
        }
    }
    c->finished = true;
}


/*
 * The next top-level item to release, NULL if none can be yet; the caller frees it. An item
 * is released once no decoder can emit one starting earlier, and the hold after its end has
 * passed.
 */
struct codec_item *codec_next(struct codec *c)
{
    struct codec_item *it = NULL;
    uint64_t horizon = UINT64_MAX;

    if (!c->pend_len) {
        return NULL;
    }
    it = c->pending[c->pend_head];
    if (!c->finished) {
        for (size_t i = 0; i < c->dec_count; i++) {
            if (c->decs[i]->horizon < horizon) {
                horizon = c->decs[i]->horizon;
            }
        }
        if (it->start >= horizon || it->end + c->hold > c->tick) {
            return NULL;
        }
    }
    c->pend_head++;
    c->pend_len--;

    return it;
}


// The session's first error (from a decoder or a full queue), 0 if none.
int codec_error(const struct codec *c)
{
    return c->error;
}


const struct capture_info *codec_info(const struct codec_dec *d)
{
    return d->codec->info;
}


// Adds channel to the lines the decoder reads.
int codec_dec_line(struct codec_dec *d, uint16_t channel)
{
    uint16_t *bigger = realloc(d->lines, (d->line_count + 1) * sizeof(*bigger));

    if (!bigger) {
        return -ENOMEM;
    }
    d->lines = bigger;
    d->lines[d->line_count++] = channel;

    return 0;
}


// A channel's level as far as the input has been handed over; -1 if not known yet.
int codec_level(const struct codec_dec *d, uint16_t channel)
{
    const struct codec *c = d->codec;

    return channel < c->info->channels ? c->level[channel] : -1;
}


/*
 * A finished top-level item from d, which the session takes over. The decoders stacked on d
 * see it first.
 */
void codec_emit(struct codec_dec *d, struct codec_item *item)
{
    struct codec *c = d->codec;
    int err = 0;

    for (size_t i = 0; i < c->dec_count; i++) {
        struct codec_dec *up = c->decs[i];

        if (up->lower == d && up->def->item) {
            up->def->item(up, item);
        }
    }
    assign_ids(c, item);
    err = pending_insert(c, item);
    if (err < 0) {
        codec_failed(d, err);
        codec_item_free(item);
    }
}


// d will not emit a top-level item starting before tick (call from advance).
void codec_horizon(struct codec_dec *d, uint64_t tick)
{
    if (tick < d->horizon) {
        d->horizon = tick;
    }
}


void codec_failed(struct codec_dec *d, int err)
{
    if (!d->codec->error) {
        d->codec->error = err;
    }
}


// The value of key, NULL if not given.
const char *codec_arg(const struct codec_args *args, const char *key)
{
    for (size_t i = 0; i < args->count; i++) {
        if (!strcmp(args->key[i], key)) {
            return args->value[i];
        }
    }

    return NULL;
}


// A channel of the capture. Not given: -ENOENT if required, else *channel is -1.
int codec_arg_channel(const struct codec_dec *d, const struct codec_args *args,
                      const char *key, bool required, int *channel, char *error, size_t size)
{
    const struct capture_info *info = codec_info(d);
    const char *v = codec_arg(args, key);
    char *end = NULL;
    unsigned long ch = 0;

    *channel = -1;
    if (!v) {
        if (required) {
            snprintf(error, size, "%s: %s= is required", d->def->name, key);
            return -ENOENT;
        }
        return 0;
    }
    ch = strtoul(v, &end, 0);
    if (!*v || *end || ch >= info->channels) {
        snprintf(error, size, "%s: %s=%s is not a channel of the device", d->def->name, key, v);
        return -EINVAL;
    }
    if (!channel_set_has(info->enabled_set, (unsigned int)ch)) {
        snprintf(error, size, "%s: %s=%s: channel %lu is not in the capture", d->def->name, key,
                 v, ch);
        return -EINVAL;
    }
    *channel = (int)ch;

    return 0;
}


int codec_arg_uint(const struct codec_args *args, const char *key, uint64_t def,
                   uint64_t *v, char *error, size_t size)
{
    const char *s = codec_arg(args, key);
    char *end = NULL;

    *v = def;
    if (!s) {
        return 0;
    }
    *v = strtoull(s, &end, 0);
    if (!*s || *end) {
        snprintf(error, size, "%s=%s is not a whole number", key, s);
        return -EINVAL;
    }

    return 0;
}


int codec_arg_real(const struct codec_args *args, const char *key, double def, double *v,
                   char *error, size_t size)
{
    const char *s = codec_arg(args, key);
    char *end = NULL;

    *v = def;
    if (!s) {
        return 0;
    }
    *v = strtod(s, &end);
    if (!*s || *end) {
        snprintf(error, size, "%s=%s is not a number", key, s);
        return -EINVAL;
    }

    return 0;
}


static void on_record(void *ctx, const struct record *r)
{
    struct codec *c = ctx;

    for (size_t i = 0; i < c->dec_count; i++) {
        struct codec_dec *d = c->decs[i];

        if (d->def->record && reads(d, r->channel)) {
            d->def->record(d, r);
        }
    }
    c->level[r->channel] = (int8_t)r->level;
}


static void on_level(void *ctx, uint64_t tick, uint16_t channel, uint8_t level)
{
    struct codec *c = ctx;

    c->level[channel] = (int8_t)level;
    for (size_t i = 0; i < c->dec_count; i++) {
        struct codec_dec *d = c->decs[i];

        if (d->def->level && reads(d, channel)) {
            d->def->level(d, tick, channel, level);
        }
    }
}


static void on_advance(void *ctx, uint64_t tick)
{
    struct codec *c = ctx;

    c->tick = tick;
    for (size_t i = 0; i < c->dec_count; i++) {
        struct codec_dec *d = c->decs[i];

        d->horizon = tick;
        if (d->def->advance) {
            d->def->advance(d, tick);
        }
    }
}


static bool reads(const struct codec_dec *d, uint16_t channel)
{
    for (size_t i = 0; i < d->line_count; i++) {
        if (d->lines[i] == channel) {
            return true;
        }
    }

    return false;
}


// Whether records of a line d depends on (its own, or its lower decoder's) were lost.
static bool lost_lines(const struct codec *c, const struct codec_dec *d, const struct loss *loss)
{
    const struct capture_info *info = c->info;

    for (; d; d = d->lower) {
        for (size_t k = 0; k < d->line_count; k++) {
            for (unsigned int i = 0; i < info->n && i < loss->n; i++) {
                if (info->enabled[i] == d->lines[k] && loss->lost[i] > 0) {
                    return true;
                }
            }
        }
    }

    return false;
}


// Numbers item and its children in order of emission, parents first.
static void assign_ids(struct codec *c, struct codec_item *item)
{
    item->id = ++c->next_id;
    for (struct codec_item *ch = item->child; ch; ch = ch->next) {
        assign_ids(c, ch);
    }
}


// Inserts into pending by start; items with the same start stay in order of emission.
static int pending_insert(struct codec *c, struct codec_item *item)
{
    size_t at = 0;

    if (c->pend_head + c->pend_len == c->pend_cap) {
        if (c->pend_head > 0) {
            memmove(c->pending, &c->pending[c->pend_head], c->pend_len * sizeof(*c->pending));
            c->pend_head = 0;
        } else {
            size_t cap = c->pend_cap ? 2 * c->pend_cap : 256;
            struct codec_item **bigger = realloc(c->pending, cap * sizeof(*bigger));

            if (!bigger) {
                return -ENOMEM;
            }
            c->pending = bigger;
            c->pend_cap = cap;
        }
    }

    at = c->pend_len;
    while (at > 0 && c->pending[c->pend_head + at - 1]->start > item->start) {
        at--;
    }
    memmove(&c->pending[c->pend_head + at + 1], &c->pending[c->pend_head + at],
            (c->pend_len - at) * sizeof(*c->pending));
    c->pending[c->pend_head + at] = item;
    c->pend_len++;

    return 0;
}


static int args_parse(const char *text, struct codec_args *args)
{
    char *copy = strdup(text);
    char *save = NULL;
    int err = 0;

    memset(args, 0, sizeof(*args));
    if (!copy) {
        return -ENOMEM;
    }
    for (char *tok = strtok_r(copy, ",", &save); tok && err == 0;
         tok = strtok_r(NULL, ",", &save)) {
        char *eq = strchr(tok, '=');
        char **keys = NULL;
        char **values = NULL;

        if (!eq || eq == tok) {
            err = -EINVAL;
            break;
        }
        *eq = '\0';
        keys = realloc(args->key, (args->count + 1) * sizeof(*keys));
        if (keys) {
            args->key = keys;
        }
        values = realloc(args->value, (args->count + 1) * sizeof(*values));
        if (values) {
            args->value = values;
        }
        if (!keys || !values) {
            err = -ENOMEM;
            break;
        }
        args->key[args->count] = strdup(tok);
        args->value[args->count] = strdup(eq + 1);
        args->count++;
        if (!args->key[args->count - 1] || !args->value[args->count - 1]) {
            err = -ENOMEM;
        }
    }
    free(copy);
    if (err < 0) {
        args_free(args);
    }

    return err;
}


static void args_free(struct codec_args *args)
{
    for (size_t i = 0; i < args->count; i++) {
        free(args->key[i]);
        free(args->value[i]);
    }
    free(args->key);
    free(args->value);
    memset(args, 0, sizeof(*args));
}


static int args_check(const struct codec_decoder *def, const struct codec_args *args,
                      char *error, size_t size)
{
    for (size_t i = 0; i < args->count; i++) {
        bool known = !strcmp(args->key[i], "name") ||
                     (def->input && !strcmp(args->key[i], "on"));

        for (unsigned int p = 0; def->params && def->params[p].name && !known; p++) {
            known = !strcmp(def->params[p].name, args->key[i]);
        }
        if (!known) {
            snprintf(error, size, "%s does not take %s=", def->name, args->key[i]);
            return -EINVAL;
        }
        for (size_t k = 0; k < i; k++) {
            if (!strcmp(args->key[k], args->key[i])) {
                snprintf(error, size, "%s: %s= is given twice", def->name, args->key[i]);
                return -EINVAL;
            }
        }
    }

    return 0;
}


/*
 * The decoder a stacked decoder goes on: the one labelled on, or with on NULL the last one
 * added of the kind it reads.
 */
static int lower_find(const struct codec *c, const struct codec_decoder *def, const char *on,
                      struct codec_dec **lower, char *error, size_t size)
{
    *lower = NULL;
    for (size_t i = c->dec_count; i-- > 0 && !*lower;) {
        const struct codec_dec *d = c->decs[i];

        if (on ? !strcmp(d->name, on) : !strcmp(d->def->name, def->input)) {
            *lower = c->decs[i];
        }
    }
    if (*lower) {
        return 0;
    }
    if (on) {
        snprintf(error, size, "%s: on=%s: no decoder has that label", def->name, on);
    } else {
        snprintf(error, size, "%s goes on a %s decoder; add one first", def->name, def->input);
    }

    return -EINVAL;
}


// The label want, or want#2, want#3 and so on if another decoder has it already.
static char *label(const struct codec *c, const char *want)
{
    size_t len = strlen(want) + 16;
    char *name = malloc(len);

    if (!name) {
        return NULL;
    }
    snprintf(name, len, "%s", want);
    for (unsigned int n = 2;; n++) {
        bool taken = false;

        for (size_t i = 0; i < c->dec_count && !taken; i++) {
            taken = !strcmp(c->decs[i]->name, name);
        }
        if (!taken) {
            return name;
        }
        snprintf(name, len, "%s#%u", want, n);
    }
}


static void dec_free(struct codec_dec *d)
{
    if (!d) {
        return;
    }
    if (d->def->destroy && d->state) {
        d->def->destroy(d);
    }
    free(d->lines);
    free(d->name);
    free(d);
}
