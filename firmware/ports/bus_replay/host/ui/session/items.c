/*
 * The item cache (ui/session/items.h). Per group, its top-level items in release order,
 * which is order of start, from head on; span is the longest item's ticks, which bounds
 * how far back an item overlapping a span can start.
 */

#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "codec/item.h"
#include "ui/session/items.h"


struct group {
    uint32_t id;
    struct codec_item **it;
    size_t head;
    size_t len;
    size_t cap;
    uint64_t span;
    uint64_t dropped;
    uint64_t released;
};

struct items {
    pthread_mutex_t lock;
    struct group *g;
    size_t count;
};


static struct group *group_of(struct items *c, uint32_t group, bool add);
static void drop_front(struct group *g, size_t n);
static size_t first_from(const struct group *g, uint64_t tick);


int items_new(struct items **out)
{
    struct items *c = calloc(1, sizeof(*c));

    if (!c) {
        return -ENOMEM;
    }
    pthread_mutex_init(&c->lock, NULL);
    *out = c;

    return 0;
}


void items_free(struct items *c)
{
    if (!c) {
        return;
    }
    for (size_t i = 0; i < c->count; i++) {
        drop_front(&c->g[i], c->g[i].len);
        free(c->g[i].it);
    }
    free(c->g);
    pthread_mutex_destroy(&c->lock);
    free(c);
}


int items_add(struct items *c, uint32_t group, struct codec_item *item)
{
    struct group *g = NULL;
    int err = 0;

    pthread_mutex_lock(&c->lock);
    g = group_of(c, group, true);
    if (!g) {
        err = -ENOMEM;
    } else {
        if (g->head + g->len == g->cap) {
            if (g->head > 0) {
                memmove(g->it, g->it + g->head, g->len * sizeof(*g->it));
                g->head = 0;
            } else {
                size_t cap = g->cap ? 2 * g->cap : 256;
                struct codec_item **bigger = realloc(g->it, cap * sizeof(*bigger));

                if (!bigger) {
                    err = -ENOMEM;
                } else {
                    g->it = bigger;
                    g->cap = cap;
                }
            }
        }
        if (err == 0) {
            g->it[g->head + g->len++] = item;
            if (item->end - item->start > g->span) {
                g->span = item->end - item->start;
            }
            if (item->end > g->released) {
                g->released = item->end;
            }
            if (g->len > ITEMS_GROUP_MAX) {
                drop_front(g, g->len - ITEMS_GROUP_MAX);
            }
        }
    }
    pthread_mutex_unlock(&c->lock);
    if (err < 0) {
        codec_item_free(item);
    }

    return err;
}


void items_clear(struct items *c, uint32_t group)
{
    struct group *g = NULL;

    pthread_mutex_lock(&c->lock);
    g = group_of(c, group, false);
    if (g) {
        drop_front(g, g->len);
        g->head = 0;
        g->span = 0;
        g->released = 0;
    }
    pthread_mutex_unlock(&c->lock);
}


void items_trim(struct items *c, uint64_t tick)
{
    pthread_mutex_lock(&c->lock);
    for (size_t i = 0; i < c->count; i++) {
        struct group *g = &c->g[i];
        size_t n = 0;

        while (n < g->len && g->it[g->head + n]->end < tick) {
            n++;
        }
        drop_front(g, n);
    }
    pthread_mutex_unlock(&c->lock);
}


int items_span(struct items *c, uint32_t group, uint64_t from, uint64_t to, size_t max,
               items_fn fn, void *ctx)
{
    struct group *g = NULL;
    int n = 0;

    pthread_mutex_lock(&c->lock);
    g = group_of(c, group, false);
    if (!g) {
        n = -ENOENT;
    } else {
        for (size_t i = first_from(g, from > g->span ? from - g->span : 0);
             i < g->len && (size_t)n < max; i++) {
            const struct codec_item *it = g->it[g->head + i];

            if (it->start > to) {
                break;
            }
            if (it->end >= from) {
                fn(ctx, it);
                n++;
            }
        }
    }
    pthread_mutex_unlock(&c->lock);

    return n;
}


int items_last(struct items *c, uint32_t group, size_t n, items_fn fn, void *ctx)
{
    struct group *g = NULL;
    int count = 0;

    pthread_mutex_lock(&c->lock);
    g = group_of(c, group, false);
    if (!g) {
        count = -ENOENT;
    } else {
        for (size_t i = g->len > n ? g->len - n : 0; i < g->len; i++) {
            fn(ctx, g->it[g->head + i]);
            count++;
        }
    }
    pthread_mutex_unlock(&c->lock);

    return count;
}


int items_find(struct items *c, uint32_t group, uint64_t id, items_fn fn, void *ctx)
{
    struct group *g = NULL;
    int count = 0;

    pthread_mutex_lock(&c->lock);
    g = group_of(c, group, false);
    if (!g) {
        count = -ENOENT;
    } else {
        // Ids grow with release order: a binary search.
        size_t lo = 0;
        size_t hi = g->len;

        while (lo < hi) {
            size_t mid = (lo + hi) / 2;

            if (g->it[g->head + mid]->id < id) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if (lo < g->len && g->it[g->head + lo]->id == id) {
            fn(ctx, g->it[g->head + lo]);
            count = 1;
        }
    }
    pthread_mutex_unlock(&c->lock);

    return count;
}


int items_stats(struct items *c, uint32_t group, struct items_stats *st)
{
    struct group *g = NULL;

    memset(st, 0, sizeof(*st));
    pthread_mutex_lock(&c->lock);
    g = group_of(c, group, false);
    if (g) {
        st->count = g->len;
        st->dropped = g->dropped;
        st->released = g->released;
    }
    pthread_mutex_unlock(&c->lock);

    return g ? 0 : -ENOENT;
}


static struct group *group_of(struct items *c, uint32_t group, bool add)
{
    struct group *bigger = NULL;

    for (size_t i = 0; i < c->count; i++) {
        if (c->g[i].id == group) {
            return &c->g[i];
        }
    }
    if (!add) {
        return NULL;
    }
    bigger = realloc(c->g, (c->count + 1) * sizeof(*bigger));
    if (!bigger) {
        return NULL;
    }
    c->g = bigger;
    memset(&c->g[c->count], 0, sizeof(*c->g));
    c->g[c->count].id = group;

    return &c->g[c->count++];
}


static void drop_front(struct group *g, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        codec_item_free(g->it[g->head + i]);
    }
    g->head += n;
    g->len -= n;
    g->dropped += n;
    if (g->len == 0) {
        g->head = 0;
    }
}


// The first of the group's items that starts at or after tick.
static size_t first_from(const struct group *g, uint64_t tick)
{
    size_t lo = 0;
    size_t hi = g->len;

    while (lo < hi) {
        size_t mid = (lo + hi) / 2;

        if (g->it[g->head + mid]->start < tick) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    return lo;
}
