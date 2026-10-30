/*
 * The device through a link (docs/link-protocol.md): a client of bus-replay link, which
 * owns the device in a process of its own. The source operations go to the link as
 * requests, and the link's stream of raw items is what read gives. The link does the
 * recording, so it goes on if this client goes away.
 *
 * A link may already run a capture when the client connects (remote_running()). Joining it
 * goes through remote_attach() and capture_attach() (capture/capture.h); hello instead
 * ends it, as it resets the device.
 *
 * Requests wait for their reply up to REMOTE_TIMEOUT_MS. Items that arrive meanwhile are
 * queued, and read gives them first; only a reply is waited for, so the queue holds what
 * one request's wait lets in. Item times are the link's, moved onto this host's monotonic
 * clock at the capture's start or join.
 */

#ifndef CAPTURE_REMOTE_H
#define CAPTURE_REMOTE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "capture/source.h"


#define REMOTE_TIMEOUT_MS 5000
#define REMOTE_PATH_MAX 108

// path: the link's socket (NULL: remote_default_path()).
struct remote_options {
    const char *path;
};

/*
 * What the link says of itself (link protocol, STATUS): whether a capture runs and is
 * recorded, where to and how much so far, the recording's error (0 or a negative errno),
 * and what the link dropped for this client because it read too slowly.
 */
struct remote_status {
    bool running;
    bool recording;
    char path[256];
    uint64_t recorded;
    int record_error;
    uint64_t dropped_bytes;
    uint32_t dropped_control;
};

struct capture_attach;

extern const struct source_ops remote_ops;


int remote_default_path(char *buf, size_t size);
int remote_open(void **src, const struct remote_options *opt);
bool remote_running(void *src);

/*
 * What a capture already running needs to be joined: fills a (valid until the next call)
 * and starts the item stream at its first grid count. -ENOENT if no capture runs.
 */
int remote_attach(void *src, struct capture_attach *a);

// The file the link records the next capture to; NULL or "" for none. -EBUSY while one runs.
int remote_record(void *src, const char *path);
int remote_status(void *src, struct remote_status *st);

#endif
