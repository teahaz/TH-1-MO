/*
 * A backend: where a capture's raw traffic comes from. A device backend (the dummy, or the
 * device on this Pi) gives the hello, takes configuration messages, and then hands over
 * what arrives as items: data channel bytes, control messages the device sent, and host
 * events. These are the capture file's chunks (docs/capture-file.md), so the capture
 * session records them as they are and parses them the same way from any backend.
 */

#ifndef CAPTURE_SOURCE_H
#define CAPTURE_SOURCE_H

#include <stddef.h>
#include <stdint.h>


/*
 * type is THC_DATA, THC_CONTROL or THC_EVENT (capture/thcfile.h), with its flags and
 * channel as in the file. time is CLOCK_MONOTONIC in nanoseconds. data is valid until the
 * next read.
 */
struct source_item {
    uint8_t type;
    uint8_t flags;
    uint16_t channel;
    uint64_t time;
    const uint8_t *data;
    size_t len;
};

/*
 * hello resets the device and gives its hello message. config sends one configuration
 * message and gives the reply. read waits up to timeout_ms for the next item: 1 with an
 * item, 0 if none came, a negative errno on failure. stop ends the capture with a reset.
 * Messages given out are valid until the next call.
 */
struct source_ops {
    int (*hello)(void *src, const uint8_t **msg, size_t *len);
    int (*config)(void *src, const uint8_t *msg, size_t len, const uint8_t **reply,
                  size_t *reply_len);
    int (*read)(void *src, struct source_item *item, int timeout_ms);
    int (*stop)(void *src);
    void (*close)(void *src);
};

#endif
