/*
 * Store batches (store/store.h): what one capture pull hands over, copied in order.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "common/capture_info.h"
#include "common/record.h"
#include "ui/store/batch.h"
#include "ui/store/store.h"


static struct batch_entry *entry_add(struct store_batch *b);
static int grow(void **p, size_t *cap, size_t need, size_t size);


int store_batch_new(struct store_batch **out, const struct capture_info *info)
{
    struct store_batch *b = calloc(1, sizeof(*b));

    if (!b) {
        return -ENOMEM;
    }
    b->n = info->n;
    b->level_bytes = (info->n + 7) / 8;
    *out = b;

    return 0;
}


void store_batch_free(struct store_batch *b)
{
    if (!b) {
        return;
    }
    free(b->e);
    free(b->levels);
    free(b->lost);
    free(b);
}


void store_batch_record(struct store_batch *b, const struct record *r)
{
    struct batch_entry *e = entry_add(b);

    if (!e) {
        return;
    }
    e->type = BATCH_RECORD;
    e->tick = r->tick;
    e->channel = r->channel;
    e->level = r->level;
}


void store_batch_heartbeat(struct store_batch *b, const struct heartbeat *hb)
{
    struct batch_entry *e = NULL;

    if (b->err == 0 && grow((void **)&b->levels, &b->levels_cap,
                            b->levels_len + b->level_bytes, 1) < 0) {
        b->err = -ENOMEM;
    }
    e = entry_add(b);
    if (!e) {
        return;
    }
    e->type = BATCH_HEARTBEAT;
    e->tick = hb->tick;
    e->channel = hb->data_channel;
    e->off = b->levels_len;
    memcpy(b->levels + b->levels_len, hb->levels, b->level_bytes);
    b->levels_len += b->level_bytes;
}


void store_batch_loss(struct store_batch *b, const struct loss *loss)
{
    struct batch_entry *e = NULL;

    if (b->err == 0 && grow((void **)&b->lost, &b->lost_cap, b->lost_len + b->n,
                            sizeof(*b->lost)) < 0) {
        b->err = -ENOMEM;
    }
    e = entry_add(b);
    if (!e) {
        return;
    }
    e->type = BATCH_LOSS;
    e->tick = loss->start;
    e->end = loss->end;
    e->off = b->lost_len;
    for (uint16_t i = 0; i < b->n; i++) {
        b->lost[b->lost_len + i] = i < loss->n ? loss->lost[i] : 0;
    }
    b->lost_len += b->n;
}


void store_batch_clear(struct store_batch *b)
{
    b->len = 0;
    b->levels_len = 0;
    b->lost_len = 0;
    b->err = 0;
}


// A new entry at the end, or NULL once the batch has failed.
static struct batch_entry *entry_add(struct store_batch *b)
{
    struct batch_entry *e = NULL;

    if (b->err == 0 && grow((void **)&b->e, &b->cap, b->len + 1, sizeof(*b->e)) < 0) {
        b->err = -ENOMEM;
    }
    if (b->err < 0) {
        return NULL;
    }
    e = &b->e[b->len++];
    memset(e, 0, sizeof(*e));

    return e;
}


// Makes room for need elements of size bytes in *p, doubling.
static int grow(void **p, size_t *cap, size_t need, size_t size)
{
    size_t n = *cap ? *cap : 256;
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
