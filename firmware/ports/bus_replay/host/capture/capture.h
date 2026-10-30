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
#include "capture/remote.h"
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

/*
 * A capture that already runs, to join from where it has got to (capture_attach()): the
 * hello, the configuration messages and replies it was started with, in order (types
 * THC_ITEM_CONFIG and THC_ITEM_CONFIG_REPLY), the last loss report before the join (NULL
 * for none), and the grid count at which every data channel's stream begins with a
 * heartbeat (0: from the capture's start), with the capture's time there (ns from its
 * start), which item times go on from. The same as a capture file that starts later
 * (docs/capture-file.md, Header).
 */
struct capture_attach {
    const uint8_t *hello;
    size_t hello_len;
    const struct recorder_msg *configs;
    size_t config_count;
    const uint8_t *loss_base;
    size_t loss_base_len;
    uint64_t first_grid;
    uint64_t time;
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
int capture_open_remote(struct capture **out, const struct remote_options *opt,
                        const struct capture_handler *handler);
void capture_close(struct capture *cap);

int capture_hello(struct capture *cap, const struct hello **h);
int capture_hello_msg(const struct capture *cap, const uint8_t **msg, size_t *len);
int capture_record(struct capture *cap, const char *path,
                   const struct capture_record_options *opt);
int capture_start(struct capture *cap, const struct capture_config *cfg, char *error,
                  size_t size);
int capture_attach(struct capture *cap, const struct capture_attach *a, char *error,
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
void *capture_source(const struct capture *cap);
uint64_t capture_stream_pos(const struct capture *cap, uint16_t data_channel);
int capture_trailer(const struct capture *cap, uint8_t reason, struct thc_trailer *t);

#endif
