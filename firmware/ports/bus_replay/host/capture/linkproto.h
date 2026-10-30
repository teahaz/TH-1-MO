/*
 * The link protocol's messages (docs/link-protocol.md), for the link (modes/link.c) and its
 * client (capture/remote.c): the header, parsing messages out of a byte stream, the
 * socket's path, and writing with a timeout.
 */

#ifndef CAPTURE_LINKPROTO_H
#define CAPTURE_LINKPROTO_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "capture/thcfile.h"


#define LINK_MAJOR 1
#define LINK_MINOR 0
#define LINK_HEAD 16
#define LINK_PAYLOAD_MAX (64U << 20)

// Items have the capture file's chunk types: THC_DATA, THC_CONTROL, THC_EVENT.
#define LINK_HELLO 0x10
#define LINK_CONFIG 0x11
#define LINK_STOP 0x12
#define LINK_ATTACH 0x13
#define LINK_RECORD 0x14
#define LINK_STATUS 0x15
#define LINK_WELCOME 0x20
#define LINK_BUSY 0x21
#define LINK_REPLY 0x80

// WELCOME and STATUS state bits.
#define LINK_RUNNING 0x01
#define LINK_RECORDING 0x02

// A message. data points into the buffer it was parsed from.
struct link_msg {
    uint8_t type;
    uint8_t flags;
    uint16_t channel;
    uint64_t time;
    const uint8_t *data;
    size_t len;
};


int link_put(struct thc_buf *b, const struct link_msg *m);
ssize_t link_parse(const uint8_t *buf, size_t len, struct link_msg *m);
int link_path(char *buf, size_t size, const char *given);
int link_send(int fd, const uint8_t *data, size_t len, int timeout_ms);

#endif
