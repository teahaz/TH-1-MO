/*
 * Merges a capture's data channels into one stream in tick order. Each data channel's
 * records arrive in tick order, but the channels arrive independently; a data channel's
 * heartbeat says every record of its up to that tick has arrived. The merge holds records
 * back until every data channel has passed their tick, then hands them over with the
 * heartbeats' levels, ordered by tick, records before levels, then by channel.
 */

#ifndef CODEC_MERGE_H
#define CODEC_MERGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common/capture_info.h"
#include "common/record.h"


/*
 * record: a record. level: a heartbeat's level for one channel, at the end of tick.
 * advance: everything up to and including tick has been handed over.
 */
struct merge_handler {
    void (*record)(void *ctx, const struct record *r);
    void (*level)(void *ctx, uint64_t tick, uint16_t channel, uint8_t level);
    void (*advance)(void *ctx, uint64_t tick);
    void *ctx;
};

struct merge_entry {
    uint64_t tick;
    uint16_t channel;
    uint8_t level;
    bool is_level;
};

// One data channel's entries not yet handed over, oldest at head.
struct merge_queue {
    struct merge_entry *e;
    size_t head;
    size_t len;
    size_t cap;
    bool have_wm;
    uint64_t wm;
    bool empty_block;
};

struct merge {
    const struct capture_info *info;
    struct merge_queue *q;
    uint16_t *data_channel;
    struct merge_handler handler;
    bool have_released;
    uint64_t released;
};


int merge_init(struct merge *m, const struct capture_info *info,
               const struct merge_handler *handler);
void merge_free(struct merge *m);
int merge_record(struct merge *m, const struct record *r);
int merge_heartbeat(struct merge *m, const struct heartbeat *hb);
int merge_level(struct merge *m, uint64_t tick, uint16_t channel, uint8_t level);
int merge_advance(struct merge *m, uint64_t tick);
void merge_flush(struct merge *m);

#endif
