#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "capture/index.h"
#include "capture/thcfile.h"


const uint8_t index_magic[THC_MAGIC_LEN] = {0x8E, 0x54, 0x48, 0x49, 0x0D, 0x0A, 0x1A, 0x0A};


// K: the grid points in about one second, at least 1.
uint32_t index_interval(uint32_t rate, uint32_t c)
{
    uint32_t k = c ? (uint32_t)(((uint64_t)rate + c / 2) / c) : 1;

    return k ? k : 1;
}


// The first grid count after grid that is a multiple of k.
uint64_t index_next_grid(uint64_t grid, uint32_t k)
{
    return (grid / k + 1) * k;
}


// The index file's name: the capture file's with ".index" appended. Free with free().
char *index_path(const char *capture_path)
{
    size_t n = strlen(capture_path);
    char *path = malloc(n + sizeof(".index"));

    if (path) {
        memcpy(path, capture_path, n);
        memcpy(&path[n], ".index", sizeof(".index"));
    }

    return path;
}


int index_builder_init(struct index_builder *ib, uint16_t data_channels, uint32_t k)
{
    memset(ib, 0, sizeof(*ib));
    ib->k = k ? k : 1;
    ib->data_channels = data_channels;
    ib->lane = calloc(data_channels ? data_channels : 1, sizeof(*ib->lane));

    return ib->lane ? 0 : -ENOMEM;
}


void index_builder_free(struct index_builder *ib)
{
    if (ib->lane) {
        for (unsigned int d = 0; d < ib->data_channels; d++) {
            free(ib->lane[d].spans);
        }
        free(ib->lane);
    }
    thc_buf_free(&ib->out);
    memset(ib, 0, sizeof(*ib));
}


// A DATA chunk at file offset chunk carries stream bytes start to start + len - 1.
int index_builder_chunk(struct index_builder *ib, uint16_t channel, uint64_t start,
                        uint64_t len, uint64_t chunk)
{
    struct index_lane *l = NULL;

    if (channel >= ib->data_channels || len == 0) {
        return channel >= ib->data_channels ? -EINVAL : 0;
    }
    l = &ib->lane[channel];
    if (l->head > 0 && l->head == l->count) {
        l->head = 0;
        l->count = 0;
    }
    if (l->count == l->cap) {
        if (l->head > 0) {
            memmove(l->spans, &l->spans[l->head], (l->count - l->head) * sizeof(*l->spans));
            l->count -= l->head;
            l->head = 0;
        } else {
            size_t grown = l->cap ? 2 * l->cap : 16;
            struct index_span *bigger = realloc(l->spans, grown * sizeof(*bigger));

            if (!bigger) {
                return -ENOMEM;
            }
            l->spans = bigger;
            l->cap = grown;
        }
    }
    l->spans[l->count++] = (struct index_span){.start = start, .len = len, .chunk = chunk};

    return 0;
}


/*
 * A heartbeat that checked good, at grid count grid, its first byte at stream offset pos.
 * Makes an entry if it is the first at or after the next multiple of K and its chunk is
 * known; a heartbeat in bytes that never reached the file is skipped.
 */
int index_builder_heartbeat(struct index_builder *ib, uint16_t channel, uint64_t grid,
                            uint64_t pos)
{
    struct index_lane *l = NULL;
    const struct index_span *s = NULL;
    int err = 0;

    if (channel >= ib->data_channels) {
        return -EINVAL;
    }
    l = &ib->lane[channel];
    if (grid < l->next_grid) {
        return 0;
    }

    // Heartbeats come in stream order: spans before this one are done with.
    while (l->head < l->count && l->spans[l->head].start + l->spans[l->head].len <= pos) {
        l->head++;
    }
    if (l->head == l->count || l->spans[l->head].start > pos) {
        return 0;
    }
    s = &l->spans[l->head];

    err = thc_buf_le(&ib->out, grid, 8);
    err = err ? err : thc_buf_le(&ib->out, s->chunk, 8);
    err = err ? err : thc_buf_le(&ib->out, pos - s->start, 4);
    err = err ? err : thc_buf_le(&ib->out, channel, 2);
    err = err ? err : thc_buf_le(&ib->out, 0, 2);
    if (err < 0) {
        return err;
    }
    ib->entries++;
    l->next_grid = index_next_grid(grid, ib->k);

    return 0;
}


int index_header(struct thc_buf *b, uint32_t header_crc, uint64_t start_time, uint32_t k)
{
    int err = thc_buf_put(b, index_magic, THC_MAGIC_LEN);

    err = err ? err : thc_buf_le(b, THC_MAJOR, 2);
    err = err ? err : thc_buf_le(b, THC_MINOR, 2);
    err = err ? err : thc_buf_le(b, header_crc, 4);
    err = err ? err : thc_buf_le(b, start_time, 8);
    err = err ? err : thc_buf_le(b, k, 4);
    err = err ? err : thc_buf_le(b, 0, 4);

    return err;
}


/*
 * Whether the index file at path belongs to the capture file with this header CRC and start
 * time: 0 if it does, -ENOENT if there is none, -ESTALE if it is unreadable or another's.
 */
int index_check(const char *path, uint32_t header_crc, uint64_t start_time)
{
    uint8_t head[INDEX_HEADER_LEN];
    FILE *f = fopen(path, "rb");
    bool ok = false;

    if (!f) {
        return errno == ENOENT ? -ENOENT : -ESTALE;
    }
    ok = fread(head, 1, sizeof(head), f) == sizeof(head) &&
         memcmp(head, index_magic, THC_MAGIC_LEN) == 0 &&
         thc_get_le(&head[8], 2) == THC_MAJOR && thc_get_le(&head[12], 4) == header_crc &&
         thc_get_le(&head[16], 8) == start_time;
    fclose(f);

    return ok ? 0 : -ESTALE;
}


/*
 * Reads an index file's interval and entries; an incomplete last entry is left out. Free
 * the entries with free().
 */
int index_read(const char *path, uint32_t *k, struct index_entry **entries, size_t *count)
{
    uint8_t head[INDEX_HEADER_LEN];
    uint8_t e[INDEX_ENTRY_LEN];
    struct index_entry *list = NULL;
    size_t n = 0;
    size_t cap = 0;
    FILE *f = fopen(path, "rb");
    int err = 0;

    *entries = NULL;
    *count = 0;
    if (!f) {
        return -errno;
    }
    if (fread(head, 1, sizeof(head), f) != sizeof(head) ||
        memcmp(head, index_magic, THC_MAGIC_LEN) != 0) {
        fclose(f);
        return -EPROTO;
    }
    *k = (uint32_t)thc_get_le(&head[24], 4);

    while (fread(e, 1, sizeof(e), f) == sizeof(e)) {
        if (n == cap) {
            size_t grown = cap ? 2 * cap : 64;
            struct index_entry *bigger = realloc(list, grown * sizeof(*bigger));

            if (!bigger) {
                err = -ENOMEM;
                break;
            }
            list = bigger;
            cap = grown;
        }
        list[n++] = (struct index_entry){
            .grid = thc_get_le(e, 8),
            .chunk = thc_get_le(&e[8], 8),
            .payload = (uint32_t)thc_get_le(&e[16], 4),
            .channel = (uint16_t)thc_get_le(&e[20], 2),
        };
    }
    fclose(f);
    if (err < 0) {
        free(list);
        return err;
    }
    *entries = list;
    *count = n;

    return 0;
}
