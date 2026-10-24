#include <stddef.h>

#include "codec/activity/activity.h"
#include "codec/codec.h"
#include "codec/i2c/i2c.h"
#include "codec/uart/uart.h"


// Chip-level decoders (codec/chips/): each one's file defines its decoder.
extern const struct codec_decoder ina219_decoder;
extern const struct codec_decoder sht3x_decoder;

// Every decoder built in, by name (codec_find()). NULL-terminated.
const struct codec_decoder *const codec_decoders[] = {
    &i2c_decoder,
    &uart_decoder,
    &activity_decoder,
    &ina219_decoder,
    &sht3x_decoder,
    NULL,
};
