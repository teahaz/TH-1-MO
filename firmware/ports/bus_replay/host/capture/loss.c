#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "capture/control.h"
#include "capture/loss.h"
#include "common/record.h"


// Counters start from 0 and the sequence from 0, as after a reset.
int loss_tracker_init(struct loss_tracker *lt, uint16_t n)
{
    memset(lt, 0, sizeof(*lt));
    lt->n = n;
    lt->last = calloc(n ? n : 1, sizeof(*lt->last));
    lt->lost = calloc(n ? n : 1, sizeof(*lt->lost));
    if (!lt->last || !lt->lost) {
        loss_tracker_free(lt);
        return -ENOMEM;
    }

    return 0;
}


void loss_tracker_free(struct loss_tracker *lt)
{
    free(lt->last);
    free(lt->lost);
    memset(lt, 0, sizeof(*lt));
}


/*
 * For a recording that starts during a capture: rep is the last report received before it,
 * end its loss end as a tick. Later reports are read against it.
 */
void loss_tracker_base(struct loss_tracker *lt, const struct loss_report *rep, uint64_t end)
{
    for (unsigned int i = 0; i < lt->n && i < rep->n; i++) {
        lt->last[i] = loss_report_lost(rep, i);
    }
    lt->have_prev = true;
    lt->next_seq = rep->seq + 1;
    lt->prev_end = end;
}


/*
 * Takes a report whose loss start and end are the ticks start and end. out gets the range
 * to mark and the records each logical channel lost; a channel with 0 is not marked. After
 * a skipped report the range starts at the last report's loss end, or at the capture's
 * start if there was none.
 */
void loss_tracker_take(struct loss_tracker *lt, const struct loss_report *rep, uint64_t start,
                       uint64_t end, struct loss *out)
{
    if (rep->seq != lt->next_seq) {
        lt->skipped++;
        start = lt->have_prev ? lt->prev_end : 0;
    }
    for (unsigned int i = 0; i < lt->n; i++) {
        uint32_t now = i < rep->n ? loss_report_lost(rep, i) : lt->last[i];

        // Unsigned difference: a counter that wrapped reads correctly.
        lt->lost[i] = now - lt->last[i];
        lt->last[i] = now;
    }
    lt->have_prev = true;
    lt->next_seq = rep->seq + 1;
    lt->prev_end = end;

    out->start = start;
    out->end = end;
    out->n = lt->n;
    out->lost = lt->lost;
}
