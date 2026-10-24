#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "capture/thcfile.h"
#include "common/capture_info.h"
#include "common/status.h"


#define CRC32_POLY 0xEDB88320U

struct thc_reader {
    FILE *f;
    struct thc_header header;
    bool complete;
    struct thc_trailer trailer;
    enum thc_state state;
    uint64_t offset;
    uint8_t *buf;
    size_t cap;
};


static int read_at(FILE *f, uint64_t offset, void *data, size_t len);
static int header_parse(struct thc_reader *r);
static int footer_check(struct thc_reader *r);
static int trailer_parse(const uint8_t *p, size_t len, struct thc_trailer *t);
static int ensure(struct thc_reader *r, size_t len);


const uint8_t thc_magic[THC_MAGIC_LEN] = {0x8E, 0x54, 0x48, 0x43, 0x0D, 0x0A, 0x1A, 0x0A};
const uint8_t thc_end_magic[THC_MAGIC_LEN] = {0x8E, 0x54, 0x48, 0x43, 0x45, 0x4E, 0x44, 0x0A};


// CRC-32/ISO-HDLC, as zlib's crc32(): start with crc 0 and chain the result.
uint32_t thc_crc32(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1) ? (crc >> 1) ^ CRC32_POLY : crc >> 1;
        }
    }

    return ~crc;
}


int thc_buf_put(struct thc_buf *b, const void *data, size_t len)
{
    if (b->len + len > b->cap) {
        size_t grown = b->cap ? 2 * b->cap : 4096;
        uint8_t *bigger = NULL;

        while (grown < b->len + len) {
            grown *= 2;
        }
        bigger = realloc(b->data, grown);
        if (!bigger) {
            return -ENOMEM;
        }
        b->data = bigger;
        b->cap = grown;
    }
    if (len) {
        memcpy(&b->data[b->len], data, len);
    }
    b->len += len;

    return 0;
}


// v as n bytes, little endian.
int thc_buf_le(struct thc_buf *b, uint64_t v, size_t n)
{
    uint8_t bytes[8];

    for (size_t i = 0; i < n; i++) {
        bytes[i] = (uint8_t)(v >> (8 * i));
    }

    return thc_buf_put(b, bytes, n);
}


void thc_buf_free(struct thc_buf *b)
{
    free(b->data);
    memset(b, 0, sizeof(*b));
}


uint64_t thc_get_le(const uint8_t *p, size_t n)
{
    uint64_t v = 0;

    for (size_t i = 0; i < n; i++) {
        v |= (uint64_t)p[i] << (8 * i);
    }

    return v;
}


// Starts a header in an empty buffer: the fixed part, with the length filled in at the end.
int thc_header_begin(struct thc_buf *b, uint8_t spec_major, uint8_t spec_minor,
                     uint64_t start_time, uint64_t first_grid)
{
    int err = thc_buf_put(b, thc_magic, THC_MAGIC_LEN);

    err = err ? err : thc_buf_le(b, THC_MAJOR, 2);
    err = err ? err : thc_buf_le(b, THC_MINOR, 2);
    err = err ? err : thc_buf_le(b, 0, 4);
    err = err ? err : thc_buf_le(b, spec_major, 1);
    err = err ? err : thc_buf_le(b, spec_minor, 1);
    err = err ? err : thc_buf_le(b, 0, 2);
    err = err ? err : thc_buf_le(b, 0, 4);
    err = err ? err : thc_buf_le(b, start_time, 8);
    err = err ? err : thc_buf_le(b, first_grid, 8);

    return err;
}


int thc_header_item(struct thc_buf *b, uint16_t type, const void *value, size_t len)
{
    int err = thc_buf_le(b, type, 2);

    err = err ? err : thc_buf_le(b, len, 4);
    err = err ? err : thc_buf_put(b, value, len);

    return err;
}


// Fills in the header length and appends the header CRC, also returned in crc.
int thc_header_end(struct thc_buf *b, uint32_t *crc)
{
    uint64_t length = b->len + 4;

    if (length > UINT32_MAX) {
        return -E2BIG;
    }
    for (size_t i = 0; i < 4; i++) {
        b->data[12 + i] = (uint8_t)(length >> (8 * i));
    }
    *crc = thc_crc32(0, b->data, b->len);

    return thc_buf_le(b, *crc, 4);
}


int thc_info_item(struct thc_buf *b, const struct capture_info *info)
{
    struct thc_buf item = {0};
    int err = thc_buf_le(&item, info->rate, 4);

    err = err ? err : thc_buf_le(&item, info->c, 4);
    err = err ? err : thc_buf_le(&item, info->channels, 2);
    err = err ? err : thc_buf_le(&item, info->data_channels, 1);
    err = err ? err : thc_buf_le(&item, 0, 1);
    err = err ? err : thc_buf_put(&item, info->enabled_set, info->set_size);
    err = err ? err : thc_buf_put(&item, info->split_set, info->set_size);
    err = err ? err : thc_header_item(b, THC_ITEM_INFO, item.data, item.len);
    thc_buf_free(&item);

    return err;
}


int thc_info_parse(const uint8_t *item, size_t len, struct thc_info *info)
{
    if (len < 12) {
        return -EPROTO;
    }
    info->rate = (uint32_t)thc_get_le(item, 4);
    info->c = (uint32_t)thc_get_le(&item[4], 4);
    info->channels = (uint16_t)thc_get_le(&item[8], 2);
    info->data_channels = item[10];
    info->set_size = ((size_t)info->channels + 7) / 8;
    if (len < 12 + 2 * info->set_size) {
        return -EPROTO;
    }
    info->enabled = &item[12];
    info->split = &item[12 + info->set_size];

    return 0;
}


int thc_chunk_put(struct thc_buf *b, uint8_t type, uint8_t flags, uint16_t channel,
                  uint64_t time, const void *payload, size_t len)
{
    size_t at = b->len;
    int err = 0;

    if (len > THC_PAYLOAD_MAX) {
        return -E2BIG;
    }
    err = thc_buf_le(b, type, 1);
    err = err ? err : thc_buf_le(b, flags, 1);
    err = err ? err : thc_buf_le(b, channel, 2);
    err = err ? err : thc_buf_le(b, len, 4);
    err = err ? err : thc_buf_le(b, time, 8);
    err = err ? err : thc_buf_put(b, payload, len);
    err = err ? err : thc_buf_le(b, thc_crc32(0, &b->data[at], b->len - at), 4);

    return err;
}


// The END chunk's payload from t.
int thc_trailer_payload(struct thc_buf *b, const struct thc_trailer *t)
{
    int err = thc_buf_le(b, t->last_tick, 8);

    err = err ? err : thc_buf_le(b, t->reason, 1);
    err = err ? err : thc_buf_le(b, t->data_channels, 1);
    err = err ? err : thc_buf_le(b, t->n, 2);
    err = err ? err : thc_buf_le(b, 0, 4);
    err = err ? err : thc_buf_le(b, t->loss_reports, 8);
    err = err ? err : thc_buf_le(b, t->mismatches, 8);
    err = err ? err : thc_buf_le(b, t->excused, 8);
    err = err ? err : thc_buf_le(b, t->overflow, 8);
    for (unsigned int d = 0; d < t->data_channels && !err; d++) {
        const struct data_stats *s = &t->data[d];

        err = thc_buf_le(b, s->bytes, 8);
        err = err ? err : thc_buf_le(b, s->records, 8);
        err = err ? err : thc_buf_le(b, s->heartbeats, 8);
        err = err ? err : thc_buf_le(b, s->crc_errors, 8);
        err = err ? err : thc_buf_le(b, s->gap_errors, 8);
        err = err ? err : thc_buf_le(b, s->resyncs, 8);
    }
    for (unsigned int i = 0; i < t->n && !err; i++) {
        err = thc_buf_le(b, t->records[i], 8);
        err = err ? err : thc_buf_le(b, t->lost[i], 8);
    }

    return err;
}


int thc_footer_put(struct thc_buf *b, uint64_t end_offset)
{
    int err = thc_buf_le(b, end_offset, 8);

    return err ? err : thc_buf_put(b, thc_end_magic, THC_MAGIC_LEN);
}


int thc_trailer_alloc(struct thc_trailer *t, uint8_t data_channels, uint16_t n)
{
    memset(t, 0, sizeof(*t));
    t->data_channels = data_channels;
    t->n = n;
    t->data = calloc(data_channels ? data_channels : 1, sizeof(*t->data));
    t->records = calloc(n ? n : 1, sizeof(*t->records));
    t->lost = calloc(n ? n : 1, sizeof(*t->lost));
    if (!t->data || !t->records || !t->lost) {
        thc_trailer_free(t);
        return -ENOMEM;
    }

    return 0;
}


void thc_trailer_free(struct thc_trailer *t)
{
    free(t->data);
    free(t->records);
    free(t->lost);
    memset(t, 0, sizeof(*t));
}


/*
 * Opens a capture file and reads its header. Returns -ENOENT and the like from fopen(),
 * -EPROTO for a file that is not a capture file or whose header is damaged, -ENOTSUP for a
 * format major this reader does not know, -ENOMEM if out of memory.
 */
int thc_reader_open(const char *path, struct thc_reader **out)
{
    struct thc_reader *r = calloc(1, sizeof(*r));
    int err = 0;

    *out = NULL;
    if (!r) {
        return -ENOMEM;
    }
    r->f = fopen(path, "rb");
    if (!r->f) {
        err = -errno;
        free(r);
        return err;
    }

    err = header_parse(r);
    if (err == 0) {
        r->complete = footer_check(r) == 0;
        r->offset = r->header.length;
        r->state = THC_READING;
        if (fseeko(r->f, (off_t)r->offset, SEEK_SET) == 0) {
            *out = r;
            return 0;
        }
        err = -errno;
    }

    thc_reader_close(r);
    return err;
}


void thc_reader_close(struct thc_reader *r)
{
    if (!r) {
        return;
    }
    if (r->f) {
        fclose(r->f);
    }
    free(r->header.raw);
    thc_trailer_free(&r->trailer);
    free(r->buf);
    free(r);
}


const struct thc_header *thc_reader_header(const struct thc_reader *r)
{
    return &r->header;
}


// The file ends with a footer that points at a valid END chunk.
bool thc_reader_complete(const struct thc_reader *r)
{
    return r->complete;
}


// The END chunk's values, for a complete file; NULL otherwise.
const struct thc_trailer *thc_reader_trailer(const struct thc_reader *r)
{
    return r->complete ? &r->trailer : NULL;
}


/*
 * Reads the next chunk, in file order. Returns 1 with a chunk, or 0 at the end: the END
 * chunk (not returned), the end of the file, a chunk cut short, or one that fails its CRC.
 * The state says which; every chunk returned before it is valid. Returns -ENOMEM if out
 * of memory.
 */
int thc_reader_next(struct thc_reader *r, struct thc_chunk *c)
{
    uint8_t head[THC_CHUNK_HEAD];
    uint32_t len = 0;
    uint32_t crc = 0;
    size_t got = 0;
    int err = 0;

    if (r->state != THC_READING) {
        return 0;
    }

    got = fread(head, 1, sizeof(head), r->f);
    if (got < sizeof(head)) {
        r->state = got == 0 ? THC_EOF : THC_CUT;
        return 0;
    }
    len = (uint32_t)thc_get_le(&head[4], 4);
    if (len > THC_PAYLOAD_MAX) {
        r->state = THC_BAD_CRC;
        return 0;
    }
    err = ensure(r, THC_CHUNK_HEAD + (size_t)len + THC_CHUNK_CRC);
    if (err < 0) {
        return err;
    }
    memcpy(r->buf, head, sizeof(head));
    got = fread(&r->buf[THC_CHUNK_HEAD], 1, (size_t)len + THC_CHUNK_CRC, r->f);
    if (got < (size_t)len + THC_CHUNK_CRC) {
        r->state = THC_CUT;
        return 0;
    }
    crc = (uint32_t)thc_get_le(&r->buf[THC_CHUNK_HEAD + len], 4);
    if (crc != thc_crc32(0, r->buf, THC_CHUNK_HEAD + (size_t)len)) {
        r->state = THC_BAD_CRC;
        return 0;
    }

    c->type = head[0];
    c->flags = head[1];
    c->channel = (uint16_t)thc_get_le(&head[2], 2);
    c->len = len;
    c->time = thc_get_le(&head[8], 8);
    c->offset = r->offset;
    c->payload = &r->buf[THC_CHUNK_HEAD];
    r->offset += THC_CHUNK_HEAD + (uint64_t)len + THC_CHUNK_CRC;

    if (c->type == THC_END) {
        r->state = THC_ENDED;
        return 0;
    }

    return 1;
}


enum thc_state thc_reader_state(const struct thc_reader *r)
{
    return r->state;
}


/*
 * For a file still being written: reading stopped at its end, or inside a chunk not all
 * written yet, and goes on from there with the next thc_reader_next(). -EINVAL in any other
 * state.
 */
int thc_reader_resume(struct thc_reader *r)
{
    if (r->state != THC_EOF && r->state != THC_CUT) {
        return -EINVAL;
    }
    clearerr(r->f);
    if (fseeko(r->f, (off_t)r->offset, SEEK_SET) != 0) {
        return -errno;
    }
    r->state = THC_READING;

    return 0;
}


// The file offset of the next chunk to read.
uint64_t thc_reader_offset(const struct thc_reader *r)
{
    return r->offset;
}


/*
 * Walks the header items: start with *at = 0. Returns 1 with the next item, 0 after the
 * last one.
 */
int thc_header_next_item(const struct thc_header *h, size_t *at, uint16_t *type,
                         const uint8_t **value, size_t *len)
{
    size_t end = h->length - 4;
    size_t i = *at ? *at : THC_FIXED_LEN;
    uint64_t n = 0;

    if (i + 6 > end) {
        return 0;
    }
    n = thc_get_le(&h->raw[i + 2], 4);
    if (n > end - i - 6) {
        return 0;
    }
    *type = (uint16_t)thc_get_le(&h->raw[i], 2);
    *value = &h->raw[i + 6];
    *len = (size_t)n;
    *at = i + 6 + (size_t)n;

    return 1;
}


static int read_at(FILE *f, uint64_t offset, void *data, size_t len)
{
    if (fseeko(f, (off_t)offset, SEEK_SET) < 0) {
        return -errno;
    }

    return fread(data, 1, len, f) == len ? 0 : -EPROTO;
}


static int header_parse(struct thc_reader *r)
{
    struct thc_header *h = &r->header;
    uint8_t fixed[THC_FIXED_LEN];
    size_t at = 0;
    uint16_t type = 0;
    const uint8_t *value = NULL;
    size_t len = 0;

    if (read_at(r->f, 0, fixed, sizeof(fixed)) < 0 ||
        memcmp(fixed, thc_magic, THC_MAGIC_LEN) != 0) {
        return -EPROTO;
    }
    h->major = (uint16_t)thc_get_le(&fixed[8], 2);
    h->minor = (uint16_t)thc_get_le(&fixed[10], 2);
    h->length = (uint32_t)thc_get_le(&fixed[12], 4);
    h->spec_major = fixed[16];
    h->spec_minor = fixed[17];
    h->start_time = thc_get_le(&fixed[24], 8);
    h->first_grid = thc_get_le(&fixed[32], 8);
    if (h->major != THC_MAJOR) {
        return -ENOTSUP;
    }
    if (h->length < THC_FIXED_LEN + 4 || h->length > THC_PAYLOAD_MAX) {
        return -EPROTO;
    }

    h->raw = malloc(h->length);
    if (!h->raw) {
        return -ENOMEM;
    }
    if (read_at(r->f, 0, h->raw, h->length) < 0) {
        return -EPROTO;
    }
    h->crc = (uint32_t)thc_get_le(&h->raw[h->length - 4], 4);
    if (h->crc != thc_crc32(0, h->raw, h->length - 4)) {
        return -EPROTO;
    }

    while (thc_header_next_item(h, &at, &type, &value, &len) > 0) {
        if (type == THC_ITEM_HELLO) {
            h->hello = value;
            h->hello_len = len;
        } else if (type == THC_ITEM_INFO) {
            h->info = value;
            h->info_len = len;
        } else if (type == THC_ITEM_LOSS_BASE) {
            h->loss_base = value;
            h->loss_base_len = len;
        } else if (type == THC_ITEM_BINDING) {
            h->binding = value;
            h->binding_len = len;
        } else if (type == THC_ITEM_HOST) {
            h->host = value;
            h->host_len = len;
        }
    }
    if (!h->hello || !h->info) {
        return -EPROTO;
    }

    return 0;
}


// Reads the footer and the END chunk it points at; 0 if both are good.
static int footer_check(struct thc_reader *r)
{
    uint8_t footer[THC_FOOTER_LEN];
    uint8_t head[THC_CHUNK_HEAD];
    uint64_t size = 0;
    uint64_t end = 0;
    uint32_t len = 0;
    int err = 0;

    if (fseeko(r->f, 0, SEEK_END) < 0) {
        return -errno;
    }
    size = (uint64_t)ftello(r->f);
    if (size < (uint64_t)r->header.length + THC_FOOTER_LEN) {
        return -ENOENT;
    }
    err = read_at(r->f, size - THC_FOOTER_LEN, footer, sizeof(footer));
    if (err < 0 || memcmp(&footer[8], thc_end_magic, THC_MAGIC_LEN) != 0) {
        return -ENOENT;
    }
    end = thc_get_le(footer, 8);
    if (end < r->header.length || end + THC_CHUNK_HEAD > size - THC_FOOTER_LEN) {
        return -ENOENT;
    }
    if (read_at(r->f, end, head, sizeof(head)) < 0 || head[0] != THC_END) {
        return -ENOENT;
    }
    len = (uint32_t)thc_get_le(&head[4], 4);
    if (end + THC_CHUNK_HEAD + len + THC_CHUNK_CRC != size - THC_FOOTER_LEN) {
        return -ENOENT;
    }

    err = ensure(r, THC_CHUNK_HEAD + (size_t)len + THC_CHUNK_CRC);
    if (err < 0) {
        return err;
    }
    if (read_at(r->f, end, r->buf, THC_CHUNK_HEAD + (size_t)len + THC_CHUNK_CRC) < 0 ||
        thc_get_le(&r->buf[THC_CHUNK_HEAD + len], 4) !=
            thc_crc32(0, r->buf, THC_CHUNK_HEAD + (size_t)len)) {
        return -ENOENT;
    }
    err = trailer_parse(&r->buf[THC_CHUNK_HEAD], len, &r->trailer);
    if (err == 0) {
        r->trailer.time = thc_get_le(&r->buf[8], 8);
    }

    return err;
}


static int trailer_parse(const uint8_t *p, size_t len, struct thc_trailer *t)
{
    uint8_t d_count = 0;
    uint16_t n = 0;
    size_t at = THC_END_FIXED;
    int err = 0;

    if (len < THC_END_FIXED) {
        return -EPROTO;
    }
    d_count = p[9];
    n = (uint16_t)thc_get_le(&p[10], 2);
    if (len < THC_END_FIXED + (size_t)d_count * THC_END_DATA + (size_t)n * THC_END_LOGICAL) {
        return -EPROTO;
    }
    err = thc_trailer_alloc(t, d_count, n);
    if (err < 0) {
        return err;
    }
    t->last_tick = thc_get_le(p, 8);
    t->reason = p[8];
    t->loss_reports = thc_get_le(&p[16], 8);
    t->mismatches = thc_get_le(&p[24], 8);
    t->excused = thc_get_le(&p[32], 8);
    t->overflow = thc_get_le(&p[40], 8);
    for (unsigned int d = 0; d < d_count; d++, at += THC_END_DATA) {
        struct data_stats *s = &t->data[d];

        s->bytes = thc_get_le(&p[at], 8);
        s->records = thc_get_le(&p[at + 8], 8);
        s->heartbeats = thc_get_le(&p[at + 16], 8);
        s->crc_errors = thc_get_le(&p[at + 24], 8);
        s->gap_errors = thc_get_le(&p[at + 32], 8);
        s->resyncs = thc_get_le(&p[at + 40], 8);
    }
    for (unsigned int i = 0; i < n; i++, at += THC_END_LOGICAL) {
        t->records[i] = thc_get_le(&p[at], 8);
        t->lost[i] = thc_get_le(&p[at + 8], 8);
    }

    return 0;
}


static int ensure(struct thc_reader *r, size_t len)
{
    if (len > r->cap) {
        uint8_t *bigger = realloc(r->buf, len);

        if (!bigger) {
            return -ENOMEM;
        }
        r->buf = bigger;
        r->cap = len;
    }

    return 0;
}
