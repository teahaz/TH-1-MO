/*
 * The device on this Pi (capture/board.h): the SPI control binding with pin reset, and one
 * serial data channel.
 *
 * A reader thread takes the data UART's bytes as they come, timestamps them and queues
 * them, gathering up to COALESCE_MS of them into one item; it never waits on the caller,
 * the parser or storage. If the caller falls behind by more than QUEUE_MAX bytes, data is
 * dropped and an overflow event says how much. Everything else (SPI transactions, the IRQ
 * line, the events) happens in the caller's thread, inside the source operations.
 */

#ifndef CAPTURE_LOCAL_H
#define CAPTURE_LOCAL_H

#include "capture/source.h"


extern const struct source_ops local_ops;


int local_open(void **src);

#endif
