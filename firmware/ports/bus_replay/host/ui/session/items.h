/*
 * The decoded items of one capture, as the UI reads them: per decoder group (a protocol
 * decoder and the decoders stacked on it, ui/session/session.h), its top-level items in
 * order of their start, each with its tree (codec/item.h). It is a cache over the store:
 * filled by the session thread as items are released, and trimmed with the store, so an
 * item never outlives the records it points at.
 *
 * One writer, any number of readers, with one mutex. A reader's callback runs under it, so
 * it must not wait, keep the item, or call into the session; it copies what it needs (the
 * Lua binding builds its tables there). An item's dec is not to be used: its group's codec
 * session may have gone (a group decoded again keeps no items, but a view's items outlive
 * the groups that made them). The kind names an item; the group's label its decoder.
 */

#ifndef UI_SESSION_ITEMS_H
#define UI_SESSION_ITEMS_H

#include <stddef.h>
#include <stdint.h>

#include "codec/item.h"


// A group's top-level items past this many are dropped, oldest first.
#define ITEMS_GROUP_MAX 200000

typedef void (*items_fn)(void *ctx, const struct codec_item *item);

// count items held, and the newest released item's end: everything before it is final.
struct items_stats {
    size_t count;
    uint64_t dropped;
    uint64_t released;
};

struct items;


int items_new(struct items **out);
void items_free(struct items *c);

/*
 * Writing, for the session thread. items_add() takes the item over. items_clear() drops a
 * group's items (the group is decoded again from the start, or removed). items_trim() drops
 * every item that ends before tick, with the store's oldest tick.
 */
int items_add(struct items *c, uint32_t group, struct codec_item *item);
void items_clear(struct items *c, uint32_t group);
void items_trim(struct items *c, uint64_t tick);

/*
 * Reading: the group's top-level items overlapping ticks from to to (inclusive), oldest
 * first, at most max; its newest n; the one with id. Each is handed to fn. They return the
 * number handed over, or -ENOENT for a group with no items yet.
 */
int items_span(struct items *c, uint32_t group, uint64_t from, uint64_t to, size_t max,
               items_fn fn, void *ctx);
int items_last(struct items *c, uint32_t group, size_t n, items_fn fn, void *ctx);
int items_find(struct items *c, uint32_t group, uint64_t id, items_fn fn, void *ctx);
int items_stats(struct items *c, uint32_t group, struct items_stats *st);

#endif
