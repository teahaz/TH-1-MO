/*
 * Capture settings and the capture loop, shared by the control side (hello, registers,
 * start checks) and main.
 */

#ifndef CAPTURE_H
#define CAPTURE_H

#include <stdint.h>


// Channels 0 to 5 are the cable's CH1 to CH6, all on P0 so one read samples them all.
#define CHANNEL_COUNT 6
#define DATA_CHANNEL_COUNT 1

// Ticks are paced in CPU cycles: sample rate = CPU_HZ / divisor.
#define CPU_HZ 64000000
#define MIN_DIVISOR 128
#define MAX_DIVISOR CPU_HZ

#define DATA_BAUD 1000000
#define UART_BITS_PER_BYTE 10

// Output ring: the spec's buffer, in RAM8.
#define OUT_SIZE (128 * 1024)

// C up to 2^27 - 1 keeps W at 4 bytes or less with any number of channels.
#define C_MIN 1
#define C_MAX ((1UL << 27) - 1)

// IRQ line to the Pi, driven by the control side and by the capture loop.
#define IRQ_PIN 26 // P0.26

#define CAPTURE_ERR_LAPPED 1
#define CAPTURE_ERR_HEARTBEAT_LATE 2


struct capture_config {
    uint32_t sample_rate;
    uint32_t c;
    uint16_t enabled;
    uint16_t split;
};

// One cable channel: the wire pair in one J12 column.
struct channel {
    uint8_t sense; // P0 pin, bottom row
    uint8_t replay; // P0 pin, top row, not driven
    const char *name;
};


extern const struct channel channels[CHANNEL_COUNT];


uint32_t capture_record_width(const struct capture_config *cfg);
uint32_t capture_heartbeat_size(const struct capture_config *cfg);
void capture_prepare(const struct capture_config *cfg);
int capture_run(uint32_t *ticks);

#endif
