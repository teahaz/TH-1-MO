#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <bits/pthreadtypes.h>
#include <bits/time.h>
#include <bits/types/struct_timespec.h>

#include "capture/index.h"
#include "capture/recorder.h"
#include "capture/thcfile.h"


#define OVERFLOW_LEN (2 + 8 + 4)

enum item_kind {
    ITEM_HEADER,
    ITEM_CHUNK,
    ITEM_MARK,
    ITEM_END,
};

/*
 * A queued piece of the file. A DATA chunk's stream offset is in a; a heartbeat to index
 * has its grid count in a and stream offset in b.
 */
struct item {
    struct item *next;
    uint8_t kind;
    uint8_t type;
    uint8_t flags;
    uint16_t channel;
    uint64_t time;
    uint64_t a;
    uint64_t b;
    size_t len;
    uint8_t data[];
};

/*
 * The fields under "caller" belong to the thread making the calls, the ones under "writer"
 * to the writer thread, and the rest are shared under lock.
 */
struct recorder {
    int fd;
    int index_fd;
    char *path;
    char *index_path;
    pthread_t thread;
    bool begun;

    pthread_mutex_t lock;
    pthread_cond_t cond;
    struct item *head;
    struct item *tail;
    size_t queued;
    bool closing;
    int error;
    uint64_t written;
    uint64_t index_entries;

    // Caller.
    uint16_t data_channels;
    uint32_t k;
    uint64_t *stream;
    uint64_t *dropped;
    uint64_t *next_grid;
    uint32_t dropped_ctrl;
    uint64_t overflow_bytes;
    uint64_t overflow_events;

    // Writer.
    struct thc_buf batch;
    struct index_builder ib;
    uint64_t offset;
    bool flushed;
};


static void *writer_main(void *arg);
static int serialize(struct recorder *r, struct item *it);
static int flush(struct recorder *r);
static int write_all(int fd, const uint8_t *data, size_t len);
static struct item *item_new(uint8_t kind, uint8_t type, uint8_t flags, uint16_t channel,
                             uint64_t time, const void *data, size_t len);
static struct item *overflow_item(struct recorder *r, uint16_t channel, uint64_t time);
static bool push(struct recorder *r, struct item *first, struct item *second, bool droppable);
static uint64_t mono_ms(void);


/*
 * Creates the capture file at path, which must not exist yet (a writer never appends), and
 * its index file beside it. Nothing is written until recorder_begin(). Returns -EEXIST if
 * the file exists, other errors from open(), -ENOMEM if out of memory.
 */
int recorder_open(struct recorder **out, const char *path)
{
    struct recorder *r = calloc(1, sizeof(*r));
    pthread_condattr_t attr;
    int err = 0;

    *out = NULL;
    if (!r) {
        return -ENOMEM;
    }
    r->fd = -1;
    r->index_fd = -1;
    r->path = strdup(path);
    r->index_path = index_path(path);
    if (!r->path || !r->index_path) {
        err = -ENOMEM;
        goto fail;
    }

    r->fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (r->fd < 0) {
        err = -errno;
        goto fail;
    }
    r->index_fd = open(r->index_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (r->index_fd < 0) {
        err = -errno;
        close(r->fd);
        r->fd = -1;
        unlink(path);
        goto fail;
    }

    pthread_mutex_init(&r->lock, NULL);
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&r->cond, &attr);
    pthread_condattr_destroy(&attr);
    *out = r;

    return 0;

fail:
    free(r->path);
    free(r->index_path);
    free(r);
    return err;
}


int recorder_begin(struct recorder *r, const struct recorder_header *h)
{
    struct thc_buf hdr = {0};
    struct item *it = NULL;
    uint32_t crc = 0;
    uint8_t spec_major = h->hello_len > 2 ? h->hello[1] : 0;
    uint8_t spec_minor = h->hello_len > 2 ? h->hello[2] : 0;
    int err = 0;

    r->data_channels = h->info->data_channels;
    r->k = index_interval(h->info->rate, h->info->c);
    r->stream = calloc(r->data_channels, sizeof(*r->stream));
    r->dropped = calloc(r->data_channels, sizeof(*r->dropped));
    r->next_grid = calloc(r->data_channels, sizeof(*r->next_grid));
    if (!r->stream || !r->dropped || !r->next_grid) {
        return -ENOMEM;
    }
    err = index_builder_init(&r->ib, r->data_channels, r->k);
    if (err < 0) {
        return err;
    }

    err = thc_header_begin(&hdr, spec_major, spec_minor, h->start_time, h->first_grid);
    err = err ? err : thc_header_item(&hdr, THC_ITEM_HELLO, h->hello, h->hello_len);
    for (size_t i = 0; i < h->config_count && err == 0; i++) {
        err = thc_header_item(&hdr, h->configs[i].type, h->configs[i].data, h->configs[i].len);
    }
    err = err ? err : thc_info_item(&hdr, h->info);
    if (err == 0 && h->binding) {
        err = thc_header_item(&hdr, THC_ITEM_BINDING, h->binding, strlen(h->binding));
    }
    if (err == 0 && h->host) {
        err = thc_header_item(&hdr, THC_ITEM_HOST, h->host, strlen(h->host));
    }
    if (err == 0 && h->loss_base) {
        err = thc_header_item(&hdr, THC_ITEM_LOSS_BASE, h->loss_base, h->loss_base_len);
    }
    err = err ? err : thc_header_end(&hdr, &crc);
    // The index header goes first in the index's output: the writer flushes it with the
    // capture file's header.
    err = err ? err : index_header(&r->ib.out, crc, h->start_time, r->k);
    if (err == 0) {
        it = item_new(ITEM_HEADER, 0, 0, 0, 0, hdr.data, hdr.len);
        err = it ? 0 : -ENOMEM;
    }
    thc_buf_free(&hdr);
    if (err < 0) {
        return err;
    }

    if (!push(r, it, NULL, false)) {
        free(it);
        return -EIO;
    }
    err = -pthread_create(&r->thread, NULL, writer_main, r);
    if (err < 0) {
        return err;
    }
    r->begun = true;

    return 0;
}


/*
 * Bytes read on data channel channel, at time (nanoseconds from the recording's start).
 * Dropped if storage is too far behind; the next bytes that fit carry an overflow event.
 */
void recorder_data(struct recorder *r, uint16_t channel, uint64_t time, const uint8_t *data,
                   size_t len)
{
    struct item *ev = NULL;
    struct item *it = NULL;

    if (channel >= r->data_channels || len == 0) {
        return;
    }
    if (r->dropped[channel] || r->dropped_ctrl) {
        ev = overflow_item(r, channel, time);
    }
    it = item_new(ITEM_CHUNK, THC_DATA, 0, channel, time, data, len);
    if (it) {
        it->a = r->stream[channel];
    }
    r->stream[channel] += len;

    if (it && push(r, ev, it, true)) {
        if (ev) {
            r->dropped[channel] = 0;
            r->dropped_ctrl = 0;
            r->overflow_events++;
        }
        return;
    }
    free(ev);
    free(it);
    r->dropped[channel] += len;
    r->overflow_bytes += len;
}


// A control message; flags has THC_TO_DEVICE for one the host sent.
void recorder_control(struct recorder *r, uint8_t flags, uint64_t time, const uint8_t *msg,
                      size_t len)
{
    struct item *ev = r->dropped_ctrl ? overflow_item(r, 0, time) : NULL;
    struct item *it = item_new(ITEM_CHUNK, THC_CONTROL, flags, 0, time, msg, len);

    if (it && push(r, ev, it, true)) {
        if (ev) {
            r->dropped_ctrl = 0;
            r->dropped[0] = 0;
            r->overflow_events++;
        }
        return;
    }
    free(ev);
    free(it);
    r->dropped_ctrl++;
}


// An event: code, then data (docs/capture-file.md, EVENT). Events are never dropped.
void recorder_event(struct recorder *r, uint16_t code, uint16_t channel, uint64_t time,
                    const uint8_t *data, size_t len)
{
    struct item *it = item_new(ITEM_CHUNK, THC_EVENT, 0, channel, time, NULL, 2 + len);

    if (!it) {
        return;
    }
    it->data[0] = (uint8_t)code;
    it->data[1] = (uint8_t)(code >> 8);
    if (len) {
        memcpy(&it->data[2], data, len);
    }
    if (!push(r, it, NULL, false)) {
        free(it);
    }
}


/*
 * A heartbeat that checked good on data channel channel: grid count grid, its first byte at
 * stream offset pos. One is indexed every K grid points.
 */
void recorder_heartbeat(struct recorder *r, uint16_t channel, uint64_t grid, uint64_t pos)
{
    struct item *it = NULL;

    if (channel >= r->data_channels || grid < r->next_grid[channel]) {
        return;
    }
    r->next_grid[channel] = index_next_grid(grid, r->k);
    it = item_new(ITEM_MARK, 0, 0, channel, 0, NULL, 0);
    if (it) {
        it->a = grid;
        it->b = pos;
        if (!push(r, it, NULL, true)) {
            free(it);
        }
    }
}


/*
 * Ends the recording: the END chunk from t (its overflow count is filled in here) and the
 * footer, everything flushed and synced, and the files closed. Frees r. Returns the
 * recording's first error, 0 if there was none.
 */
int recorder_close(struct recorder *r, uint64_t time, struct thc_trailer *t)
{
    struct thc_buf payload = {0};
    struct item *it = NULL;
    int err = 0;

    if (r->begun) {
        t->overflow = r->overflow_bytes;
        if (thc_trailer_payload(&payload, t) == 0) {
            it = item_new(ITEM_END, THC_END, 0, 0, time, payload.data, payload.len);
        }
        thc_buf_free(&payload);
        if (it && !push(r, it, NULL, false)) {
            free(it);
            it = NULL;
        }

        pthread_mutex_lock(&r->lock);
        r->closing = true;
        pthread_cond_signal(&r->cond);
        pthread_mutex_unlock(&r->lock);
        pthread_join(r->thread, NULL);
        err = r->error ? r->error : it ? 0 : -ENOMEM;
    } else {
        // Nothing was recorded: no file is left behind.
        unlink(r->path);
        unlink(r->index_path);
    }

    if (r->fd >= 0) {
        close(r->fd);
    }
    if (r->index_fd >= 0) {
        close(r->index_fd);
    }
    pthread_mutex_destroy(&r->lock);
    pthread_cond_destroy(&r->cond);
    thc_buf_free(&r->batch);
    index_builder_free(&r->ib);
    free(r->stream);
    free(r->dropped);
    free(r->next_grid);
    free(r->path);
    free(r->index_path);
    free(r);

    return err;
}


// The first write or sync error, as a negative errno; 0 while the recording is fine.
int recorder_error(struct recorder *r)
{
    int err = 0;

    pthread_mutex_lock(&r->lock);
    err = r->error;
    pthread_mutex_unlock(&r->lock);

    return err;
}


void recorder_stats(struct recorder *r, struct recorder_stats *st)
{
    pthread_mutex_lock(&r->lock);
    st->written = r->written;
    st->queued = r->queued;
    st->index_entries = r->index_entries;
    pthread_mutex_unlock(&r->lock);
    st->overflow_bytes = r->overflow_bytes;
    st->overflow_events = r->overflow_events;
}


static void *writer_main(void *arg)
{
    struct recorder *r = arg;
    uint64_t last_flush = mono_ms();
    uint64_t last_sync = last_flush;
    bool done = false;

    while (!done) {
        struct item *list = NULL;
        size_t taken = 0;
        bool closing = false;
        uint64_t now = 0;
        int err = 0;

        pthread_mutex_lock(&r->lock);
        while (!r->head && !r->closing) {
            struct timespec ts;
            uint64_t due = (r->batch.len ? last_flush + RECORDER_FLUSH_MS :
                                           last_sync + RECORDER_SYNC_MS);

            if (due <= mono_ms()) {
                break;
            }
            ts.tv_sec = (time_t)(due / 1000);
            ts.tv_nsec = (long)(due % 1000) * 1000000;
            pthread_cond_timedwait(&r->cond, &r->lock, &ts);
        }
        list = r->head;
        r->head = NULL;
        r->tail = NULL;
        closing = r->closing;
        err = r->error;
        pthread_mutex_unlock(&r->lock);

        while (list) {
            struct item *next = list->next;

            if (err == 0) {
                err = serialize(r, list);
            }
            taken += sizeof(*list) + list->len;
            free(list);
            list = next;
        }

        now = mono_ms();
        if (err == 0 && (r->batch.len >= RECORDER_BATCH_MIN || !r->flushed || closing ||
                         now - last_flush >= RECORDER_FLUSH_MS)) {
            err = flush(r);
            last_flush = now;
        }
        if (err == 0 && (closing || now - last_sync >= RECORDER_SYNC_MS)) {
            err = fsync(r->fd) < 0 || fsync(r->index_fd) < 0 ? -errno : 0;
            last_sync = now;
        }

        pthread_mutex_lock(&r->lock);
        r->queued -= taken;
        if (err < 0 && r->error == 0) {
            r->error = err;
        }
        r->written = r->offset;
        r->index_entries = r->ib.entries;
        done = closing && !r->head;
        pthread_mutex_unlock(&r->lock);
    }

    return NULL;
}


static int serialize(struct recorder *r, struct item *it)
{
    uint64_t at = r->offset + r->batch.len;
    int err = 0;

    switch (it->kind) {
    case ITEM_HEADER:
        return thc_buf_put(&r->batch, it->data, it->len);
    case ITEM_MARK:
        return index_builder_heartbeat(&r->ib, it->channel, it->a, it->b);
    case ITEM_END:
        err = thc_chunk_put(&r->batch, THC_END, 0, 0, it->time, it->data, it->len);
        return err ? err : thc_footer_put(&r->batch, at);
    default:
        if (it->type == THC_DATA) {
            err = index_builder_chunk(&r->ib, it->channel, it->a, it->len, at);
        }
        return err ? err : thc_chunk_put(&r->batch, it->type, it->flags, it->channel, it->time,
                                         it->data, it->len);
    }
}


static int flush(struct recorder *r)
{
    int err = write_all(r->fd, r->batch.data, r->batch.len);

    if (err == 0) {
        r->offset += r->batch.len;
        r->batch.len = 0;
        err = write_all(r->index_fd, r->ib.out.data, r->ib.out.len);
        r->ib.out.len = 0;
    }
    r->flushed = true;

    return err;
}


static int write_all(int fd, const uint8_t *data, size_t len)
{
    while (len) {
        ssize_t n = write(fd, data, len);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -errno;
        }
        data += n;
        len -= (size_t)n;
    }

    return 0;
}


static struct item *item_new(uint8_t kind, uint8_t type, uint8_t flags, uint16_t channel,
                             uint64_t time, const void *data, size_t len)
{
    struct item *it = malloc(sizeof(*it) + len);

    if (!it) {
        return NULL;
    }
    *it = (struct item){
        .kind = kind,
        .type = type,
        .flags = flags,
        .channel = channel,
        .time = time,
        .len = len,
    };
    if (data && len) {
        memcpy(it->data, data, len);
    }

    return it;
}


// The overflow event for what was dropped on channel since the last one.
static struct item *overflow_item(struct recorder *r, uint16_t channel, uint64_t time)
{
    struct item *it = item_new(ITEM_CHUNK, THC_EVENT, 0, channel, time, NULL, OVERFLOW_LEN);
    uint64_t bytes = r->dropped[channel];

    if (!it) {
        return NULL;
    }
    for (size_t i = 0; i < 2; i++) {
        it->data[i] = (uint8_t)(THC_EV_OVERFLOW >> (8 * i));
    }
    for (size_t i = 0; i < 8; i++) {
        it->data[2 + i] = (uint8_t)(bytes >> (8 * i));
    }
    for (size_t i = 0; i < 4; i++) {
        it->data[10 + i] = (uint8_t)(r->dropped_ctrl >> (8 * i));
    }

    return it;
}


/*
 * Queues first and second (either may be NULL) together. A droppable pair is refused when
 * the queue would grow past RECORDER_QUEUE_MAX; after an error everything is refused.
 * Returns whether they were queued; refused items stay the caller's.
 */
static bool push(struct recorder *r, struct item *first, struct item *second, bool droppable)
{
    size_t need = (first ? sizeof(*first) + first->len : 0) +
                  (second ? sizeof(*second) + second->len : 0);
    struct item *items[2] = {first, second};

    pthread_mutex_lock(&r->lock);
    if (r->error || (droppable && r->queued + need > RECORDER_QUEUE_MAX)) {
        pthread_mutex_unlock(&r->lock);
        return false;
    }
    for (size_t i = 0; i < 2; i++) {
        if (!items[i]) {
            continue;
        }
        items[i]->next = NULL;
        if (r->tail) {
            r->tail->next = items[i];
        } else {
            r->head = items[i];
        }
        r->tail = items[i];
    }
    r->queued += need;
    pthread_cond_signal(&r->cond);
    pthread_mutex_unlock(&r->lock);

    return true;
}


static uint64_t mono_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
