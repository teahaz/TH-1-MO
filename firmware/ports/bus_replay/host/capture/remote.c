/*
 * The device through a link (capture/remote.h, docs/link-protocol.md).
 */

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "capture/capture.h"
#include "capture/control.h"
#include "capture/linkproto.h"
#include "capture/recorder.h"
#include "capture/remote.h"
#include "capture/source.h"
#include "capture/thcfile.h"


#define READ_CHUNK 65536

// An item from the link, waiting to be read.
struct queued {
    uint8_t type;
    uint8_t flags;
    uint16_t channel;
    uint64_t time;
    uint8_t *data;
    size_t len;
};

/*
 * in holds bytes read and not parsed yet. Items wait in q (head to head + len - 1); cur is
 * the one read last, kept until the next read. reply is the last reply, for the request
 * waiting on it. base moves link times onto this host's clock.
 */
struct remote {
    int fd;
    bool running;
    bool closed;
    struct thc_buf in;
    size_t in_off;

    struct queued *q;
    size_t head;
    size_t len;
    size_t cap;
    struct queued cur;

    bool have_reply;
    uint8_t reply_type;
    struct thc_buf reply;
    uint64_t base;

    struct recorder_msg *configs;
    size_t config_count;
};


static int remote_hello(void *src, const uint8_t **msg, size_t *len);
static int remote_config(void *src, const uint8_t *msg, size_t len, const uint8_t **reply,
                         size_t *reply_len);
static int remote_read(void *src, struct source_item *item, int timeout_ms);
static int remote_stop(void *src);
static void remote_close(void *src);
static int request(struct remote *r, uint8_t type, const uint8_t *data, size_t len,
                   const uint8_t **payload, size_t *payload_len);
static int receive(struct remote *r, int timeout_ms);
static int take(struct remote *r, const struct link_msg *m);
static void queue_clear(struct remote *r);
static uint64_t now_ns(void);


const struct source_ops remote_ops = {
    .hello = remote_hello,
    .config = remote_config,
    .read = remote_read,
    .stop = remote_stop,
    .close = remote_close,
};


int remote_default_path(char *buf, size_t size)
{
    return link_path(buf, size, NULL);
}


/*
 * Connects to the link and reads its WELCOME. -EBUSY if it serves another client, -EPROTO
 * for a protocol major this host does not know, and the errors of connecting.
 */
int remote_open(void **src, const struct remote_options *opt)
{
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    struct remote *r = calloc(1, sizeof(*r));
    uint64_t until = now_ns() + (uint64_t)REMOTE_TIMEOUT_MS * 1000000;
    int err = 0;

    *src = NULL;
    if (!r) {
        return -ENOMEM;
    }
    err = link_path(addr.sun_path, sizeof(addr.sun_path), opt ? opt->path : NULL);
    r->fd = err < 0 ? -1 : socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (err == 0 && r->fd < 0) {
        err = -errno;
    }
    if (err == 0 && connect(r->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        err = -errno;
    }
    while (err == 0 && !r->have_reply) {
        uint64_t now = now_ns();

        err = now < until ? receive(r, (int)((until - now) / 1000000) + 1) : -ETIMEDOUT;
    }
    if (err == 0 && r->reply_type == LINK_BUSY) {
        err = -EBUSY;
    } else if (err == 0 && (r->reply_type != LINK_WELCOME || r->reply.len < 8 ||
                            thc_get_le(r->reply.data, 2) != LINK_MAJOR)) {
        err = -EPROTO;
    }
    if (err < 0) {
        remote_close(r);
        return err;
    }
    r->running = r->reply.data[4] & LINK_RUNNING;
    r->have_reply = false;
    *src = r;

    return 0;
}


bool remote_running(void *src)
{
    struct remote *r = src;

    return r->running;
}


int remote_attach(void *src, struct capture_attach *a)
{
    struct remote *r = src;
    const uint8_t *p = NULL;
    size_t len = 0;
    size_t off = 0;
    uint64_t count = 0;
    uint64_t n = 0;
    int err = request(r, LINK_ATTACH, NULL, 0, &p, &len);

    if (err < 0) {
        return err;
    }
    memset(a, 0, sizeof(*a));
    if (len < 20) {
        return -EPROTO;
    }
    a->first_grid = thc_get_le(p, 8);
    a->time = thc_get_le(&p[8], 8);
    n = thc_get_le(&p[16], 4);
    off = 20;
    if (n > len - off) {
        return -EPROTO;
    }
    a->hello = &p[off];
    a->hello_len = (size_t)n;
    off += (size_t)n;
    if (len - off < 4) {
        return -EPROTO;
    }
    count = thc_get_le(&p[off], 4);
    off += 4;
    free(r->configs);
    r->configs = calloc(count ? count : 1, sizeof(*r->configs));
    r->config_count = 0;
    if (!r->configs) {
        return -ENOMEM;
    }
    for (uint64_t i = 0; i < count; i++) {
        if (len - off < 6) {
            return -EPROTO;
        }
        n = thc_get_le(&p[off + 2], 4);
        if (n > len - off - 6) {
            return -EPROTO;
        }
        r->configs[r->config_count++] = (struct recorder_msg){
            .type = (uint16_t)thc_get_le(&p[off], 2), .data = &p[off + 6], .len = (size_t)n};
        off += 6 + (size_t)n;
    }
    if (len - off < 4) {
        return -EPROTO;
    }
    n = thc_get_le(&p[off], 4);
    off += 4;
    if (n > len - off) {
        return -EPROTO;
    }
    a->configs = r->configs;
    a->config_count = r->config_count;
    a->loss_base = n ? &p[off] : NULL;
    a->loss_base_len = (size_t)n;
    r->base = now_ns() - a->time;
    r->running = true;

    return 0;
}


int remote_record(void *src, const char *path)
{
    struct remote *r = src;

    return request(r, LINK_RECORD, (const uint8_t *)(path ? path : ""), path ? strlen(path) : 0,
                   NULL, NULL);
}


int remote_status(void *src, struct remote_status *st)
{
    struct remote *r = src;
    const uint8_t *p = NULL;
    size_t len = 0;
    uint64_t n = 0;
    int err = request(r, LINK_STATUS, NULL, 0, &p, &len);

    memset(st, 0, sizeof(*st));
    if (err < 0) {
        return err;
    }
    if (len < 32) {
        return -EPROTO;
    }
    st->running = p[0] & LINK_RUNNING;
    st->recording = p[0] & LINK_RECORDING;
    st->record_error = (int32_t)thc_get_le(&p[4], 4);
    st->recorded = thc_get_le(&p[8], 8);
    st->dropped_bytes = thc_get_le(&p[16], 8);
    st->dropped_control = (uint32_t)thc_get_le(&p[24], 4);
    n = thc_get_le(&p[28], 4);
    if (n > len - 32) {
        return -EPROTO;
    }
    snprintf(st->path, sizeof(st->path), "%.*s", (int)n, (const char *)&p[32]);
    r->running = st->running;

    return 0;
}


// The link resets the device (ending a capture it runs) and gives its hello.
static int remote_hello(void *src, const uint8_t **msg, size_t *len)
{
    struct remote *r = src;
    int err = request(r, LINK_HELLO, NULL, 0, msg, len);

    if (err == 0) {
        r->running = false;
        queue_clear(r);
    }

    return err;
}


// A configuration message; an accepted capture register write starts the stream.
static int remote_config(void *src, const uint8_t *msg, size_t len, const uint8_t **reply,
                         size_t *reply_len)
{
    struct remote *r = src;
    struct config_reply rep;
    int err = request(r, LINK_CONFIG, msg, len, reply, reply_len);

    if (err == 0 && len >= 3 && msg[0] == MSG_CONFIG && msg[1] == REG_CAPTURE && msg[2] == 1 &&
        config_reply_parse(*reply, *reply_len, &rep) == 0 && rep.status == 0) {
        r->base = now_ns();
        r->running = true;
    }

    return err;
}


static int remote_read(void *src, struct source_item *item, int timeout_ms)
{
    struct remote *r = src;
    struct queued *q = NULL;
    int err = 0;

    free(r->cur.data);
    r->cur.data = NULL;
    if (!r->len) {
        err = receive(r, timeout_ms);
        if (err < 0) {
            return err;
        }
    }
    if (!r->len) {
        return 0;
    }
    q = &r->q[r->head];
    r->cur = *q;
    r->head = (r->head + 1) % r->cap;
    r->len--;
    *item = (struct source_item){.type = q->type, .flags = q->flags, .channel = q->channel,
                                 .time = r->base + q->time, .data = r->cur.data,
                                 .len = r->cur.len};

    return 1;
}


static int remote_stop(void *src)
{
    struct remote *r = src;
    int err = request(r, LINK_STOP, NULL, 0, NULL, NULL);

    r->running = false;
    queue_clear(r);

    return err;
}


static void remote_close(void *src)
{
    struct remote *r = src;

    if (!r) {
        return;
    }
    if (r->fd >= 0) {
        close(r->fd);
    }
    queue_clear(r);
    free(r->q);
    free(r->cur.data);
    free(r->configs);
    thc_buf_free(&r->in);
    thc_buf_free(&r->reply);
    free(r);
}


/*
 * Sends a request and waits for its reply, queueing the items that come first. The
 * reply's status is returned; payload (if not NULL) gets what follows it, valid until the
 * next request.
 */
static int request(struct remote *r, uint8_t type, const uint8_t *data, size_t len,
                   const uint8_t **payload, size_t *payload_len)
{
    struct link_msg m = {.type = type, .data = data, .len = len};
    struct thc_buf out = {0};
    uint64_t until = now_ns() + (uint64_t)REMOTE_TIMEOUT_MS * 1000000;
    int err = link_put(&out, &m);

    err = err ? err : link_send(r->fd, out.data, out.len, REMOTE_TIMEOUT_MS);
    thc_buf_free(&out);
    r->have_reply = false;
    while (err == 0 && !r->have_reply) {
        uint64_t now = now_ns();

        err = now < until ? receive(r, (int)((until - now) / 1000000) + 1) : -ETIMEDOUT;
    }
    if (err < 0) {
        return err;
    }
    r->have_reply = false;
    if (r->reply_type != (type | LINK_REPLY) || r->reply.len < 4) {
        return -EPROTO;
    }
    if (payload) {
        *payload = r->reply.data + 4;
        *payload_len = r->reply.len - 4;
    }

    return (int32_t)thc_get_le(r->reply.data, 4);
}


/*
 * Reads what the socket has, waiting up to timeout_ms for the first of it, and takes every
 * whole message in it. -ECONNRESET once the link has gone.
 */
static int receive(struct remote *r, int timeout_ms)
{
    struct pollfd p = {.fd = r->fd, .events = POLLIN};
    uint8_t buf[READ_CHUNK];
    ssize_t n = 0;
    int err = 0;

    if (r->closed) {
        return -ECONNRESET;
    }
    n = poll(&p, 1, timeout_ms);
    if (n < 0) {
        return errno == EINTR ? 0 : -errno;
    }
    if (n == 0) {
        return 0;
    }
    n = recv(r->fd, buf, sizeof(buf), MSG_DONTWAIT);
    if (n < 0) {
        return errno == EAGAIN || errno == EINTR ? 0 : -errno;
    }
    if (n == 0) {
        r->closed = true;
        return -ECONNRESET;
    }
    err = thc_buf_put(&r->in, buf, (size_t)n);
    while (err == 0) {
        struct link_msg m;
        ssize_t used = link_parse(r->in.data + r->in_off, r->in.len - r->in_off, &m);

        if (used <= 0) {
            err = (int)used;
            break;
        }
        err = take(r, &m);
        r->in_off += (size_t)used;
    }
    if (r->in_off == r->in.len) {
        r->in.len = 0;
        r->in_off = 0;
    } else if (r->in_off > r->in.len / 2) {
        memmove(r->in.data, r->in.data + r->in_off, r->in.len - r->in_off);
        r->in.len -= r->in_off;
        r->in_off = 0;
    }

    return err;
}


// One message from the link: an item is queued, anything else is a reply.
static int take(struct remote *r, const struct link_msg *m)
{
    struct queued q = {.type = m->type, .flags = m->flags, .channel = m->channel,
                       .time = m->time, .len = m->len};

    if (m->type == THC_DATA || m->type == THC_CONTROL || m->type == THC_EVENT) {
        if (r->len == r->cap) {
            size_t cap = r->cap ? 2 * r->cap : 256;
            struct queued *bigger = malloc(cap * sizeof(*bigger));

            if (!bigger) {
                return -ENOMEM;
            }
            for (size_t i = 0; i < r->len; i++) {
                bigger[i] = r->q[(r->head + i) % r->cap];
            }
            free(r->q);
            r->q = bigger;
            r->head = 0;
            r->cap = cap;
        }
        q.data = malloc(m->len ? m->len : 1);
        if (!q.data) {
            return -ENOMEM;
        }
        memcpy(q.data, m->data, m->len);
        r->q[(r->head + r->len) % r->cap] = q;
        r->len++;
        return 0;
    }
    if (m->type < LINK_WELCOME) {
        return 0;
    }
    r->reply.len = 0;
    r->reply_type = m->type;
    r->have_reply = true;

    return thc_buf_put(&r->reply, m->data, m->len);
}


static void queue_clear(struct remote *r)
{
    while (r->len) {
        free(r->q[r->head].data);
        r->head = (r->head + 1) % r->cap;
        r->len--;
    }
    r->head = 0;
}


static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
