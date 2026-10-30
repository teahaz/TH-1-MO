/*
 * The store (store/store.h). Per channel: its records in blocks, the kept heartbeats as
 * anchors (a level from the end of a tick on), the spans its level is unknown in, and the
 * spans its losses cover, each sorted by tick so every query is a few binary searches.
 * Per data channel: the watermark and the heartbeats of the last two seconds, for a loss
 * that is reported late.
 */

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "common/capture_info.h"
#include "common/record.h"
#include "ui/store/batch.h"
#include "ui/store/store.h"


#define BLOCK_SHIFT 16
#define BLOCK_MASK (STORE_BLOCK_RECORDS - 1)
#define BLOCK_BYTES ((size_t)STORE_BLOCK_RECORDS * sizeof(uint64_t))
#define NONE 0xFFFF
#define OPEN UINT64_MAX
// Heartbeats held for late losses: two seconds of capture, or this many without a rate.
#define LATE_S 2
#define LATE_BEATS 1024

_Static_assert(STORE_BLOCK_RECORDS == 1 << BLOCK_SHIFT, "block size");

// A kept heartbeat's level for one channel: the level from the end of tick on.
struct anchor {
    uint64_t tick;
    uint8_t level;
};

/*
 * The level is unknown from start to known - 1 (known is OPEN until it is known). from is
 * the first tick a record or heartbeat can make it known at: a loss's end + 1, or 0 for the
 * start of the capture.
 */
struct span {
    uint64_t start;
    uint64_t from;
    uint64_t known;
};

// Ticks start to end, inclusive.
struct ival {
    uint64_t start;
    uint64_t end;
};

// A loss as reported, with its counts per logical channel.
struct lossrec {
    uint64_t start;
    uint64_t end;
    uint64_t total;
    uint32_t *lost;
};

// A data channel's recent heartbeats, oldest first, in a circular buffer.
struct ring {
    uint64_t *tick;
    uint8_t *levels;
    size_t head;
    size_t len;
    size_t cap;
};

struct data {
    bool have_wm;
    uint64_t wm;
    struct ring ring;
};

/*
 * A channel. blocks[head] to blocks[count - 1] are held, all full but the last, which has
 * last records. Its oldest record held has position first. Once blocks were dropped, cover
 * is the tick of the last record dropped and base the level at its end.
 */
struct chan {
    uint16_t ch;
    uint16_t index;
    uint16_t dc;

    uint64_t **blocks;
    size_t head;
    size_t count;
    size_t cap;
    size_t last;
    uint64_t first;
    uint64_t next;

    bool dropped;
    uint64_t cover;
    uint8_t base;

    bool have_last;
    uint64_t last_tick;
    uint8_t cur;

    struct anchor *anchors;
    size_t anchor_len;
    size_t anchor_cap;
    struct span *spans;
    size_t span_len;
    size_t span_cap;
    struct ival *ivals;
    size_t ival_len;
    size_t ival_cap;
};

struct store {
    pthread_mutex_t lock;
    const struct capture_info *info;
    size_t limit;
    size_t level_bytes;
    uint64_t late;

    uint16_t *map;
    struct chan *chans;
    struct data *data;

    size_t blocks;
    uint64_t oldest;
    bool have_newest;
    uint64_t newest;

    struct lossrec *losses;
    size_t loss_len;
    size_t loss_cap;
    uint64_t loss_total;
};


static int apply_record(struct store *s, const struct batch_entry *e);
static int apply_heartbeat(struct store *s, const struct store_batch *b,
                           const struct batch_entry *e);
static int apply_loss(struct store *s, const struct store_batch *b, const struct batch_entry *e);
static int block_add(struct store *s, struct chan *c);
static void enforce(struct store *s);
static bool drop_oldest(struct store *s);
static bool trim_side(struct store *s);
static void trim_to(struct store *s, struct chan *c, uint64_t t);
static size_t bytes_used(const struct store *s);
static int span_add(struct store *s, struct chan *c, uint64_t start, uint64_t from);
static uint64_t span_resolve(struct store *s, struct chan *c, uint64_t from, bool *beat,
                             uint8_t *level);
static int ival_add(struct chan *c, uint64_t start, uint64_t end);
static int anchor_put(struct chan *c, uint64_t tick, uint8_t level);
static int ring_push(struct store *s, struct ring *r, uint64_t tick, const uint8_t *levels);
static struct chan *chan_of(const struct store *s, uint16_t channel);
static uint64_t rec_at(const struct chan *c, uint64_t k);
static uint64_t held(const struct chan *c);
static uint64_t count_lt(const struct chan *c, uint64_t tick);
static uint64_t count_le(const struct chan *c, uint64_t tick);
static uint8_t level_k(const struct chan *c, uint64_t tick, uint64_t k);
static uint8_t level_at(const struct chan *c, uint64_t tick);
static bool unknown_in(const struct chan *c, uint64_t a, uint64_t b);
static bool loss_in(const struct chan *c, uint64_t a, uint64_t b);
static void drop_front(void **p, size_t *len, size_t *cap, size_t keep, size_t size);
static int grow(void **p, size_t *cap, size_t need, size_t size);


int store_new(struct store **out, const struct capture_info *info,
              const struct store_options *opt)
{
    struct store *s = calloc(1, sizeof(*s));

    if (!s) {
        return -ENOMEM;
    }
    s->info = info;
    s->limit = opt && opt->limit ? opt->limit : STORE_LIMIT_DEFAULT;
    s->level_bytes = (info->n + 7) / 8;
    s->late = (uint64_t)info->rate * LATE_S;
    s->map = malloc((info->channels ? info->channels : 1) * sizeof(*s->map));
    s->chans = calloc(info->n ? info->n : 1, sizeof(*s->chans));
    s->data = calloc(info->data_channels ? info->data_channels : 1, sizeof(*s->data));
    if (!s->map || !s->chans || !s->data) {
        store_free(s);
        return -ENOMEM;
    }
    pthread_mutex_init(&s->lock, NULL);

    for (uint16_t ch = 0; ch < info->channels; ch++) {
        s->map[ch] = NONE;
    }
    for (uint16_t i = 0; i < info->n; i++) {
        struct chan *c = &s->chans[i];

        c->ch = info->enabled[i];
        c->index = i;
        c->cur = STORE_UNKNOWN;
        s->map[c->ch] = i;
        if (span_add(s, c, 0, 0) < 0) {
            store_free(s);
            return -ENOMEM;
        }
    }
    for (uint8_t d = 0; d < info->data_channels; d++) {
        for (uint16_t i = info->block[d]; i < info->block[d + 1]; i++) {
            s->chans[i].dc = d;
        }
    }
    *out = s;

    return 0;
}


void store_free(struct store *s)
{
    if (!s) {
        return;
    }
    if (s->chans) {
        for (uint16_t i = 0; i < s->info->n; i++) {
            struct chan *c = &s->chans[i];

            for (size_t k = c->head; k < c->count; k++) {
                free(c->blocks[k]);
            }
            free(c->blocks);
            free(c->anchors);
            free(c->spans);
            free(c->ivals);
        }
    }
    if (s->data) {
        for (uint8_t d = 0; d < s->info->data_channels; d++) {
            free(s->data[d].ring.tick);
            free(s->data[d].ring.levels);
        }
    }
    for (size_t i = 0; i < s->loss_len; i++) {
        free(s->losses[i].lost);
    }
    if (s->map && s->chans && s->data) {
        pthread_mutex_destroy(&s->lock);
    }
    free(s->losses);
    free(s->map);
    free(s->chans);
    free(s->data);
    free(s);
}


const struct capture_info *store_info(const struct store *s)
{
    return s->info;
}


int store_write(struct store *s, const struct store_batch *b)
{
    int first = b->err;

    pthread_mutex_lock(&s->lock);
    for (size_t i = 0; i < b->len; i++) {
        const struct batch_entry *e = &b->e[i];
        int err = 0;

        if (e->type == BATCH_RECORD) {
            err = apply_record(s, e);
        } else if (e->type == BATCH_HEARTBEAT) {
            err = apply_heartbeat(s, b, e);
        } else {
            err = apply_loss(s, b, e);
        }
        if (err < 0 && first == 0) {
            first = err;
        }
        if (err == -ENOMEM) {
            break;
        }
    }
    enforce(s);
    pthread_mutex_unlock(&s->lock);

    return first;
}


void store_stats(struct store *s, struct store_stats *st)
{
    memset(st, 0, sizeof(*st));
    pthread_mutex_lock(&s->lock);
    st->oldest = s->oldest;
    st->newest = s->newest;
    st->have_watermark = s->info->data_channels > 0;
    st->watermark = UINT64_MAX;
    for (uint8_t d = 0; d < s->info->data_channels; d++) {
        if (!s->data[d].have_wm) {
            st->have_watermark = false;
        } else if (s->data[d].wm < st->watermark) {
            st->watermark = s->data[d].wm;
        }
    }
    if (!st->have_watermark) {
        st->watermark = 0;
    }
    for (uint16_t i = 0; i < s->info->n; i++) {
        st->records += held(&s->chans[i]);
        st->total += s->chans[i].next;
    }
    st->dropped = st->total - st->records;
    st->losses = s->loss_total;
    st->bytes = bytes_used(s);
    st->limit = s->limit;
    pthread_mutex_unlock(&s->lock);
}


int store_channel_stats(struct store *s, uint16_t channel, struct store_channel_stats *st)
{
    struct chan *c = chan_of(s, channel);

    if (!c) {
        return -EINVAL;
    }
    pthread_mutex_lock(&s->lock);
    st->first = c->first;
    st->next = c->next;
    st->oldest = c->dropped ? c->cover : 0;
    st->have_arrived = s->data[c->dc].have_wm;
    st->arrived = s->data[c->dc].wm;
    pthread_mutex_unlock(&s->lock);

    return 0;
}


int store_level(struct store *s, uint16_t channel, uint64_t tick)
{
    struct chan *c = chan_of(s, channel);
    int level = 0;

    if (!c) {
        return -EINVAL;
    }
    pthread_mutex_lock(&s->lock);
    if (tick < s->oldest || !s->data[c->dc].have_wm || tick > s->data[c->dc].wm) {
        level = -ERANGE;
    } else {
        level = level_at(c, tick);
    }
    pthread_mutex_unlock(&s->lock);

    return level;
}


int store_columns(struct store *s, const uint16_t *channels, size_t count,
                  const uint64_t *edges, size_t cols, struct store_column *out)
{
    for (size_t k = 0; k < count; k++) {
        if (!chan_of(s, channels[k])) {
            return -EINVAL;
        }
    }
    for (size_t x = 0; x < cols; x++) {
        if (edges[x + 1] < edges[x]) {
            return -EINVAL;
        }
    }

    pthread_mutex_lock(&s->lock);
    for (size_t k = 0; k < count; k++) {
        const struct chan *c = chan_of(s, channels[k]);
        const struct data *d = &s->data[c->dc];
        uint64_t a = edges[0];
        uint64_t ka = count_lt(c, a);
        uint8_t la = a > 0 && a - 1 >= s->oldest ? level_at(c, a - 1) : STORE_UNKNOWN;

        for (size_t x = 0; x < cols; x++) {
            struct store_column *col = &out[k * cols + x];
            uint64_t b = edges[x + 1];
            uint64_t kb = b > a ? count_lt(c, b) : ka;
            uint8_t lb = b > a ? level_k(c, b - 1, kb) : la;

            if (b > a && b - 1 < s->oldest) {
                lb = STORE_UNKNOWN;
            }
            col->start_level = la;
            col->end_level = lb;
            col->changes = (uint32_t)(kb - ka);
            col->flags = 0;
            if (b > a) {
                if (loss_in(c, a, b - 1)) {
                    col->flags |= STORE_COL_LOSS;
                }
                if (unknown_in(c, a, b - 1)) {
                    col->flags |= STORE_COL_UNKNOWN;
                }
                if (a < s->oldest || !d->have_wm || b - 1 > d->wm) {
                    col->flags |= STORE_COL_NO_DATA;
                }
            }
            a = b;
            ka = kb;
            la = lb;
        }
    }
    pthread_mutex_unlock(&s->lock);

    return 0;
}


int store_records(struct store *s, uint16_t channel, uint64_t from, uint64_t to,
                  struct record *out, size_t max, size_t *count, bool *more)
{
    struct chan *c = chan_of(s, channel);
    uint64_t k0 = 0;
    uint64_t k1 = 0;
    size_t n = 0;

    *count = 0;
    *more = false;
    if (!c) {
        return -EINVAL;
    }
    if (to < from) {
        return 0;
    }
    pthread_mutex_lock(&s->lock);
    k0 = count_lt(c, from);
    k1 = count_le(c, to);
    n = k1 - k0 < max ? (size_t)(k1 - k0) : max;
    for (size_t i = 0; i < n; i++) {
        uint64_t v = rec_at(c, k0 + i);

        out[i] = (struct record){.tick = v >> 1, .channel = c->ch, .level = v & 1};
    }
    *count = n;
    *more = k1 - k0 > n;
    pthread_mutex_unlock(&s->lock);

    return 0;
}


int store_next(struct store *s, const uint16_t *channels, size_t count, uint64_t tick,
               struct record *found)
{
    struct store_stats st;
    bool have = false;

    for (size_t k = 0; k < count; k++) {
        if (!chan_of(s, channels[k])) {
            return -EINVAL;
        }
    }
    store_stats(s, &st);
    if (!st.have_watermark) {
        return -ENOENT;
    }

    pthread_mutex_lock(&s->lock);
    for (size_t k = 0; k < count; k++) {
        const struct chan *c = chan_of(s, channels[k]);
        uint64_t i = count_le(c, tick);
        uint64_t v = 0;

        if (i >= held(c)) {
            continue;
        }
        v = rec_at(c, i);
        if ((v >> 1) > st.watermark) {
            continue;
        }
        if (!have || (v >> 1) < found->tick ||
            ((v >> 1) == found->tick && c->ch < found->channel)) {
            *found = (struct record){.tick = v >> 1, .channel = c->ch, .level = v & 1};
            have = true;
        }
    }
    pthread_mutex_unlock(&s->lock);

    return have ? 0 : -ENOENT;
}


int store_prev(struct store *s, const uint16_t *channels, size_t count, uint64_t tick,
               struct record *found)
{
    bool have = false;

    for (size_t k = 0; k < count; k++) {
        if (!chan_of(s, channels[k])) {
            return -EINVAL;
        }
    }

    pthread_mutex_lock(&s->lock);
    for (size_t k = 0; k < count; k++) {
        const struct chan *c = chan_of(s, channels[k]);
        uint64_t i = count_lt(c, tick);
        uint64_t v = 0;

        if (i == 0) {
            continue;
        }
        v = rec_at(c, i - 1);
        if (!have || (v >> 1) > found->tick ||
            ((v >> 1) == found->tick && c->ch < found->channel)) {
            *found = (struct record){.tick = v >> 1, .channel = c->ch, .level = v & 1};
            have = true;
        }
    }
    pthread_mutex_unlock(&s->lock);

    return have ? 0 : -ENOENT;
}


int store_read(struct store *s, uint16_t channel, uint64_t *pos, struct record *out,
               size_t max, size_t *count)
{
    struct chan *c = chan_of(s, channel);
    int err = 0;

    *count = 0;
    if (!c) {
        return -EINVAL;
    }
    pthread_mutex_lock(&s->lock);
    if (*pos < c->first || *pos > c->next) {
        err = -ERANGE;
    } else {
        size_t n = c->next - *pos < max ? (size_t)(c->next - *pos) : max;

        for (size_t i = 0; i < n; i++) {
            uint64_t v = rec_at(c, *pos - c->first + i);

            out[i] = (struct record){.tick = v >> 1, .channel = c->ch, .level = v & 1};
        }
        *pos += n;
        *count = n;
    }
    pthread_mutex_unlock(&s->lock);

    return err;
}


int store_read_merged(struct store *s, uint64_t *pos, struct record *out, size_t max,
                      size_t *count)
{
    struct store_stats st;
    size_t n = 0;
    int err = 0;

    *count = 0;
    store_stats(s, &st);

    pthread_mutex_lock(&s->lock);
    for (uint16_t i = 0; i < s->info->n; i++) {
        if (pos[i] < s->chans[i].first || pos[i] > s->chans[i].next) {
            err = -ERANGE;
        }
    }
    while (err == 0 && st.have_watermark && n < max) {
        const struct chan *best = NULL;
        uint64_t best_v = 0;

        for (uint16_t i = 0; i < s->info->n; i++) {
            const struct chan *c = &s->chans[i];
            uint64_t v = 0;

            if (pos[i] >= c->next) {
                continue;
            }
            v = rec_at(c, pos[i] - c->first);
            if ((v >> 1) <= st.watermark && (!best || (v >> 1) < (best_v >> 1))) {
                best = c;
                best_v = v;
            }
        }
        if (!best) {
            break;
        }
        out[n++] = (struct record){.tick = best_v >> 1, .channel = best->ch, .level = best_v & 1};
        pos[best->index]++;
    }
    *count = n;
    pthread_mutex_unlock(&s->lock);

    return err;
}


int store_losses(struct store *s, uint64_t from, uint64_t to, struct store_loss *out,
                 uint32_t *lost, size_t max, size_t *count)
{
    size_t n = 0;

    pthread_mutex_lock(&s->lock);
    for (size_t i = 0; i < s->loss_len && n < max; i++) {
        const struct lossrec *l = &s->losses[i];

        if (l->start > to) {
            break;
        }
        if (l->end < from) {
            continue;
        }
        out[n] = (struct store_loss){.start = l->start, .end = l->end, .lost = l->total};
        if (lost) {
            memcpy(&lost[n * s->info->n], l->lost, s->info->n * sizeof(*lost));
        }
        n++;
    }
    *count = n;
    pthread_mutex_unlock(&s->lock);

    return 0;
}


// A record: the channel's next change of level. It makes an unknown level known.
static int apply_record(struct store *s, const struct batch_entry *e)
{
    struct chan *c = chan_of(s, e->channel);
    struct span *last = NULL;
    int err = 0;

    if (!c || (c->have_last && e->tick <= c->last_tick) || e->tick > (UINT64_MAX >> 1)) {
        return -EINVAL;
    }
    if (c->count == c->head || c->last == STORE_BLOCK_RECORDS) {
        err = block_add(s, c);
        if (err < 0) {
            return err;
        }
    }
    c->blocks[c->count - 1][c->last++] = e->tick << 1 | (e->level & 1);
    c->next++;
    c->have_last = true;
    c->last_tick = e->tick;
    c->cur = e->level & 1;
    if (!s->have_newest || e->tick > s->newest) {
        s->newest = e->tick;
        s->have_newest = true;
    }

    last = c->span_len ? &c->spans[c->span_len - 1] : NULL;
    if (last && last->known == OPEN && e->tick >= last->from) {
        last->known = e->tick;
    }

    return 0;
}


/*
 * A heartbeat: the watermark of its data channel. It is kept for a channel whose level it
 * makes known, or whose level by the records it contradicts; it is held for late losses.
 */
static int apply_heartbeat(struct store *s, const struct store_batch *b,
                           const struct batch_entry *e)
{
    const struct capture_info *info = s->info;
    const uint8_t *levels = b->levels + e->off;
    struct data *d = NULL;

    if (e->channel >= info->data_channels) {
        return -EINVAL;
    }
    d = &s->data[e->channel];
    for (uint16_t i = info->block[e->channel]; i < info->block[e->channel + 1]; i++) {
        struct chan *c = &s->chans[i];
        struct span *last = c->span_len ? &c->spans[c->span_len - 1] : NULL;
        uint8_t level = (levels[i / 8] >> (i % 8)) & 1;
        bool keep = false;

        if (last && last->known == OPEN) {
            if (e->tick >= last->from) {
                last->known = e->tick;
                keep = true;
            }
        } else if (c->cur != level) {
            keep = true;
        }
        if (keep) {
            if (anchor_put(c, e->tick, level) < 0) {
                return -ENOMEM;
            }
            c->cur = level;
        }
    }
    if (!d->have_wm || e->tick > d->wm) {
        d->wm = e->tick;
        d->have_wm = true;
    }

    return ring_push(s, &d->ring, e->tick, levels);
}


// A loss: kept as reported, and on each channel that lost records, its level unknown.
static int apply_loss(struct store *s, const struct store_batch *b, const struct batch_entry *e)
{
    const uint32_t *lost = b->lost + e->off;
    struct lossrec l = {.start = e->tick, .end = e->end};
    size_t at = s->loss_len;

    if (e->end < e->tick) {
        return -EINVAL;
    }
    l.lost = malloc((s->info->n ? s->info->n : 1) * sizeof(*l.lost));
    if (!l.lost || grow((void **)&s->losses, &s->loss_cap, s->loss_len + 1,
                        sizeof(*s->losses)) < 0) {
        free(l.lost);
        return -ENOMEM;
    }
    for (uint16_t i = 0; i < s->info->n; i++) {
        l.lost[i] = lost[i];
        l.total += lost[i];
    }
    while (at > 0 && s->losses[at - 1].start > l.start) {
        at--;
    }
    memmove(&s->losses[at + 1], &s->losses[at], (s->loss_len - at) * sizeof(*s->losses));
    s->losses[at] = l;
    s->loss_len++;
    s->loss_total++;

    for (uint16_t i = 0; i < s->info->n; i++) {
        struct chan *c = &s->chans[i];

        if (lost[i] == 0) {
            continue;
        }
        if (ival_add(c, e->tick, e->end) < 0 || span_add(s, c, e->tick, e->end + 1) < 0) {
            return -ENOMEM;
        }
    }

    return 0;
}


// A new block for the channel to write to, dropping the oldest ones past the limit.
static int block_add(struct store *s, struct chan *c)
{
    uint64_t *blk = malloc(BLOCK_BYTES);

    if (!blk) {
        return -ENOMEM;
    }
    if (c->count == c->cap && c->head > 0) {
        memmove(c->blocks, c->blocks + c->head, (c->count - c->head) * sizeof(*c->blocks));
        c->count -= c->head;
        c->head = 0;
    }
    if (grow((void **)&c->blocks, &c->cap, c->count + 1, sizeof(*c->blocks)) < 0) {
        free(blk);
        return -ENOMEM;
    }
    c->blocks[c->count++] = blk;
    c->last = 0;
    s->blocks++;
    enforce(s);

    return 0;
}


/*
 * Keeps the store within its limit: drops the oldest full blocks, and with none left to
 * drop, the older half of the kept heartbeats or losses of the channel that has the most.
 */
static void enforce(struct store *s)
{
    while (bytes_used(s) > s->limit && (drop_oldest(s) || trim_side(s))) {
    }
}


// Drops the full block whose newest record is the oldest in the store; false if none.
static bool drop_oldest(struct store *s)
{
    struct chan *best = NULL;
    uint64_t best_tick = 0;

    for (uint16_t i = 0; i < s->info->n; i++) {
        struct chan *c = &s->chans[i];
        uint64_t t = 0;

        if (c->count - c->head < 2) {
            continue;
        }
        t = c->blocks[c->head][STORE_BLOCK_RECORDS - 1] >> 1;
        if (!best || t < best_tick) {
            best = c;
            best_tick = t;
        }
    }
    if (!best) {
        return false;
    }
    trim_to(s, best, best_tick);
    free(best->blocks[best->head]);
    best->head++;
    best->first += STORE_BLOCK_RECORDS;
    s->blocks--;

    return true;
}


// The older half of the anchors (or loss spans) of the channel with the most; false if none.
static bool trim_side(struct store *s)
{
    struct chan *best = NULL;
    size_t most = 1;
    size_t before = 0;

    for (uint16_t i = 0; i < s->info->n; i++) {
        struct chan *c = &s->chans[i];
        size_t n = c->anchor_len > c->ival_len ? c->anchor_len : c->ival_len;

        if (n > most) {
            best = c;
            most = n;
        }
    }
    if (!best) {
        return false;
    }
    before = best->anchor_len + best->ival_len;
    if (best->anchor_len >= best->ival_len) {
        trim_to(s, best, best->anchors[best->anchor_len / 2].tick);
    } else {
        trim_to(s, best, best->ivals[best->ival_len / 2].end);
    }

    return best->anchor_len + best->ival_len < before;
}


/*
 * Lets go of the channel's levels before tick t: the level at the end of t becomes its
 * base, and what only told about older ticks goes. The store's oldest tick follows.
 */
static void trim_to(struct store *s, struct chan *c, uint64_t t)
{
    size_t keep = 0;

    if (!c->dropped || t > c->cover) {
        c->base = level_at(c, t);
        c->dropped = true;
        c->cover = t;
    }
    if (t > s->oldest) {
        s->oldest = t;
    }

    while (keep < c->anchor_len && c->anchors[keep].tick <= t) {
        keep++;
    }
    drop_front((void **)&c->anchors, &c->anchor_len, &c->anchor_cap, keep, sizeof(*c->anchors));
    keep = 0;
    while (keep < c->span_len && c->spans[keep].known <= t) {
        keep++;
    }
    drop_front((void **)&c->spans, &c->span_len, &c->span_cap, keep, sizeof(*c->spans));
    keep = 0;
    while (keep < c->ival_len && c->ivals[keep].end < t) {
        keep++;
    }
    drop_front((void **)&c->ivals, &c->ival_len, &c->ival_cap, keep, sizeof(*c->ivals));
    keep = 0;
    while (keep < s->loss_len && s->losses[keep].end < s->oldest) {
        free(s->losses[keep].lost);
        keep++;
    }
    drop_front((void **)&s->losses, &s->loss_len, &s->loss_cap, keep, sizeof(*s->losses));
}


// The memory the store takes: the record blocks and everything kept beside them.
static size_t bytes_used(const struct store *s)
{
    size_t n = s->blocks * BLOCK_BYTES + s->loss_cap * sizeof(*s->losses) +
               s->loss_len * s->info->n * sizeof(uint32_t);

    for (uint16_t i = 0; i < s->info->n; i++) {
        const struct chan *c = &s->chans[i];

        n += c->cap * sizeof(*c->blocks) + c->anchor_cap * sizeof(*c->anchors) +
             c->span_cap * sizeof(*c->spans) + c->ival_cap * sizeof(*c->ivals);
    }
    for (uint8_t d = 0; d < s->info->data_channels; d++) {
        n += s->data[d].ring.cap * (sizeof(uint64_t) + s->level_bytes);
    }

    return n;
}


/*
 * Makes the level unknown from start on, until the first record or heartbeat at or after
 * from, merged with the spans it meets. A heartbeat held for late losses that ends the
 * span is kept as an anchor.
 */
static int span_add(struct store *s, struct chan *c, uint64_t start, uint64_t from)
{
    struct span n = {.start = start, .from = from};
    size_t p = c->span_len;
    bool beat = false;
    uint8_t level = 0;

    if (grow((void **)&c->spans, &c->span_cap, c->span_len + 1, sizeof(*c->spans)) < 0) {
        return -ENOMEM;
    }
    n.known = span_resolve(s, c, from, &beat, &level);
    while (p > 0 && c->spans[p - 1].start > start) {
        p--;
    }
    memmove(&c->spans[p + 1], &c->spans[p], (c->span_len - p) * sizeof(*c->spans));
    c->spans[p] = n;
    c->span_len++;

    while (p > 0 && c->spans[p - 1].known >= c->spans[p].start) {
        struct span *a = &c->spans[p - 1];

        a->from = a->from > c->spans[p].from ? a->from : c->spans[p].from;
        a->known = span_resolve(s, c, a->from, &beat, &level);
        memmove(&c->spans[p], &c->spans[p + 1], (c->span_len - p - 1) * sizeof(*c->spans));
        c->span_len--;
        p--;
    }
    while (p + 1 < c->span_len && c->spans[p + 1].start <= c->spans[p].known) {
        struct span *a = &c->spans[p];

        a->from = a->from > c->spans[p + 1].from ? a->from : c->spans[p + 1].from;
        a->known = span_resolve(s, c, a->from, &beat, &level);
        memmove(&c->spans[p + 1], &c->spans[p + 2], (c->span_len - p - 2) * sizeof(*c->spans));
        c->span_len--;
    }

    if (c->spans[p].known == OPEN) {
        c->cur = STORE_UNKNOWN;
    } else if (beat) {
        return anchor_put(c, c->spans[p].known, level);
    }

    return 0;
}


/*
 * The first tick at or after from that a record, an anchor or a held heartbeat gives the
 * level at, or OPEN. *beat says whether it is a held heartbeat, with its level in *level.
 */
static uint64_t span_resolve(struct store *s, struct chan *c, uint64_t from, bool *beat,
                             uint8_t *level)
{
    const struct ring *r = &s->data[c->dc].ring;
    uint64_t known = OPEN;
    uint64_t k = count_lt(c, from);
    size_t lo = 0;
    size_t hi = c->anchor_len;

    *beat = false;
    if (k < held(c)) {
        known = rec_at(c, k) >> 1;
    }
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;

        if (c->anchors[mid].tick < from) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < c->anchor_len && c->anchors[lo].tick < known) {
        known = c->anchors[lo].tick;
    }

    lo = 0;
    hi = r->len;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;

        if (r->tick[(r->head + mid) % r->cap] < from) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < r->len) {
        size_t at = (r->head + lo) % r->cap;

        if (r->tick[at] < known) {
            known = r->tick[at];
            *beat = true;
            *level = (r->levels[at * s->level_bytes + c->index / 8] >> (c->index % 8)) & 1;
        }
    }

    return known;
}


// A loss's span on the channel, merged with the ones it meets.
static int ival_add(struct chan *c, uint64_t start, uint64_t end)
{
    size_t p = c->ival_len;

    if (grow((void **)&c->ivals, &c->ival_cap, c->ival_len + 1, sizeof(*c->ivals)) < 0) {
        return -ENOMEM;
    }
    while (p > 0 && c->ivals[p - 1].start > start) {
        p--;
    }
    memmove(&c->ivals[p + 1], &c->ivals[p], (c->ival_len - p) * sizeof(*c->ivals));
    c->ivals[p] = (struct ival){.start = start, .end = end};
    c->ival_len++;

    if (p > 0 && c->ivals[p - 1].end + 1 >= start) {
        p--;
        if (c->ivals[p + 1].end > c->ivals[p].end) {
            c->ivals[p].end = c->ivals[p + 1].end;
        }
        memmove(&c->ivals[p + 1], &c->ivals[p + 2], (c->ival_len - p - 2) * sizeof(*c->ivals));
        c->ival_len--;
    }
    while (p + 1 < c->ival_len && c->ivals[p + 1].start <= c->ivals[p].end + 1) {
        if (c->ivals[p + 1].end > c->ivals[p].end) {
            c->ivals[p].end = c->ivals[p + 1].end;
        }
        memmove(&c->ivals[p + 1], &c->ivals[p + 2], (c->ival_len - p - 2) * sizeof(*c->ivals));
        c->ival_len--;
    }

    return 0;
}


// An anchor in tick order; one already at that tick is replaced.
static int anchor_put(struct chan *c, uint64_t tick, uint8_t level)
{
    size_t p = c->anchor_len;

    while (p > 0 && c->anchors[p - 1].tick > tick) {
        p--;
    }
    if (p > 0 && c->anchors[p - 1].tick == tick) {
        c->anchors[p - 1].level = level;
        return 0;
    }
    if (grow((void **)&c->anchors, &c->anchor_cap, c->anchor_len + 1, sizeof(*c->anchors)) < 0) {
        return -ENOMEM;
    }
    memmove(&c->anchors[p + 1], &c->anchors[p], (c->anchor_len - p) * sizeof(*c->anchors));
    c->anchors[p] = (struct anchor){.tick = tick, .level = level};
    c->anchor_len++;

    return 0;
}


// Holds a heartbeat for late losses, and lets go of those older than they can come for.
static int ring_push(struct store *s, struct ring *r, uint64_t tick, const uint8_t *levels)
{
    size_t lb = s->level_bytes;
    size_t at = 0;

    while (r->len > 0 && (s->late ? r->tick[r->head] + s->late < tick : r->len >= LATE_BEATS)) {
        r->head = (r->head + 1) % r->cap;
        r->len--;
    }
    if (r->len == r->cap) {
        size_t cap = r->cap ? r->cap * 2 : 64;
        uint64_t *t = malloc(cap * sizeof(*t));
        uint8_t *l = malloc(cap * (lb ? lb : 1));

        if (!t || !l) {
            free(t);
            free(l);
            return -ENOMEM;
        }
        for (size_t i = 0; i < r->len; i++) {
            size_t from = (r->head + i) % r->cap;

            t[i] = r->tick[from];
            memcpy(l + i * lb, r->levels + from * lb, lb);
        }
        free(r->tick);
        free(r->levels);
        r->tick = t;
        r->levels = l;
        r->head = 0;
        r->cap = cap;
    }
    at = (r->head + r->len) % r->cap;
    r->tick[at] = tick;
    memcpy(r->levels + at * lb, levels, lb);
    r->len++;

    return 0;
}


static struct chan *chan_of(const struct store *s, uint16_t channel)
{
    if (channel >= s->info->channels || s->map[channel] == NONE) {
        return NULL;
    }

    return &s->chans[s->map[channel]];
}


// The channel's k-th record held, packed.
static uint64_t rec_at(const struct chan *c, uint64_t k)
{
    return c->blocks[c->head + (k >> BLOCK_SHIFT)][k & BLOCK_MASK];
}


static uint64_t held(const struct chan *c)
{
    return c->next - c->first;
}


// How many of the channel's records held come before tick.
static uint64_t count_lt(const struct chan *c, uint64_t tick)
{
    uint64_t lo = 0;
    uint64_t hi = held(c);

    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;

        if ((rec_at(c, mid) >> 1) < tick) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    return lo;
}


static uint64_t count_le(const struct chan *c, uint64_t tick)
{
    return tick == UINT64_MAX ? held(c) : count_lt(c, tick + 1);
}


/*
 * The level at the end of tick, given k, the records held up to and including it: the
 * newest of its last record and its last anchor there (an anchor after the records of its
 * tick), or the base for what is older than the cover, unless the level is unknown there.
 */
static uint8_t level_k(const struct chan *c, uint64_t tick, uint64_t k)
{
    size_t lo = 0;
    size_t hi = c->span_len;
    const struct anchor *an = NULL;

    if (c->dropped && tick < c->cover) {
        return STORE_UNKNOWN;
    }
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;

        if (c->spans[mid].start <= tick) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo > 0 && tick < c->spans[lo - 1].known) {
        return STORE_UNKNOWN;
    }

    lo = 0;
    hi = c->anchor_len;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;

        if (c->anchors[mid].tick <= tick) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    an = lo > 0 ? &c->anchors[lo - 1] : NULL;

    // Records and anchors before the cover were let go of: the base stands for them.
    if (k > 0) {
        uint64_t v = rec_at(c, k - 1);

        if ((!an || an->tick < (v >> 1)) && (!c->dropped || (v >> 1) >= c->cover)) {
            return v & 1;
        }
    }
    if (an && (!c->dropped || an->tick >= c->cover)) {
        return an->level;
    }

    return c->dropped ? c->base : STORE_UNKNOWN;
}


static uint8_t level_at(const struct chan *c, uint64_t tick)
{
    return level_k(c, tick, count_le(c, tick));
}


// Whether the level is unknown at some tick from a to b (inclusive).
static bool unknown_in(const struct chan *c, uint64_t a, uint64_t b)
{
    size_t lo = 0;
    size_t hi = c->span_len;

    if (c->dropped && a < c->cover) {
        return true;
    }
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;

        if (c->spans[mid].start <= b) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    return lo > 0 && c->spans[lo - 1].known > a;
}


// Whether a loss on the channel overlaps ticks a to b (inclusive).
static bool loss_in(const struct chan *c, uint64_t a, uint64_t b)
{
    size_t lo = 0;
    size_t hi = c->ival_len;

    while (lo < hi) {
        size_t mid = (lo + hi) / 2;

        if (c->ivals[mid].start <= b) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    return lo > 0 && c->ivals[lo - 1].end >= a;
}


/*
 * Removes the first keep of the *len elements of size bytes at *p, and gives back memory
 * once less than a quarter of *cap is used.
 */
static void drop_front(void **p, size_t *len, size_t *cap, size_t keep, size_t size)
{
    void *q = NULL;

    if (keep == 0) {
        return;
    }
    memmove(*p, (uint8_t *)*p + keep * size, (*len - keep) * size);
    *len -= keep;
    if (*cap > 64 && *len < *cap / 4) {
        q = realloc(*p, *cap / 2 * size);
        if (q) {
            *p = q;
            *cap /= 2;
        }
    }
}


// Makes room for need elements of size bytes in *p, doubling.
static int grow(void **p, size_t *cap, size_t need, size_t size)
{
    size_t n = *cap ? *cap : 16;
    void *q = NULL;

    if (need <= *cap) {
        return 0;
    }
    while (n < need) {
        n *= 2;
    }
    q = realloc(*p, n * size);
    if (!q) {
        return -ENOMEM;
    }
    *p = q;
    *cap = n;

    return 0;
}
