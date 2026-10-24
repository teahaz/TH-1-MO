/*
 * Loss reports into losses (protocol/spec.tex, Loss, Host behaviour): the lost-records
 * counters as differences from the last report, and the range to mark, widened back to the
 * last report received when a report was skipped.
 */

#ifndef CAPTURE_LOSS_H
#define CAPTURE_LOSS_H

#include <stdbool.h>
#include <stdint.h>

#include "capture/control.h"
#include "common/record.h"


/*
 * last holds each logical channel's counter as last seen, lost the differences of the
 * report taken last (the struct loss points at it). prev_end is the last report's loss end
 * as a tick.
 */
struct loss_tracker {
    uint16_t n;
    uint32_t *last;
    uint32_t *lost;
    bool have_prev;
    uint32_t next_seq;
    uint64_t prev_end;
    uint64_t skipped;
};


int loss_tracker_init(struct loss_tracker *lt, uint16_t n);
void loss_tracker_free(struct loss_tracker *lt);
void loss_tracker_base(struct loss_tracker *lt, const struct loss_report *rep, uint64_t end);
void loss_tracker_take(struct loss_tracker *lt, const struct loss_report *rep, uint64_t start,
                       uint64_t end, struct loss *out);

#endif
