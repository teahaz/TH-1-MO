/*
 * The capture file format (docs/capture-file.md): building its parts for a writer, and
 * reading a file back chunk by chunk. The index file is in capture/index.h.
 */

#ifndef CAPTURE_THCFILE_H
#define CAPTURE_THCFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common/capture_info.h"
#include "common/status.h"


#define THC_MAJOR 1
#define THC_MINOR 0

#define THC_MAGIC_LEN 8
#define THC_FIXED_LEN 40
#define THC_CHUNK_HEAD 16
#define THC_CHUNK_CRC 4
#define THC_FOOTER_LEN 16
#define THC_END_FIXED 48
#define THC_END_DATA 48
#define THC_END_LOGICAL 16

#define THC_ITEM_HELLO 0x0001
#define THC_ITEM_CONFIG 0x0002
#define THC_ITEM_CONFIG_REPLY 0x0003
#define THC_ITEM_INFO 0x0004
#define THC_ITEM_BINDING 0x0005
#define THC_ITEM_HOST 0x0006
#define THC_ITEM_NOTE 0x0007
#define THC_ITEM_LOSS_BASE 0x0008

#define THC_DATA 0x01
#define THC_CONTROL 0x02
#define THC_EVENT 0x03
#define THC_END 0x7F

// CONTROL flags, bit 0: the message went host to device.
#define THC_TO_DEVICE 0x01

#define THC_EV_STOP 0x0001
#define THC_EV_DATA_ERROR 0x0002
#define THC_EV_DATA_SILENT 0x0003
#define THC_EV_OVERFLOW 0x0004
#define THC_EV_CONTROL_ERROR 0x0005
#define THC_EV_NOTE 0x0006

#define THC_REASON_USER 0
#define THC_REASON_DURATION 1
#define THC_REASON_SIGNAL 2
#define THC_REASON_ERROR 3
#define THC_REASON_OFF 4

// A chunk is refused past this payload length: no writer makes one this long.
#define THC_PAYLOAD_MAX (64U << 20)

extern const uint8_t thc_magic[THC_MAGIC_LEN];
extern const uint8_t thc_end_magic[THC_MAGIC_LEN];

// A growable byte buffer.
struct thc_buf {
    uint8_t *data;
    size_t len;
    size_t cap;
};

// The capture info item, read back. The sets point into the item.
struct thc_info {
    uint32_t rate;
    uint32_t c;
    uint16_t channels;
    uint8_t data_channels;
    size_t set_size;
    const uint8_t *enabled;
    const uint8_t *split;
};

/*
 * A header as read. The items found point into raw; thc_header_next_item() walks all of
 * them, configurations and notes included.
 */
struct thc_header {
    uint16_t major;
    uint16_t minor;
    uint32_t length;
    uint8_t spec_major;
    uint8_t spec_minor;
    uint64_t start_time;
    uint64_t first_grid;
    uint32_t crc;
    uint8_t *raw;

    const uint8_t *hello;
    size_t hello_len;
    const uint8_t *info;
    size_t info_len;
    const uint8_t *loss_base;
    size_t loss_base_len;
    const uint8_t *binding;
    size_t binding_len;
    const uint8_t *host;
    size_t host_len;
};

// A chunk as read. payload is valid until the next read.
struct thc_chunk {
    uint8_t type;
    uint8_t flags;
    uint16_t channel;
    uint32_t len;
    uint64_t time;
    uint64_t offset;
    const uint8_t *payload;
};

/*
 * The END chunk's values. data has data_channels entries (only bytes, records, heartbeats,
 * crc_errors, gap_errors and resyncs are stored), records and lost have n.
 */
struct thc_trailer {
    uint64_t time;
    uint64_t last_tick;
    uint8_t reason;
    uint8_t data_channels;
    uint16_t n;
    uint64_t loss_reports;
    uint64_t mismatches;
    uint64_t excused;
    uint64_t overflow;
    struct data_stats *data;
    uint64_t *records;
    uint64_t *lost;
};

/*
 * Where reading stopped: at the END chunk, at the end of a file without one, inside a chunk
 * cut short, or at a chunk that failed its CRC.
 */
enum thc_state {
    THC_READING,
    THC_ENDED,
    THC_EOF,
    THC_CUT,
    THC_BAD_CRC,
};

struct thc_reader;


uint32_t thc_crc32(uint32_t crc, const uint8_t *data, size_t len);

int thc_buf_put(struct thc_buf *b, const void *data, size_t len);
int thc_buf_le(struct thc_buf *b, uint64_t v, size_t n);
void thc_buf_free(struct thc_buf *b);
uint64_t thc_get_le(const uint8_t *p, size_t n);

int thc_header_begin(struct thc_buf *b, uint8_t spec_major, uint8_t spec_minor,
                     uint64_t start_time, uint64_t first_grid);
int thc_header_item(struct thc_buf *b, uint16_t type, const void *value, size_t len);
int thc_header_end(struct thc_buf *b, uint32_t *crc);
int thc_info_item(struct thc_buf *b, const struct capture_info *info);
int thc_info_parse(const uint8_t *item, size_t len, struct thc_info *info);
int thc_chunk_put(struct thc_buf *b, uint8_t type, uint8_t flags, uint16_t channel,
                  uint64_t time, const void *payload, size_t len);
int thc_trailer_payload(struct thc_buf *b, const struct thc_trailer *t);
int thc_footer_put(struct thc_buf *b, uint64_t end_offset);

int thc_trailer_alloc(struct thc_trailer *t, uint8_t data_channels, uint16_t n);
void thc_trailer_free(struct thc_trailer *t);

int thc_reader_open(const char *path, struct thc_reader **out);
void thc_reader_close(struct thc_reader *r);
const struct thc_header *thc_reader_header(const struct thc_reader *r);
bool thc_reader_complete(const struct thc_reader *r);
const struct thc_trailer *thc_reader_trailer(const struct thc_reader *r);
int thc_reader_next(struct thc_reader *r, struct thc_chunk *c);
enum thc_state thc_reader_state(const struct thc_reader *r);
int thc_reader_resume(struct thc_reader *r);
uint64_t thc_reader_offset(const struct thc_reader *r);
int thc_header_next_item(const struct thc_header *h, size_t *at, uint16_t *type,
                         const uint8_t **value, size_t *len);

#endif
