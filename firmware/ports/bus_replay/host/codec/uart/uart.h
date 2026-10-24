/*
 * UART, no parity, one stop bit, idle high: bursts of bytes. A burst is the bytes of one
 * direction until the other direction sends or the line has been idle for more than gap
 * byte times. Its arguments: tx=, rx= (channels, at least one), baud=, bits= (5 to 9,
 * default 8), gap= (byte times, default UART_GAP).
 *
 * A burst's words are its bytes, on the tx or rx lane, flagged CODEC_WORD_ERROR when the
 * start or stop bit was wrong (a framing error). It offers a raw export: the bytes alone.
 */

#ifndef CODEC_UART_H
#define CODEC_UART_H

#include "codec/codec.h"
#include "codec/item.h"


#define UART_GAP 10

// Burst fields.
#define UART_DIR 0
#define UART_BYTES 1
#define UART_ERRORS 2

#define UART_TX 0
#define UART_RX 1

extern const struct codec_kind uart_burst;
extern const struct codec_decoder uart_decoder;

#endif
