/*
 * One device-to-host data channel on the serial data binding (protocol/spec.tex, Time and
 * records, Heartbeat, Loss, Serial): the parser turns its bytes into records and heartbeats,
 * the writer turns records and heartbeats into its bytes.
 *
 * The parser checks heartbeats by their CRC-8 check byte and against the levels the records
 * give. A mismatch inside a reported loss is excused, whichever of the two arrives first.
 * After a failed check it finds the next heartbeat itself; records are untrusted, and not
 * reported, until a heartbeat checks good again.
 */

#ifndef CAPTURE_WIRE_H
#define CAPTURE_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "capture/control.h"
#include "common/capture_info.h"
#include "common/record.h"
#include "common/status.h"


#define WIRE_BUF_SIZE 65536

struct wire_handler {
    void (*record)(void *ctx, const struct record *r);
    void (*heartbeat)(void *ctx, const struct heartbeat *hb);
    void *ctx;
};

// A tick range, inclusive.
struct wire_span {
    uint64_t start;
    uint64_t end;
};

/*
 * The data channel carries logical channels first to first + count - 1 of n (its block);
 * enabled gives their channel numbers and is borrowed from the capture info. grid is the
 * unwrapped grid count of the latest heartbeat, level and last are per block channel (level
 * -1 before it is known). pos is the stream offset of buf[0], counted over every byte fed;
 * hb_pos is the stream offset of the heartbeat being handled, for the heartbeat handler.
 * last_good is the tick of the latest heartbeat that checked good with no mismatch.
 */
struct wire {
    uint32_t c;
    uint16_t data_channel;
    uint16_t n;
    uint16_t first;
    uint16_t count;
    const uint16_t *enabled;
    unsigned int t;
    unsigned int w;
    unsigned int nb;
    unsigned int hb_len;

    uint8_t buf[WIRE_BUF_SIZE];
    size_t len;
    uint64_t pos;
    uint64_t hb_pos;
    uint8_t crc;
    bool synced;
    bool anchor;
    bool trusted;
    bool have_grid;
    bool start_unverified;
    uint64_t first_grid;
    uint64_t grid;
    uint64_t hb_tick;
    uint64_t latest;
    uint64_t last_good;
    int8_t *level;
    uint64_t *last;

    // Losses that may still explain a mismatch, and mismatches no loss explains yet.
    struct wire_span *losses;
    size_t loss_count;
    size_t loss_cap;
    struct wire_span *mismatches;
    size_t mismatch_count;
    size_t mismatch_cap;

    struct data_stats stats;
    struct wire_handler handler;
};

// The writer's state for one data channel; out holds the bytes written so far.
struct wire_writer {
    uint32_t c;
    uint16_t n;
    uint16_t first;
    uint16_t count;
    const uint16_t *enabled;
    unsigned int t;
    unsigned int w;
    unsigned int nb;

    uint8_t crc;
    bool started;
    uint64_t grid;
    uint64_t hb_tick;
    uint64_t *last;

    uint8_t *out;
    size_t len;
    size_t cap;
};


int wire_init(struct wire *w, const struct capture_info *info, uint16_t data_channel,
              const struct wire_handler *handler);
void wire_start_at(struct wire *w, uint64_t grid, bool unverified);
void wire_free(struct wire *w);
void wire_feed(struct wire *w, const uint8_t *data, size_t len);
void wire_break(struct wire *w);
int wire_add_loss(struct wire *w, const struct loss_report *rep, struct wire_span *span);
int wire_add_loss_span(struct wire *w, const struct wire_span *span);
uint64_t wire_tick(const struct wire *w, uint32_t grid, uint32_t off);
struct watermark wire_watermark(const struct wire *w);
uint64_t wire_tail_mismatches(const struct wire *w);

int wire_writer_init(struct wire_writer *wr, const struct capture_info *info,
                     uint16_t data_channel);
void wire_writer_free(struct wire_writer *wr);
int wire_write_record(struct wire_writer *wr, const struct record *r);
int wire_write_heartbeat(struct wire_writer *wr, uint64_t grid, const uint8_t *levels);
void wire_writer_clear(struct wire_writer *wr);

uint8_t crc8(uint8_t crc, const uint8_t *data, size_t len);

#endif
