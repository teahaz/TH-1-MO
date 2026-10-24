/*
 * Writes a capture file (docs/capture-file.md) and its index file as a capture goes on.
 *
 * The caller hands over the header, then data bytes, control messages, events and good
 * heartbeats; none of these calls waits on storage. A writer thread turns them into chunks
 * and writes them in batches of at least BATCH_MIN bytes, or what has gathered after
 * FLUSH_MS, with an fsync every SYNC_MS. If storage falls behind by more than QUEUE_MAX
 * bytes, data is dropped and an overflow event marks the gap. A failed write stops the
 * recording; recorder_error() then says why, and the calls do nothing.
 */

#ifndef CAPTURE_RECORDER_H
#define CAPTURE_RECORDER_H

#include <stddef.h>
#include <stdint.h>

#include "capture/thcfile.h"
#include "common/capture_info.h"


#define RECORDER_BATCH_MIN (2U << 20)
#define RECORDER_FLUSH_MS 2000
#define RECORDER_SYNC_MS 5000
#define RECORDER_QUEUE_MAX (32U << 20)

// A message for the header: a configuration (to_device) or a configuration reply.
struct recorder_msg {
    uint16_t type;
    const uint8_t *data;
    size_t len;
};

/*
 * The header's contents. configs are the configuration messages and replies in the order
 * they happened. loss_base is NULL for a recording from the capture's start.
 */
struct recorder_header {
    const uint8_t *hello;
    size_t hello_len;
    const struct recorder_msg *configs;
    size_t config_count;
    const struct capture_info *info;
    const char *binding;
    const char *host;
    uint64_t start_time;
    uint64_t first_grid;
    const uint8_t *loss_base;
    size_t loss_base_len;
};

struct recorder_stats {
    uint64_t written;
    uint64_t queued;
    uint64_t overflow_bytes;
    uint64_t overflow_events;
    uint64_t index_entries;
};

struct recorder;


int recorder_open(struct recorder **out, const char *path);
int recorder_begin(struct recorder *r, const struct recorder_header *h);
void recorder_data(struct recorder *r, uint16_t channel, uint64_t time, const uint8_t *data,
                   size_t len);
void recorder_control(struct recorder *r, uint8_t flags, uint64_t time, const uint8_t *msg,
                      size_t len);
void recorder_event(struct recorder *r, uint16_t code, uint16_t channel, uint64_t time,
                    const uint8_t *data, size_t len);
void recorder_heartbeat(struct recorder *r, uint16_t channel, uint64_t grid, uint64_t pos);
int recorder_close(struct recorder *r, uint64_t time, struct thc_trailer *t);
int recorder_error(struct recorder *r);
void recorder_stats(struct recorder *r, struct recorder_stats *st);

#endif
