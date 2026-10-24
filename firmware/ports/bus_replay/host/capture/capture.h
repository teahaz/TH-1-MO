/*
 * A capture session: one capture from a backend (capture/source.h) or one capture file,
 * parsed into records, heartbeats and losses.
 *
 * Single-threaded and pull based: the caller calls capture_pull() whenever it wants more,
 * and the handler is called from inside it. A live capture can be recorded to a capture
 * file (capture_record() before capture_start()); the file then holds exactly the traffic
 * the session parsed.
 */

#ifndef CAPTURE_CAPTURE_H
#define CAPTURE_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "capture/control.h"
#include "capture/dummy.h"
#include "capture/recorder.h"
#include "capture/source.h"
#include "capture/thcfile.h"
#include "common/capture_info.h"
#include "common/record.h"
#include "common/status.h"


// Any of the callbacks may be NULL. An event's data follows its code, as in the file.
struct capture_handler {
    void (*record)(void *ctx, const struct record *r);
    void (*heartbeat)(void *ctx, const struct heartbeat *hb);
    void (*loss)(void *ctx, const struct loss *loss);
    void (*event)(void *ctx, uint16_t code, uint16_t channel, const uint8_t *data, size_t len);
    void *ctx;
};

/*
 * What to start. 0 or NULL keeps the device's current value (from the hello). Sets are
 * channel sets of ceil(N/8) bytes.
 */
struct capture_config {
    uint32_t rate;
    uint32_t c;
    const uint8_t *enabled;
    const uint8_t *split;
};

// What a recording adds to the header.
struct capture_record_options {
    const char *binding;
    const char *host;
};

struct capture;


int capture_open(struct capture **out, const struct source_ops *ops, void *src,
                 const struct capture_handler *handler);
int capture_open_dummy(struct capture **out, const struct dummy_options *opt,
                       const struct capture_handler *handler);
int capture_open_local(struct capture **out, const struct capture_handler *handler);
int capture_open_file(struct capture **out, const char *path,
                      const struct capture_handler *handler);
void capture_close(struct capture *cap);

int capture_hello(struct capture *cap, const struct hello **h);
int capture_record(struct capture *cap, const char *path,
                   const struct capture_record_options *opt);
int capture_start(struct capture *cap, const struct capture_config *cfg, char *error,
                  size_t size);
int capture_pull(struct capture *cap, int timeout_ms);
int capture_stop(struct capture *cap, uint8_t reason);
int capture_seek(struct capture *cap, uint64_t tick);
int capture_index_rebuild(const char *path);

const struct capture_info *capture_info(const struct capture *cap);
void capture_status(const struct capture *cap, struct capture_status *st);
const struct data_stats *capture_data_stats(const struct capture *cap, unsigned int d);
const uint64_t *capture_channel_records(const struct capture *cap);
const uint64_t *capture_channel_lost(const struct capture *cap);
uint64_t capture_time(const struct capture *cap);
uint64_t capture_overflow(const struct capture *cap);
uint64_t capture_tail_mismatches(const struct capture *cap);
struct recorder *capture_recorder(const struct capture *cap);
struct thc_reader *capture_reader(const struct capture *cap);
int capture_trailer(const struct capture *cap, uint8_t reason, struct thc_trailer *t);

#endif
