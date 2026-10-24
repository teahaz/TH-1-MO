/*
 * The index file beside a capture file (docs/capture-file.md, Index file): one entry per
 * data channel for the first good heartbeat at or after every K-th grid point, pointing at
 * the DATA chunk that holds the heartbeat's first byte.
 *
 * The builder is fed the DATA chunks as they go into the file (with their stream offsets)
 * and the good heartbeats as the parser finds them (with theirs), and pairs them up.
 */

#ifndef CAPTURE_INDEX_H
#define CAPTURE_INDEX_H

#include <stddef.h>
#include <stdint.h>

#include "capture/thcfile.h"


#define INDEX_HEADER_LEN 32
#define INDEX_ENTRY_LEN 24

extern const uint8_t index_magic[THC_MAGIC_LEN];

struct index_entry {
    uint64_t grid;
    uint64_t chunk;
    uint32_t payload;
    uint16_t channel;
};

// A DATA chunk's stream bytes: stream offsets start to start + len - 1, at file offset chunk.
struct index_span {
    uint64_t start;
    uint64_t len;
    uint64_t chunk;
};

// One data channel: the spans that may still hold a heartbeat to index, and the next due.
struct index_lane {
    struct index_span *spans;
    size_t head;
    size_t count;
    size_t cap;
    uint64_t next_grid;
};

// out collects the entries made since it was last emptied.
struct index_builder {
    uint32_t k;
    uint16_t data_channels;
    struct index_lane *lane;
    struct thc_buf out;
    uint64_t entries;
};


uint32_t index_interval(uint32_t rate, uint32_t c);
uint64_t index_next_grid(uint64_t grid, uint32_t k);
char *index_path(const char *capture_path);

int index_builder_init(struct index_builder *ib, uint16_t data_channels, uint32_t k);
void index_builder_free(struct index_builder *ib);
int index_builder_chunk(struct index_builder *ib, uint16_t channel, uint64_t start,
                        uint64_t len, uint64_t chunk);
int index_builder_heartbeat(struct index_builder *ib, uint16_t channel, uint64_t grid,
                            uint64_t pos);

int index_header(struct thc_buf *b, uint32_t header_crc, uint64_t start_time, uint32_t k);
int index_check(const char *path, uint32_t header_crc, uint64_t start_time);
int index_read(const char *path, uint32_t *k, struct index_entry **entries, size_t *count);

#endif
