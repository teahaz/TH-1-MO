/*
 * What a replay asks of the device's pins, per channel and tick. Not used yet: the spec's
 * replay format is still open (protocol/spec.tex, Replay).
 */

#ifndef COMMON_REPLAY_H
#define COMMON_REPLAY_H

#include <stdint.h>


enum replay_drive {
    REPLAY_RELEASE,
    REPLAY_LOW,
    REPLAY_HIGH,
};

// From tick on, the channel's replay pin does what drive says.
struct replay_step {
    uint64_t tick;
    uint16_t channel;
    uint8_t drive;
};

#endif
