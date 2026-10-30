/*
 * The inside of a store batch (store/store.h), for the store to read. Entries keep the
 * order they were handed over in; a heartbeat's levels and a loss's counts are in pools,
 * at their entry's offset.
 */

#ifndef UI_STORE_BATCH_H
#define UI_STORE_BATCH_H

#include <stddef.h>
#include <stdint.h>


enum batch_type {
    BATCH_RECORD,
    BATCH_HEARTBEAT,
    BATCH_LOSS,
};

/*
 * A record: tick, channel, level. A heartbeat: tick, its data channel in channel, its
 * levels at off in the level pool. A loss: start in tick, end, its counts at off in the
 * lost pool.
 */
struct batch_entry {
    uint64_t tick;
    uint64_t end;
    size_t off;
    uint16_t channel;
    uint8_t level;
    uint8_t type;
};

struct store_batch {
    uint16_t n;
    size_t level_bytes;

    struct batch_entry *e;
    size_t len;
    size_t cap;

    uint8_t *levels;
    size_t levels_len;
    size_t levels_cap;

    uint32_t *lost;
    size_t lost_len;
    size_t lost_cap;

    int err;
};

#endif
