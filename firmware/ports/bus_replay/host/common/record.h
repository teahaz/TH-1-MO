/*
 * The raw data of a capture, as every library sees it: records, heartbeats, losses and the
 * watermark. Times are ticks on the capture timeline (protocol/spec.tex, Time and records,
 * Heartbeat); the sample rate that turns them into seconds is in the capture info.
 *
 * Channels are the device's channel numbers. A logical channel's index is its position
 * among the enabled channels in ascending channel number (capture_info.h).
 */

#ifndef COMMON_RECORD_H
#define COMMON_RECORD_H

#include <stdbool.h>
#include <stdint.h>


// A heartbeat whose check byte could not be checked: found by resync, or the first one of
// a recording that starts later. Its levels are taken as they are.
#define HEARTBEAT_UNVERIFIED 0x01

// A level change on one channel, at the end of tick.
struct record {
    uint64_t tick;
    uint16_t channel;
    uint8_t level;
};

/*
 * The level of every logical channel at the end of the heartbeat's tick, as one data
 * channel received it. levels is a bitmask over the n logical channels: logical channel i
 * is bit (i % 8) of byte i / 8. It is valid only while the heartbeat is being handled.
 */
struct heartbeat {
    uint64_t grid;
    uint64_t tick;
    uint16_t data_channel;
    uint16_t n;
    uint8_t flags;
    const uint8_t *levels;
};

/*
 * A loss: records on the listed logical channels may be missing from start to end (ticks,
 * inclusive). lost has one entry per logical channel: the records lost in this loss, 0 for
 * a channel that lost none.
 */
struct loss {
    uint64_t start;
    uint64_t end;
    uint16_t n;
    const uint32_t *lost;
};

// Every record up to and including tick has arrived.
struct watermark {
    uint64_t tick;
};


static inline bool heartbeat_level(const struct heartbeat *hb, unsigned int i)
{
    return (hb->levels[i / 8] >> (i % 8)) & 1;
}

#endif
