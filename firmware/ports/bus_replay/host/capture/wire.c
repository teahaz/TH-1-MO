#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "capture/control.h"
#include "capture/wire.h"
#include "common/capture_info.h"
#include "common/record.h"


#define CRC8_INIT 0xFF
#define CRC8_POLY 0x07

// After a resync, a heartbeat is taken only if its grid index is this close after the last.
#define RESYNC_STEP_MAX 16


static size_t sync_at(struct wire *w, size_t i);
static void heartbeat(struct wire *w, const uint8_t *hb);
static void record(struct wire *w, uint64_t value, const uint8_t *raw);
static bool covered(const struct wire *w, const struct wire_span *span);
static void mismatch(struct wire *w, uint64_t start, uint64_t end);
static int span_push(struct wire_span **list, size_t *count, size_t *cap,
                     const struct wire_span *span);
static int emit(struct wire_writer *wr, const uint8_t *data, size_t len);


/*
 * Sets w up to parse data channel data_channel of the capture in info. info must outlive
 * w. Returns -EINVAL for a data channel the capture does not have, -ENOMEM if out of
 * memory.
 */
int wire_init(struct wire *w, const struct capture_info *info, uint16_t data_channel,
              const struct wire_handler *handler)
{
    memset(w, 0, sizeof(*w));
    if (data_channel >= info->data_channels) {
        return -EINVAL;
    }

    w->c = info->c;
    w->data_channel = data_channel;
    w->n = info->n;
    w->first = info->block[data_channel];
    w->count = (uint16_t)(info->block[data_channel + 1] - w->first);
    w->enabled = info->enabled;
    w->t = info->tag[data_channel];
    w->w = info->width[data_channel];
    w->nb = ((unsigned int)w->n + 7) / 8;
    w->hb_len = w->w + 1 + w->nb + 1;
    w->crc = CRC8_INIT;
    if (handler) {
        w->handler = *handler;
    }

    w->level = malloc(w->count ? w->count : 1);
    w->last = calloc(w->count ? w->count : 1, sizeof(*w->last));
    if (!w->level || !w->last) {
        wire_free(w);
        return -ENOMEM;
    }
    memset(w->level, -1, w->count);

    return 0;
}


/*
 * For a stream that does not start at the capture's start: its first heartbeat is at grid
 * count grid. With unverified, that heartbeat is taken as good without its check byte,
 * whose CRC-8 covers bytes before the stream. Call before the first wire_feed().
 */
void wire_start_at(struct wire *w, uint64_t grid, bool unverified)
{
    w->first_grid = grid;
    w->start_unverified = unverified;
}


void wire_free(struct wire *w)
{
    free(w->level);
    free(w->last);
    free(w->losses);
    free(w->mismatches);
    w->level = NULL;
    w->last = NULL;
    w->losses = NULL;
    w->mismatches = NULL;
}


void wire_feed(struct wire *w, const uint8_t *data, size_t len)
{
    size_t i = 0;

    w->stats.bytes += len;
    while (len) {
        size_t n = WIRE_BUF_SIZE - w->len;

        if (n > len) {
            n = len;
        }
        memcpy(&w->buf[w->len], data, n);
        w->len += n;
        data += n;
        len -= n;

        i = 0;
        for (;;) {
            uint64_t value = 0;

            if (!w->synced) {
                i = sync_at(w, i);
                if (!w->synced) {
                    break;
                }
            }
            if (w->len - i < w->w) {
                break;
            }
            for (unsigned int b = 0; b < w->w; b++) {
                value |= (uint64_t)w->buf[i + b] << (8 * b);
            }
            if (value == 0) {
                if (w->len - i < w->hb_len) {
                    break;
                }
                w->hb_pos = w->pos + i;
                heartbeat(w, &w->buf[i]);
                i += w->hb_len;
            } else {
                record(w, value, &w->buf[i]);
                i += w->w;
            }
        }
        memmove(w->buf, &w->buf[i], w->len - i);
        w->len -= i;
        w->pos += i;
    }
}


/*
 * The stream breaks here: bytes were lost between the last ones fed and the next (spec,
 * Transport loss). Records are untrusted until a heartbeat checks good again.
 */
void wire_break(struct wire *w)
{
    w->pos += w->len;
    w->len = 0;
    w->synced = false;
    w->trusted = false;
    w->crc = CRC8_INIT;
    w->stats.resyncs++;
}


/*
 * Takes a loss report: mismatches it explains, before or after it, are excused. span gets
 * the loss's first and last tick. Returns -ENOMEM if out of memory.
 */
int wire_add_loss(struct wire *w, const struct loss_report *rep, struct wire_span *span)
{
    span->start = wire_tick(w, rep->start_grid, rep->start_off);
    span->end = wire_tick(w, rep->end_grid, rep->end_off);

    return wire_add_loss_span(w, span);
}


/*
 * Takes a loss as a tick range: mismatches inside it, before or after it, are excused.
 * Returns -ENOMEM if out of memory.
 */
int wire_add_loss_span(struct wire *w, const struct wire_span *span)
{
    size_t keep = 0;
    int err = 0;

    // A loss that ended before the latest heartbeat cannot explain a later mismatch.
    for (size_t i = 0; i < w->loss_count; i++) {
        if (w->losses[i].end >= w->hb_tick) {
            w->losses[keep++] = w->losses[i];
        }
    }
    w->loss_count = keep;
    err = span_push(&w->losses, &w->loss_count, &w->loss_cap, span);
    if (err < 0) {
        return err;
    }

    keep = 0;
    for (size_t i = 0; i < w->mismatch_count; i++) {
        if (covered(w, &w->mismatches[i])) {
            w->stats.excused++;
            w->stats.mismatches--;
        } else {
            w->mismatches[keep++] = w->mismatches[i];
        }
    }
    w->mismatch_count = keep;

    return 0;
}


// The tick of a position (spec, Position); its grid count mod 2^32 is unwrapped.
uint64_t wire_tick(const struct wire *w, uint32_t grid, uint32_t off)
{
    int64_t ref = (int64_t)(w->have_grid ? w->grid : w->first_grid);
    int64_t g = ref + (int32_t)(grid - (uint32_t)ref);

    if (g < 0) {
        g = 0;
    }

    return (uint64_t)g * w->c + off;
}


/*
 * How far the data channel is known: a heartbeat follows every record of its tick, and the
 * device writes records in tick order.
 */
struct watermark wire_watermark(const struct wire *w)
{
    struct watermark wm = {.tick = w->hb_tick};

    if (w->latest > w->hb_tick + 1) {
        wm.tick = w->latest - 1;
    }

    return wm;
}


/*
 * Mismatches no loss explains in the tail: from the latest good heartbeat on. At the end of
 * a capture these may belong to a loss still open when the device was reset, whose report
 * never comes.
 */
uint64_t wire_tail_mismatches(const struct wire *w)
{
    uint64_t count = 0;

    for (size_t i = 0; i < w->mismatch_count; i++) {
        if (w->mismatches[i].start >= w->last_good) {
            count++;
        }
    }

    return count;
}


/*
 * Sets wr up to write data channel data_channel of the capture in info. info must outlive
 * wr. Returns -EINVAL for a data channel the capture does not have, -ENOMEM if out of
 * memory.
 */
int wire_writer_init(struct wire_writer *wr, const struct capture_info *info,
                     uint16_t data_channel)
{
    memset(wr, 0, sizeof(*wr));
    if (data_channel >= info->data_channels) {
        return -EINVAL;
    }

    wr->c = info->c;
    wr->n = info->n;
    wr->first = info->block[data_channel];
    wr->count = (uint16_t)(info->block[data_channel + 1] - wr->first);
    wr->enabled = info->enabled;
    wr->t = info->tag[data_channel];
    wr->w = info->width[data_channel];
    wr->nb = ((unsigned int)wr->n + 7) / 8;
    wr->crc = CRC8_INIT;

    wr->last = calloc(wr->count ? wr->count : 1, sizeof(*wr->last));
    if (!wr->last) {
        return -ENOMEM;
    }

    return 0;
}


void wire_writer_free(struct wire_writer *wr)
{
    free(wr->last);
    free(wr->out);
    wr->last = NULL;
    wr->out = NULL;
    wr->len = 0;
    wr->cap = 0;
}


/*
 * Writes a record (spec, Record). Returns -EINVAL for a channel not on this data channel or
 * a record before the first heartbeat, -ERANGE for a tick not after the record's gap
 * reference or more than C after it (spec, Gap), -ENOMEM if out of memory.
 */
int wire_write_record(struct wire_writer *wr, const struct record *r)
{
    const uint16_t *at = NULL;
    unsigned int lo = wr->first;
    unsigned int hi = wr->first + wr->count;
    unsigned int k = 0;
    uint64_t ref = 0;
    uint64_t gap = 0;
    uint64_t value = 0;
    uint8_t bytes[8];

    // The block is in ascending channel number.
    while (lo < hi) {
        unsigned int mid = (lo + hi) / 2;

        if (wr->enabled[mid] < r->channel) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    at = lo < (unsigned int)wr->first + wr->count ? &wr->enabled[lo] : NULL;
    if (!at || *at != r->channel || !wr->started) {
        return -EINVAL;
    }
    k = lo - wr->first;

    ref = wr->last[k] > wr->hb_tick ? wr->last[k] : wr->hb_tick;
    if (r->tick <= ref || r->tick - ref > wr->c) {
        return -ERANGE;
    }
    gap = r->tick - ref;

    // ZigZag of +gap for level 1, -gap for level 0, then the tag.
    value = ((r->level ? 2 * gap : 2 * gap - 1) << wr->t) | k;
    for (unsigned int b = 0; b < wr->w; b++) {
        bytes[b] = (uint8_t)(value >> (8 * b));
    }
    wr->last[k] = r->tick;

    return emit(wr, bytes, wr->w);
}


/*
 * Writes the heartbeat of grid count grid, then its check byte (spec, Heartbeat, Serial).
 * levels is a bitmask over the n logical channels. Returns -EINVAL for a grid count not
 * after the last heartbeat's, -ERANGE if a record written since is past the grid point,
 * -ENOMEM if out of memory.
 */
int wire_write_heartbeat(struct wire_writer *wr, uint64_t grid, const uint8_t *levels)
{
    uint8_t zero[8] = {0};
    uint8_t index = (uint8_t)grid;
    uint8_t last_byte = 0;
    int err = 0;

    if (wr->started && grid <= wr->grid) {
        return -EINVAL;
    }
    for (unsigned int k = 0; k < wr->count; k++) {
        if (wr->last[k] > grid * wr->c) {
            return -ERANGE;
        }
    }

    err = emit(wr, zero, wr->w);
    if (err == 0) {
        err = emit(wr, &index, 1);
    }
    if (err == 0 && wr->nb > 1) {
        err = emit(wr, levels, wr->nb - 1);
    }
    if (err == 0 && wr->nb > 0) {
        // Unused bits are 0.
        last_byte = levels[wr->nb - 1];
        if (wr->n % 8) {
            last_byte &= (uint8_t)((1U << (wr->n % 8)) - 1);
        }
        err = emit(wr, &last_byte, 1);
    }
    if (err == 0) {
        uint8_t check = wr->crc;

        err = emit(wr, &check, 1);
    }
    if (err < 0) {
        return err;
    }

    wr->crc = CRC8_INIT;
    wr->started = true;
    wr->grid = grid;
    wr->hb_tick = grid * wr->c;

    return 0;
}


// Empties the output; the stream state is kept.
void wire_writer_clear(struct wire_writer *wr)
{
    wr->len = 0;
}


// CRC-8 (spec, Serial): polynomial 0x07, MSB first, no reflection, no final XOR.
uint8_t crc8(uint8_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ CRC8_POLY) : (uint8_t)(crc << 1);
        }
    }

    return crc;
}


/*
 * Before the first heartbeat, or after a failed check: finds a heartbeat whose grid index
 * follows the last one received. Lost bytes are not reconstructed.
 */
static size_t sync_at(struct wire *w, size_t i)
{
    while (w->len - i >= w->hb_len) {
        bool zero = true;

        for (unsigned int b = 0; b < w->w; b++) {
            zero = zero && w->buf[i + b] == 0;
        }
        if (zero) {
            unsigned int step = (unsigned int)(w->buf[i + w->w] - (uint8_t)w->grid) & 0xFF;

            if (!w->have_grid || (step > 0 && step < RESYNC_STEP_MAX)) {
                w->synced = true;
                w->anchor = w->have_grid;
                w->crc = CRC8_INIT;
                return i;
            }
        }
        i++;
    }

    return i;
}


static void heartbeat(struct wire *w, const uint8_t *hb)
{
    struct heartbeat out = {0};
    uint64_t prev = w->hb_tick;
    uint64_t found = w->stats.mismatches + w->stats.excused;
    bool first_unverified = !w->have_grid && w->start_unverified;
    bool ok = false;
    uint8_t index = hb[w->w];

    w->crc = crc8(w->crc, hb, w->hb_len - 1);
    // A heartbeat found by resync is an anchor only: its check byte covers bytes before it
    // that were skipped. The next heartbeat's check decides.
    ok = w->anchor || first_unverified || w->crc == hb[w->hb_len - 1];
    w->crc = CRC8_INIT;
    if (!ok) {
        w->stats.crc_errors++;
        w->stats.resyncs++;
        w->trusted = false;
        w->synced = false;
        return;
    }

    if (!w->have_grid) {
        w->grid = w->first_grid + ((index - (uint8_t)w->first_grid) & 0xFF);
        w->have_grid = true;
    } else {
        w->grid += (index - (uint8_t)w->grid) & 0xFF;
    }
    w->hb_tick = w->grid * w->c;
    w->stats.heartbeats++;

    for (unsigned int k = 0; k < w->count; k++) {
        unsigned int i = w->first + k;
        int8_t lvl = (int8_t)((hb[w->w + 1 + i / 8] >> (i % 8)) & 1);

        if (w->trusted && w->level[k] >= 0 && w->level[k] != lvl) {
            mismatch(w, prev, w->hb_tick);
        }
        w->level[k] = lvl;
    }
    if (w->stats.mismatches + w->stats.excused == found) {
        w->last_good = w->hb_tick;
    }

    out.grid = w->grid;
    out.tick = w->hb_tick;
    out.data_channel = w->data_channel;
    out.n = w->n;
    out.flags = (w->anchor || first_unverified) ? HEARTBEAT_UNVERIFIED : 0;
    out.levels = &hb[w->w + 1];
    w->trusted = !w->anchor;
    w->anchor = false;
    if (w->handler.heartbeat) {
        w->handler.heartbeat(w->handler.ctx, &out);
    }
}


static void record(struct wire *w, uint64_t value, const uint8_t *raw)
{
    struct record out = {0};
    unsigned int k = 0;
    uint64_t z = 0;
    uint64_t gap = 0;

    w->crc = crc8(w->crc, raw, w->w);
    if (!w->trusted) {
        return;
    }

    k = (unsigned int)(value & ((1ULL << w->t) - 1));
    z = value >> w->t;
    gap = (z + 1) >> 1;
    if (k >= w->count || gap > w->c) {
        w->stats.gap_errors++;
        return;
    }

    out.tick = (w->last[k] > w->hb_tick ? w->last[k] : w->hb_tick) + gap;
    out.channel = w->enabled[w->first + k];
    out.level = (z & 1) ? 0 : 1;
    w->last[k] = out.tick;
    w->level[k] = (int8_t)out.level;
    if (out.tick > w->latest) {
        w->latest = out.tick;
    }
    w->stats.records++;
    if (w->handler.record) {
        w->handler.record(w->handler.ctx, &out);
    }
}


static bool covered(const struct wire *w, const struct wire_span *span)
{
    for (size_t i = 0; i < w->loss_count; i++) {
        if (w->losses[i].start <= span->end && w->losses[i].end >= span->start) {
            return true;
        }
    }

    return false;
}


// Levels disagreed between the heartbeats at ticks start and end.
static void mismatch(struct wire *w, uint64_t start, uint64_t end)
{
    struct wire_span span = {.start = start, .end = end};

    if (covered(w, &span)) {
        w->stats.excused++;
        return;
    }

    w->stats.mismatches++;
    // Out of memory, it still counts, but a later loss report can no longer excuse it.
    span_push(&w->mismatches, &w->mismatch_count, &w->mismatch_cap, &span);
}


static int span_push(struct wire_span **list, size_t *count, size_t *cap,
                     const struct wire_span *span)
{
    if (*count == *cap) {
        size_t grown = *cap ? 2 * *cap : 16;
        struct wire_span *bigger = realloc(*list, grown * sizeof(**list));

        if (!bigger) {
            return -ENOMEM;
        }
        *list = bigger;
        *cap = grown;
    }
    (*list)[(*count)++] = *span;

    return 0;
}


// Appends to the output and runs the CRC-8 over it.
static int emit(struct wire_writer *wr, const uint8_t *data, size_t len)
{
    if (wr->len + len > wr->cap) {
        size_t grown = wr->cap ? 2 * wr->cap : 4096;
        uint8_t *bigger = NULL;

        while (grown < wr->len + len) {
            grown *= 2;
        }
        bigger = realloc(wr->out, grown);
        if (!bigger) {
            return -ENOMEM;
        }
        wr->out = bigger;
        wr->cap = grown;
    }
    memcpy(&wr->out[wr->len], data, len);
    wr->len += len;
    wr->crc = crc8(wr->crc, data, len);

    return 0;
}
