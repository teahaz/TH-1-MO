/*
 * The store: one capture's raw data in memory, for the UI to show and for decoders to read
 * back. It holds the records of every enabled channel, the losses, and the heartbeats that
 * say something about a channel's level. Nothing in it is decoded.
 *
 * Records are kept per channel in blocks of STORE_BLOCK_RECORDS, each record packed as
 * tick << 1 | level. The memory limit counts the blocks and everything kept beside them
 * (kept heartbeats, spans of unknown level, losses, heartbeats held for late losses). Past
 * it, the oldest full block of the store is dropped, with what was kept for its span, so
 * the store keeps the newest stretch of the capture whatever its rate; with no full block
 * left to drop, the older half of what one channel keeps beside its records goes instead.
 * A channel's records have strictly increasing ticks: a record is a change of level at a
 * sample.
 *
 * Levels are three-state: low, high or unknown. A record is a change to its level, so a
 * channel's level is known from its first record or its first heartbeat, whichever comes
 * first. A loss on a channel makes its level unknown from the start of the loss up to the
 * first record or heartbeat after the loss's end. Of the heartbeats only these are kept: one
 * that makes a level known (the first, or the first after a loss), and one whose level
 * disagrees with the records before it. The level a kept heartbeat gives holds from the end
 * of its tick, after the records of that tick. Loss reports can come in late (up to about a
 * second of capture), so the heartbeats of the last two seconds are held for them.
 *
 * Channels are the device's channel numbers, as in records (common/record.h). Times are
 * ticks. Positions are absolute: a channel's position p is its p-th record since the
 * capture started (from 0), and does not change when older records are dropped.
 *
 * Threads: one writer and any number of readers, with one mutex. The writer gathers what
 * one capture_pull() hands over into a batch, without the mutex, then writes the batch in
 * one store_write(); every query takes the mutex for its own duration. Nothing that waits
 * (I/O, capture_pull(), a lock of the caller's) may happen while the mutex is held.
 *
 * Records arrive per data channel, and the data channels independently of each other, so
 * records may have arrived past the watermark on some channels but not on others.
 * Everything up to the watermark is complete and does not change, except that a loss
 * reported late can still make levels in it unknown, and the oldest data can be dropped. A
 * reader that bounds its queries by a watermark it read once (store_stats()) sees the same
 * data in every query, such as the lanes of one frame.
 *
 * Replaying the store into a decoder added later (store_read_merged()) gives records only;
 * advancing the decoder without heartbeats is for the codec side to provide.
 */

#ifndef UI_STORE_STORE_H
#define UI_STORE_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common/capture_info.h"
#include "common/record.h"


#define STORE_BLOCK_RECORDS 65536
#define STORE_LIMIT_DEFAULT (256U << 20)

// A channel's level, as given by store_level() and in a column.
#define STORE_LOW 0
#define STORE_HIGH 1
#define STORE_UNKNOWN 2

/*
 * Column flags.
 *  - LOSS: the column overlaps a loss on the channel.
 *  - UNKNOWN: the channel's level is unknown somewhere in the column.
 *  - NO_DATA: part of the column is not held: older than the store's oldest tick, or past
 *    what has arrived on the channel.
 */
#define STORE_COL_LOSS 0x01
#define STORE_COL_UNKNOWN 0x02
#define STORE_COL_NO_DATA 0x04

/*
 * limit is the memory the store may take, in bytes (0: STORE_LIMIT_DEFAULT). The store
 * keeps at least the block each channel is writing to, whatever the limit.
 */
struct store_options {
    size_t limit;
};

/*
 * A column of a lane, ticks start to end - 1: the level at the end of the tick before
 * start, the level at the end of end - 1, and the records in between.
 */
struct store_column {
    uint8_t start_level;
    uint8_t end_level;
    uint8_t flags;
    uint32_t changes;
};

// A loss: ticks start to end, inclusive, and the records lost on every channel together.
struct store_loss {
    uint64_t start;
    uint64_t end;
    uint64_t lost;
};

/*
 * The store as a whole. oldest: the first tick held on every channel; watermark: every
 * record up to and including it has arrived, on every channel (have_watermark false until
 * every data channel has sent a heartbeat); newest: the newest record's tick on any
 * channel. records are the records held, total the records since the capture started,
 * dropped those dropped to stay within limit. losses counts the loss reports since the
 * start. bytes is the memory the store takes, counted as the limit counts it.
 */
struct store_stats {
    uint64_t oldest;
    uint64_t watermark;
    uint64_t newest;
    bool have_watermark;
    uint64_t records;
    uint64_t total;
    uint64_t dropped;
    uint64_t losses;
    size_t bytes;
    size_t limit;
};

/*
 * One channel. first is the position of its oldest record held, next the position its
 * next record will have (its record count since the start). oldest is the first tick its
 * levels are held from. arrived: every record of the channel up to and including it has
 * arrived (its data channel's last heartbeat; have_arrived false before the first).
 */
struct store_channel_stats {
    uint64_t first;
    uint64_t next;
    uint64_t oldest;
    uint64_t arrived;
    bool have_arrived;
};

struct store;
struct store_batch;


/*
 * info must outlive the store; it is the capture's, as the capture session gives it. A
 * store is for one capture: a new capture gets a new store.
 */
int store_new(struct store **out, const struct capture_info *info,
              const struct store_options *opt);
void store_free(struct store *s);
const struct capture_info *store_info(const struct store *s);

/*
 * A batch: what the capture session hands over (capture/capture.h, struct capture_handler)
 * between two writes, in the order it came, copied so that it outlives the callbacks. The
 * calls fit the handler's callbacks and report nothing; the first error (out of memory)
 * stays with the batch, and store_write() returns it. store_batch_clear() empties it for
 * the next pull, error included.
 */
int store_batch_new(struct store_batch **out, const struct capture_info *info);
void store_batch_free(struct store_batch *b);
void store_batch_record(struct store_batch *b, const struct record *r);
void store_batch_heartbeat(struct store_batch *b, const struct heartbeat *hb);
void store_batch_loss(struct store_batch *b, const struct loss *loss);
void store_batch_clear(struct store_batch *b);

/*
 * Writes a batch, under the mutex once. Everything in it that can be taken is: a record
 * out of order (-EINVAL) or for a channel that is not enabled is left out, and the first
 * error is returned. -ENOMEM stops the write where it got to; the store stays consistent.
 */
int store_write(struct store *s, const struct store_batch *b);

/*
 * Reading. Each call is one step under the mutex. -EINVAL for a channel that is not
 * enabled, -ERANGE for a tick or position that is no longer (or not yet) held.
 */
void store_stats(struct store *s, struct store_stats *st);
int store_channel_stats(struct store *s, uint16_t channel, struct store_channel_stats *st);

// The channel's level at the end of tick: STORE_LOW, STORE_HIGH or STORE_UNKNOWN.
int store_level(struct store *s, uint16_t channel, uint64_t tick);

/*
 * A lane of every channel in channels (count of them) at once: cols columns, column x
 * covering ticks edges[x] to edges[x + 1] - 1 (edges has cols + 1 ascending entries, so a
 * view may give idle stretches fewer columns). out has count * cols entries, channel k's
 * columns from out[k * cols]. A column that is not held is flagged STORE_COL_NO_DATA, and
 * a level that is not held reads STORE_UNKNOWN.
 */
int store_columns(struct store *s, const uint16_t *channels, size_t count,
                  const uint64_t *edges, size_t cols, struct store_column *out);

/*
 * The channel's records from tick from to tick to (inclusive), at most max, in count;
 * the span is cut to what is held. *more is true if records in the span were left out;
 * the next call can go on from the last one's tick + 1.
 */
int store_records(struct store *s, uint16_t channel, uint64_t from, uint64_t to,
                  struct record *out, size_t max, size_t *count, bool *more);

/*
 * The first record after tick (next) or the last one before it (prev) on any of channels
 * (the lowest channel of a tick first), in found. -ENOENT if there is none in what is held
 * (next: up to the watermark).
 */
int store_next(struct store *s, const uint16_t *channels, size_t count, uint64_t tick,
               struct record *found);
int store_prev(struct store *s, const uint16_t *channels, size_t count, uint64_t tick,
               struct record *found);

/*
 * For decoders: the records of the channel that have arrived, from position *pos on, at
 * most max. *pos is advanced past them, so it is where the next call goes on; it equals the
 * channel's next position (store_channel_stats()) once everything has been read.
 */
int store_read(struct store *s, uint16_t channel, uint64_t *pos, struct record *out,
               size_t max, size_t *count);

/*
 * The same over every enabled channel, merged in tick order (by channel within a tick), up
 * to the watermark. pos has one entry per logical channel (capture_info.h: entry i for
 * channel enabled[i]), each advanced past its records.
 */
int store_read_merged(struct store *s, uint64_t *pos, struct record *out, size_t max,
                      size_t *count);

/*
 * The losses overlapping ticks from to to (inclusive), oldest first, at most max, in count.
 * lost is NULL, or has max * n entries (n logical channels): loss k's records lost per
 * logical channel from lost[k * n], 0 for a channel that lost none.
 */
int store_losses(struct store *s, uint64_t from, uint64_t to, struct store_loss *out,
                 uint32_t *lost, size_t max, size_t *count);

#endif
