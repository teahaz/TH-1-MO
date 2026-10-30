#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "codec/merge.h"
#include "common/capture_info.h"
#include "common/record.h"


#define NO_DATA_CHANNEL 0xFFFF


static int push(struct merge_queue *q, const struct merge_entry *e);
static void release(struct merge *m, bool all);
static struct merge_queue *next_queue(struct merge *m, uint64_t limit, bool all);


int merge_init(struct merge *m, const struct capture_info *info,
               const struct merge_handler *handler)
{
    memset(m, 0, sizeof(*m));
    m->info = info;
    m->handler = *handler;
    m->q = calloc(info->data_channels ? info->data_channels : 1, sizeof(*m->q));
    m->data_channel = calloc(info->channels ? info->channels : 1, sizeof(*m->data_channel));
    if (!m->q || !m->data_channel) {
        merge_free(m);
        return -ENOMEM;
    }
    for (unsigned int ch = 0; ch < info->channels; ch++) {
        m->data_channel[ch] = NO_DATA_CHANNEL;
    }
    for (unsigned int d = 0; d < info->data_channels; d++) {
        m->q[d].empty_block = info->block[d] == info->block[d + 1];
        for (unsigned int i = info->block[d]; i < info->block[d + 1]; i++) {
            m->data_channel[info->enabled[i]] = (uint16_t)d;
        }
    }

    return 0;
}


void merge_free(struct merge *m)
{
    if (m->q) {
        for (unsigned int d = 0; d < m->info->data_channels; d++) {
            free(m->q[d].e);
        }
    }
    free(m->q);
    free(m->data_channel);
    m->q = NULL;
    m->data_channel = NULL;
}


// A record of an enabled channel; others are ignored.
int merge_record(struct merge *m, const struct record *r)
{
    struct merge_entry e = {.tick = r->tick, .channel = r->channel, .level = r->level};
    uint16_t d = r->channel < m->info->channels ? m->data_channel[r->channel] : NO_DATA_CHANNEL;

    if (d == NO_DATA_CHANNEL) {
        return 0;
    }

    return push(&m->q[d], &e);
}


// A heartbeat: its levels follow the records of its data channel, and it moves that data
// channel's watermark on; whatever every data channel has passed is handed over.
int merge_heartbeat(struct merge *m, const struct heartbeat *hb)
{
    const struct capture_info *info = m->info;
    struct merge_queue *q = NULL;
    int err = 0;

    if (hb->data_channel >= info->data_channels) {
        return 0;
    }
    q = &m->q[hb->data_channel];
    for (unsigned int i = info->block[hb->data_channel];
         i < info->block[hb->data_channel + 1] && err == 0; i++) {
        struct merge_entry e = {.tick = hb->tick, .channel = info->enabled[i],
                                .level = heartbeat_level(hb, i), .is_level = true};

        err = push(q, &e);
    }
    if (!q->have_wm || hb->tick > q->wm) {
        q->wm = hb->tick;
        q->have_wm = true;
    }
    release(m, false);

    return err;
}


// A channel's level at the end of tick, as a heartbeat of its data channel would give it.
int merge_level(struct merge *m, uint64_t tick, uint16_t channel, uint8_t level)
{
    struct merge_entry e = {.tick = tick, .channel = channel, .level = level, .is_level = true};
    uint16_t d = channel < m->info->channels ? m->data_channel[channel] : NO_DATA_CHANNEL;

    if (d == NO_DATA_CHANNEL) {
        return 0;
    }

    return push(&m->q[d], &e);
}


// Every data channel has handed over everything up to and including tick.
int merge_advance(struct merge *m, uint64_t tick)
{
    for (unsigned int d = 0; d < m->info->data_channels; d++) {
        struct merge_queue *q = &m->q[d];

        if (!q->have_wm || tick > q->wm) {
            q->wm = tick;
            q->have_wm = true;
        }
    }
    release(m, false);

    return 0;
}


// Hands over everything still held, at the end of the input.
void merge_flush(struct merge *m)
{
    release(m, true);
}


static int push(struct merge_queue *q, const struct merge_entry *e)
{
    if (q->head + q->len == q->cap) {
        if (q->head > q->cap / 2) {
            memmove(q->e, &q->e[q->head], q->len * sizeof(*q->e));
            q->head = 0;
        } else {
            size_t cap = q->cap ? 2 * q->cap : 1024;
            struct merge_entry *bigger = realloc(q->e, cap * sizeof(*bigger));

            if (!bigger) {
                return -ENOMEM;
            }
            q->e = bigger;
            q->cap = cap;
        }
    }
    q->e[q->head + q->len++] = *e;

    return 0;
}


static void release(struct merge *m, bool all)
{
    const struct merge_handler *h = &m->handler;
    uint64_t limit = UINT64_MAX;
    uint64_t last = m->released;
    struct merge_queue *q = NULL;
    bool any = false;

    for (unsigned int d = 0; d < m->info->data_channels && !all; d++) {
        if (m->q[d].empty_block) {
            continue;
        }
        if (!m->q[d].have_wm) {
            return;
        }
        if (m->q[d].wm < limit) {
            limit = m->q[d].wm;
        }
        any = true;
    }
    if (!any && !all) {
        return;
    }

    while ((q = next_queue(m, limit, all))) {
        const struct merge_entry *e = &q->e[q->head];

        if (e->is_level) {
            if (h->level) {
                h->level(h->ctx, e->tick, e->channel, e->level);
            }
        } else if (h->record) {
            struct record r = {.tick = e->tick, .channel = e->channel, .level = e->level};

            h->record(h->ctx, &r);
        }
        if (e->tick > last) {
            last = e->tick;
        }
        q->head++;
        q->len--;
    }

    if (!all) {
        last = limit;
    }
    if (!m->have_released || last > m->released) {
        m->released = last;
        m->have_released = true;
        if (h->advance) {
            h->advance(h->ctx, last);
        }
    }
}


// The queue holding the next entry to hand over, up to limit; NULL if none.
static struct merge_queue *next_queue(struct merge *m, uint64_t limit, bool all)
{
    struct merge_queue *best = NULL;
    const struct merge_entry *b = NULL;

    for (unsigned int d = 0; d < m->info->data_channels; d++) {
        struct merge_queue *q = &m->q[d];
        const struct merge_entry *e = NULL;

        if (!q->len) {
            continue;
        }
        e = &q->e[q->head];
        if (!all && e->tick > limit) {
            continue;
        }
        if (!b || e->tick < b->tick ||
            (e->tick == b->tick && (e->is_level < b->is_level ||
                                    (e->is_level == b->is_level && e->channel < b->channel)))) {
            best = q;
            b = e;
        }
    }

    return best;
}
