/*
 * A stand-in device, for work without hardware: a 6-channel device on one data channel,
 * with test signals. It answers the hello and configuration messages as the device does,
 * writes its data stream with the wire writer, and periodically drops records in a loss and
 * reports it, so what comes out goes through the same parser as a real device's.
 *
 * Its signals, by channel: I2C SCL and SDA on 0 and 1 (SHT3x measurements), 2 held high, a
 * UART at 115200 8N1 on 3, a clock on 4, a PWM on 5.
 */

#ifndef CAPTURE_DUMMY_H
#define CAPTURE_DUMMY_H

#include <stdbool.h>

#include "capture/source.h"


/*
 * realtime: the capture runs at the sample rate, as a device would. Otherwise each read
 * gives the next 50 ms of capture at once, for tests.
 */
struct dummy_options {
    bool realtime;
};

extern const struct source_ops dummy_ops;


int dummy_open(void **src, const struct dummy_options *opt);

#endif
