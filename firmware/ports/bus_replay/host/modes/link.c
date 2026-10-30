/*
 * bus-replay link: the device, served to one client at a time over a Unix socket
 * (docs/link-protocol.md). The link forwards the client's hello, configuration and stop to
 * the device, and once a capture runs it parses it in a capture session of its own,
 * records it when asked to, and forwards the raw items to the client. The capture goes on
 * when the client goes away; the next one joins it with ATTACH.
 *
 * One thread: requests are answered between pulls. A device operation blocks the loop,
 * but the local backend's reader keeps taking the data UART meanwhile.
 */

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "capture/board.h"
#include "capture/capture.h"
#include "capture/control.h"
#include "capture/dummy.h"
#include "capture/linkproto.h"
#include "capture/local.h"
#include "capture/recorder.h"
#include "capture/source.h"
#include "capture/thcfile.h"
#include "common/record.h"
#include "modes/modes.h"


#define QUEUE_MAX (32U << 20)
#define TAIL_KEEP 4096
#define READ_CHUNK 65536
#define LINK_NAME "bus-replay link"

// A configuration message or reply of the capture being set up or running.
struct logged {
    uint16_t type;
    uint8_t *data;
    size_t len;
};

/*
 * The client. out holds what is queued for it, from out_off on. streaming: items go to
 * it. attaching: it joins at grid; started says which data channels have begun, dropped
 * the bytes dropped per data channel since its last overflow event.
 */
struct client {
    int fd;
    struct thc_buf in;
    size_t in_off;
    struct thc_buf out;
    size_t out_off;
    bool streaming;
    bool attaching;
    uint64_t grid;
    bool *started;
    uint64_t *dropped;
    uint32_t dropped_control;
    uint64_t dropped_total;
    uint32_t dropped_control_total;
};

// A heartbeat seen in this pull: its data channel, grid count and stream position.
struct seen {
    uint16_t dc;
    uint64_t grid;
    uint64_t pos;
};

// An item taken in this pull, for the client.
struct taken {
    uint8_t type;
    uint8_t flags;
    uint16_t channel;
    uint64_t time;
    uint8_t *data;
    size_t len;
};

/*
 * ops and src are the device. The capture runs in cap, reading the device through tee,
 * which keeps a copy of every item for the client. tail holds the last bytes of each data
 * channel's stream, from tail_pos, so a client joins at a heartbeat that began in an
 * earlier pull.
 */
struct link {
    int listen;
    struct client cl;
    size_t queue_max;

    const struct source_ops *ops;
    void *src;
    const char *binding;
    uint8_t *hello;
    size_t hello_len;
    struct logged *log;
    size_t log_len;
    uint8_t *loss_base;
    size_t loss_base_len;

    struct capture *cap;
    bool running;
    uint64_t t0;
    uint16_t dcs;
    uint64_t *pos;
    uint64_t *last_grid;
    struct thc_buf *tail;
    uint64_t *tail_pos;

    struct taken *items;
    size_t item_len;
    size_t item_cap;
    struct seen *seen;
    size_t seen_len;
    size_t seen_cap;

    char record[256];
    int record_error;
    uint64_t recorded;
};


static int tee_hello(void *src, const uint8_t **msg, size_t *len);
static int tee_config(void *src, const uint8_t *msg, size_t len, const uint8_t **reply,
                      size_t *reply_len);
static int tee_read(void *src, struct source_item *item, int timeout_ms);
static int tee_stop(void *src);
static void tee_close(void *src);
static void on_heartbeat(void *ctx, const struct heartbeat *hb);
static int listen_on(const char *path);
static void accept_client(struct link *l);
static void drop_client(struct link *l);
static int client_read(struct link *l);
static void client_write(struct link *l);
static void handle(struct link *l, const struct link_msg *m);
static void reply(struct link *l, uint8_t type, int status, const uint8_t *data, size_t len);
static void on_hello(struct link *l);
static void on_config(struct link *l, const struct link_msg *m);
static void on_attach(struct link *l);
static void on_status(struct link *l);
static int capture_begin(struct link *l);
static void capture_end(struct link *l, uint8_t reason);
static int pull(struct link *l);
static void forward(struct link *l);
static void enqueue(struct link *l, uint8_t type, uint8_t flags, uint16_t channel,
                    uint64_t time, const uint8_t *data, size_t len);
static void log_clear(struct link *l);
static int log_add(struct link *l, uint16_t type, const uint8_t *data, size_t len);
static uint8_t state_bits(const struct link *l);
static uint64_t now_ns(void);
static void on_signal(int sig);
static void usage(FILE *out);


static volatile sig_atomic_t quit;

static const struct source_ops tee_ops = {
    .hello = tee_hello,
    .config = tee_config,
    .read = tee_read,
    .stop = tee_stop,
    .close = tee_close,
};


int link_main(int argc, char **argv)
{
    struct sigaction sa = {.sa_handler = on_signal};
    struct link l = {.listen = -1, .cl = {.fd = -1}, .queue_max = QUEUE_MAX};
    const char *socket_arg = NULL;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    bool dummy = false;
    int err = 0;

    for (int i = 1; i < argc; i++) {
        if (arg_is(argv[i], "-h", "--help")) {
            usage(stdout);
            return 0;
        } else if (arg_is(argv[i], "-s", "--socket") && i + 1 < argc) {
            socket_arg = argv[++i];
        } else if (arg_is(argv[i], NULL, "--dummy")) {
            dummy = true;
        } else if (arg_is(argv[i], "-o", "--record") && i + 1 < argc) {
            snprintf(l.record, sizeof(l.record), "%s", argv[++i]);
        } else if (arg_is(argv[i], NULL, "--queue") && i + 1 < argc) {
            l.queue_max = strtoull(argv[++i], NULL, 0);
        } else {
            usage(stderr);
            return 2;
        }
    }

    err = link_path(path, sizeof(path), socket_arg);
    if (err < 0) {
        fprintf(stderr, "bus-replay link: no socket path (err: %d)\n", err);
        return 1;
    }
    if (dummy) {
        struct dummy_options opt = {.realtime = true};

        err = dummy_open(&l.src, &opt);
        l.ops = &dummy_ops;
        l.binding = "dummy";
    } else {
        err = local_open(&l.src);
        l.ops = &local_ops;
        l.binding = BOARD_BINDING;
    }
    if (err < 0) {
        fprintf(stderr, "bus-replay link: can't open the device (err: %d)\n", err);
        return 1;
    }
    l.listen = listen_on(path);
    if (l.listen < 0) {
        fprintf(stderr, "bus-replay link: can't listen on %s (err: %d)\n", path, l.listen);
        l.ops->close(l.src);
        return 1;
    }
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    // A terminal that closes (a UI's child in it) ends the link the same way.
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    fprintf(stderr, "bus-replay link: on %s%s\n", path, dummy ? " (dummy)" : "");

    while (!quit) {
        struct pollfd p[2] = {{.fd = l.listen, .events = POLLIN}, {.fd = l.cl.fd}};
        nfds_t n = 1;

        if (l.cl.fd >= 0) {
            p[1].events = POLLIN | (l.cl.out.len > l.cl.out_off ? POLLOUT : 0);
            n = 2;
        }
        if (poll(p, n, l.running ? 2 : 100) < 0 && errno != EINTR) {
            break;
        }
        if (p[0].revents & POLLIN) {
            accept_client(&l);
        }
        if (n == 2 && (p[1].revents & (POLLIN | POLLHUP | POLLERR)) && client_read(&l) < 0) {
            drop_client(&l);
        }
        if (l.running && pull(&l) < 0) {
            fprintf(stderr, "bus-replay link: the capture failed, ending it\n");
            capture_end(&l, THC_REASON_ERROR);
        }
        if (l.cl.fd >= 0) {
            client_write(&l);
        }
    }

    if (l.running) {
        capture_end(&l, THC_REASON_SIGNAL);
    }
    drop_client(&l);
    close(l.listen);
    unlink(path);
    l.ops->close(l.src);
    log_clear(&l);
    free(l.log);
    free(l.hello);
    free(l.loss_base);
    free(l.items);
    free(l.seen);

    return 0;
}


// The capture session's hello: the one the device sent last, without a reset.
static int tee_hello(void *src, const uint8_t **msg, size_t *len)
{
    struct link *l = src;

    if (!l->hello) {
        return -ENODATA;
    }
    *msg = l->hello;
    *len = l->hello_len;

    return 0;
}


// The client configures the device; the capture session only joins (capture_attach()).
static int tee_config(void *src, const uint8_t *msg, size_t len, const uint8_t **reply,
                      size_t *reply_len)
{
    (void)src;
    (void)msg;
    (void)len;
    (void)reply;
    (void)reply_len;

    return -ENOTSUP;
}


// An item from the device, with a copy kept for the client and the stream's tail.
static int tee_read(void *src, struct source_item *item, int timeout_ms)
{
    struct link *l = src;
    struct taken *t = NULL;
    int n = l->ops->read(l->src, item, timeout_ms);

    if (n <= 0) {
        return n;
    }
    if (l->item_len == l->item_cap) {
        size_t cap = l->item_cap ? 2 * l->item_cap : 64;
        struct taken *bigger = realloc(l->items, cap * sizeof(*bigger));

        if (!bigger) {
            return -ENOMEM;
        }
        l->items = bigger;
        l->item_cap = cap;
    }
    t = &l->items[l->item_len];
    *t = (struct taken){.type = item->type, .flags = item->flags, .channel = item->channel,
                        .time = item->time > l->t0 ? item->time - l->t0 : 0,
                        .data = malloc(item->len ? item->len : 1), .len = item->len};
    if (!t->data) {
        return -ENOMEM;
    }
    memcpy(t->data, item->data, item->len);
    l->item_len++;

    if (item->type == THC_DATA && item->channel < l->dcs) {
        int err = thc_buf_put(&l->tail[item->channel], item->data, item->len);

        if (err < 0) {
            return err;
        }
        l->pos[item->channel] += item->len;
    } else if (item->type == THC_CONTROL && !(item->flags & THC_TO_DEVICE) && item->len > 0 &&
               item->data[0] == MSG_LOSS_REPORT) {
        uint8_t *copy = malloc(item->len);

        if (copy) {
            memcpy(copy, item->data, item->len);
            free(l->loss_base);
            l->loss_base = copy;
            l->loss_base_len = item->len;
        }
    }

    return 1;
}


static int tee_stop(void *src)
{
    struct link *l = src;

    return l->ops->stop(l->src);
}


// The device stays open; the link closes it on exit.
static void tee_close(void *src)
{
    (void)src;
}


// Where each heartbeat starts in its stream, for a client joining at it.
static void on_heartbeat(void *ctx, const struct heartbeat *hb)
{
    struct link *l = ctx;

    if (hb->data_channel >= l->dcs) {
        return;
    }
    l->last_grid[hb->data_channel] = hb->grid;
    if (l->seen_len == l->seen_cap) {
        size_t cap = l->seen_cap ? 2 * l->seen_cap : 64;
        struct seen *bigger = realloc(l->seen, cap * sizeof(*bigger));

        if (!bigger) {
            return;
        }
        l->seen = bigger;
        l->seen_cap = cap;
    }
    l->seen[l->seen_len++] = (struct seen){
        .dc = hb->data_channel, .grid = hb->grid,
        .pos = capture_stream_pos(l->cap, hb->data_channel)};
}


/*
 * The socket, mode 0600. A stale one is removed; one another link answers on is left, and
 * -EADDRINUSE returned.
 */
static int listen_on(const char *path)
{
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    mode_t old = 0;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int err = 0;

    if (fd < 0) {
        return -errno;
    }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        close(fd);
        return -EADDRINUSE;
    }
    close(fd);
    unlink(path);

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        return -errno;
    }
    old = umask(0177);
    err = bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ? -errno : 0;
    umask(old);
    if (err == 0 && listen(fd, 4) < 0) {
        err = -errno;
    }
    if (err < 0) {
        close(fd);
        return err;
    }

    return fd;
}


// A new client gets WELCOME; with one connected already, BUSY and the door.
static void accept_client(struct link *l)
{
    int fd = accept4(l->listen, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    uint8_t welcome[8 + sizeof(LINK_NAME) - 1] = {LINK_MAJOR, 0, LINK_MINOR, 0, 0};
    struct thc_buf b = {0};
    struct link_msg m;

    if (fd < 0) {
        return;
    }
    if (l->cl.fd >= 0) {
        static const char busy[] = "another client is connected";

        m = (struct link_msg){.type = LINK_BUSY, .data = (const uint8_t *)busy,
                              .len = sizeof(busy) - 1};

        if (link_put(&b, &m) == 0) {
            link_send(fd, b.data, b.len, 100);
        }
        thc_buf_free(&b);
        close(fd);
        return;
    }

    l->cl = (struct client){.fd = fd};
    l->cl.started = calloc(l->dcs ? l->dcs : 1, sizeof(*l->cl.started));
    l->cl.dropped = calloc(l->dcs ? l->dcs : 1, sizeof(*l->cl.dropped));
    welcome[4] = state_bits(l);
    memcpy(&welcome[8], LINK_NAME, sizeof(LINK_NAME) - 1);
    m = (struct link_msg){.type = LINK_WELCOME, .data = welcome, .len = sizeof(welcome)};
    if (!l->cl.started || !l->cl.dropped || link_put(&l->cl.out, &m) < 0) {
        drop_client(l);
    }
}


static void drop_client(struct link *l)
{
    if (l->cl.fd < 0) {
        return;
    }
    close(l->cl.fd);
    thc_buf_free(&l->cl.in);
    thc_buf_free(&l->cl.out);
    free(l->cl.started);
    free(l->cl.dropped);
    l->cl = (struct client){.fd = -1};
}


// Reads the client's requests and answers each. Negative once it has gone.
static int client_read(struct link *l)
{
    uint8_t buf[READ_CHUNK];
    ssize_t n = recv(l->cl.fd, buf, sizeof(buf), MSG_DONTWAIT);
    int err = 0;

    if (n == 0) {
        return -ECONNRESET;
    }
    if (n < 0) {
        return errno == EAGAIN || errno == EINTR ? 0 : -errno;
    }
    err = thc_buf_put(&l->cl.in, buf, (size_t)n);
    while (err == 0 && l->cl.fd >= 0) {
        struct link_msg m;
        ssize_t used = link_parse(l->cl.in.data + l->cl.in_off, l->cl.in.len - l->cl.in_off,
                                  &m);

        if (used <= 0) {
            err = (int)used;
            break;
        }
        handle(l, &m);
        l->cl.in_off += (size_t)used;
    }
    if (l->cl.in_off == l->cl.in.len) {
        l->cl.in.len = 0;
        l->cl.in_off = 0;
    }

    return err;
}


// Sends what the socket takes now, without waiting.
static void client_write(struct link *l)
{
    struct client *c = &l->cl;

    while (c->out.len > c->out_off) {
        ssize_t n = send(c->fd, c->out.data + c->out_off, c->out.len - c->out_off,
                         MSG_DONTWAIT | MSG_NOSIGNAL);

        if (n < 0) {
            if (errno != EAGAIN && errno != EINTR) {
                drop_client(l);
            }
            break;
        }
        c->out_off += (size_t)n;
    }
    if (c->fd >= 0 && c->out_off == c->out.len) {
        c->out.len = 0;
        c->out_off = 0;
    } else if (c->fd >= 0 && c->out_off > (1U << 20)) {
        memmove(c->out.data, c->out.data + c->out_off, c->out.len - c->out_off);
        c->out.len -= c->out_off;
        c->out_off = 0;
    }
}


static void handle(struct link *l, const struct link_msg *m)
{
    switch (m->type) {
    case LINK_HELLO:
        on_hello(l);
        break;
    case LINK_CONFIG:
        on_config(l, m);
        break;
    case LINK_STOP:
        if (l->running) {
            capture_end(l, THC_REASON_USER);
        }
        reply(l, LINK_STOP, 0, NULL, 0);
        break;
    case LINK_ATTACH:
        on_attach(l);
        break;
    case LINK_RECORD:
        if (l->running) {
            reply(l, LINK_RECORD, -EBUSY, NULL, 0);
            break;
        }
        snprintf(l->record, sizeof(l->record), "%.*s", (int)m->len, (const char *)m->data);
        reply(l, LINK_RECORD, 0, NULL, 0);
        break;
    case LINK_STATUS:
        on_status(l);
        break;
    default:
        break;
    }
}


// A reply: the request's type with bit 7, the status, then data. Never dropped.
static void reply(struct link *l, uint8_t type, int status, const uint8_t *data, size_t len)
{
    struct thc_buf *b = &l->cl.out;
    uint8_t st[4];
    int err = 0;

    for (int i = 0; i < 4; i++) {
        st[i] = (uint8_t)((uint32_t)status >> (8 * i));
    }
    err = thc_buf_le(b, type | LINK_REPLY, 1);
    err = err ? err : thc_buf_le(b, 0, 3);
    err = err ? err : thc_buf_le(b, 4 + len, 4);
    err = err ? err : thc_buf_le(b, 0, 8);
    err = err ? err : thc_buf_put(b, st, 4);
    if (err == 0 && len) {
        err = thc_buf_put(b, data, len);
    }
    if (err < 0) {
        drop_client(l);
    }
}


// A reset and the device's hello; a capture running ends first.
static void on_hello(struct link *l)
{
    const uint8_t *msg = NULL;
    size_t len = 0;
    int err = 0;

    if (l->running) {
        capture_end(l, THC_REASON_USER);
    }
    log_clear(l);
    err = l->ops->hello(l->src, &msg, &len);
    if (err == 0) {
        uint8_t *copy = malloc(len ? len : 1);

        if (!copy) {
            err = -ENOMEM;
        } else {
            memcpy(copy, msg, len);
            free(l->hello);
            l->hello = copy;
            l->hello_len = len;
        }
    }
    reply(l, LINK_HELLO, err, err == 0 ? l->hello : NULL, err == 0 ? l->hello_len : 0);
}


// A configuration message for the device; its capture register write starts the capture.
static void on_config(struct link *l, const struct link_msg *m)
{
    const uint8_t *rep = NULL;
    size_t rep_len = 0;
    struct config_reply cr;
    int err = l->running ? -EBUSY : l->hello ? 0 : -ENODATA;

    err = err ? err : l->ops->config(l->src, m->data, m->len, &rep, &rep_len);
    if (err < 0) {
        reply(l, LINK_CONFIG, err, NULL, 0);
        return;
    }
    l->t0 = now_ns();
    log_add(l, THC_ITEM_CONFIG, m->data, m->len);
    log_add(l, THC_ITEM_CONFIG_REPLY, rep, rep_len);
    reply(l, LINK_CONFIG, 0, rep, rep_len);

    if (m->len >= 3 && m->data[0] == MSG_CONFIG && m->data[1] == REG_CAPTURE &&
        m->data[2] == 1 && config_reply_parse(rep, rep_len, &cr) == 0 && cr.status == 0) {
        err = capture_begin(l);
        if (err < 0) {
            fprintf(stderr, "bus-replay link: can't follow the capture (err: %d)\n", err);
        } else if (l->cl.fd >= 0) {
            l->cl.streaming = true;
        }
    }
}


/*
 * Joining the capture: the client's stream starts at the next grid count every data
 * channel reaches, which the reply names at once; its items follow from there.
 */
static void on_attach(struct link *l)
{
    struct thc_buf b = {0};
    uint64_t grid = 0;
    int err = 0;

    if (!l->running) {
        reply(l, LINK_ATTACH, -ENOENT, NULL, 0);
        return;
    }
    for (uint16_t d = 0; d < l->dcs; d++) {
        if (l->last_grid[d] + 1 > grid) {
            grid = l->last_grid[d] + 1;
        }
    }
    err = thc_buf_le(&b, grid, 8);
    err = err ? err : thc_buf_le(&b, now_ns() - l->t0, 8);
    err = err ? err : thc_buf_le(&b, l->hello_len, 4);
    err = err ? err : thc_buf_put(&b, l->hello, l->hello_len);
    err = err ? err : thc_buf_le(&b, l->log_len, 4);
    for (size_t i = 0; i < l->log_len && err == 0; i++) {
        err = thc_buf_le(&b, l->log[i].type, 2);
        err = err ? err : thc_buf_le(&b, l->log[i].len, 4);
        err = err ? err : thc_buf_put(&b, l->log[i].data, l->log[i].len);
    }
    err = err ? err : thc_buf_le(&b, l->loss_base_len, 4);
    if (err == 0 && l->loss_base_len) {
        err = thc_buf_put(&b, l->loss_base, l->loss_base_len);
    }
    reply(l, LINK_ATTACH, err, err == 0 ? b.data : NULL, err == 0 ? b.len : 0);
    thc_buf_free(&b);
    if (err == 0 && l->cl.fd >= 0) {
        l->cl.streaming = false;
        l->cl.attaching = true;
        l->cl.grid = grid;
        memset(l->cl.started, 0, (l->dcs ? l->dcs : 1) * sizeof(*l->cl.started));
    }
}


static void on_status(struct link *l)
{
    struct thc_buf b = {0};
    size_t plen = strlen(l->record);
    uint64_t recorded = l->recorded;
    struct recorder *rec = l->cap ? capture_recorder(l->cap) : NULL;
    int err = 0;

    if (rec) {
        struct recorder_stats rs;

        recorder_stats(rec, &rs);
        recorded = rs.written;
    }
    err = thc_buf_le(&b, state_bits(l), 1);
    err = err ? err : thc_buf_le(&b, 0, 3);
    err = err ? err : thc_buf_le(&b, (uint32_t)l->record_error, 4);
    err = err ? err : thc_buf_le(&b, recorded, 8);
    err = err ? err : thc_buf_le(&b, l->cl.dropped_total, 8);
    err = err ? err : thc_buf_le(&b, l->cl.dropped_control_total, 4);
    err = err ? err : thc_buf_le(&b, plen, 4);
    if (err == 0 && plen) {
        err = thc_buf_put(&b, l->record, plen);
    }
    reply(l, LINK_STATUS, err, err == 0 ? b.data : NULL, err == 0 ? b.len : 0);
    thc_buf_free(&b);
}


// The capture the client started: the link's own session joins it from its start.
static int capture_begin(struct link *l)
{
    struct capture_handler h = {.heartbeat = on_heartbeat, .ctx = l};
    struct capture_record_options ro = {.binding = l->binding, .host = "bus-replay link"};
    struct recorder_msg *msgs = calloc(l->log_len ? l->log_len : 1, sizeof(*msgs));
    struct capture_attach a = {.hello = l->hello, .hello_len = l->hello_len,
                               .configs = msgs, .config_count = l->log_len};
    char error[160];
    int err = msgs ? 0 : -ENOMEM;

    for (size_t i = 0; msgs && i < l->log_len; i++) {
        msgs[i] = (struct recorder_msg){.type = l->log[i].type, .data = l->log[i].data,
                                        .len = l->log[i].len};
    }
    err = err ? err : capture_open(&l->cap, &tee_ops, l, &h);
    if (err == 0 && l->record[0]) {
        l->record_error = capture_record(l->cap, l->record, &ro);
        if (l->record_error < 0) {
            fprintf(stderr, "bus-replay link: can't record to %s (err: %d)\n", l->record,
                    l->record_error);
        }
    }
    err = err ? err : capture_attach(l->cap, &a, error, sizeof(error));
    free(msgs);
    if (err < 0) {
        capture_close(l->cap);
        l->cap = NULL;
        return err;
    }

    l->dcs = capture_info(l->cap)->data_channels;
    l->pos = calloc(l->dcs, sizeof(*l->pos));
    l->last_grid = calloc(l->dcs, sizeof(*l->last_grid));
    l->tail = calloc(l->dcs, sizeof(*l->tail));
    l->tail_pos = calloc(l->dcs, sizeof(*l->tail_pos));
    if (l->cl.fd >= 0) {
        free(l->cl.started);
        free(l->cl.dropped);
        l->cl.started = calloc(l->dcs, sizeof(*l->cl.started));
        l->cl.dropped = calloc(l->dcs, sizeof(*l->cl.dropped));
    }
    free(l->loss_base);
    l->loss_base = NULL;
    l->loss_base_len = 0;
    l->recorded = 0;
    l->running = true;

    return l->pos && l->last_grid && l->tail && l->tail_pos ? 0 : -ENOMEM;
}


static void capture_end(struct link *l, uint8_t reason)
{
    struct recorder *rec = capture_recorder(l->cap);

    if (rec) {
        struct recorder_stats rs;

        recorder_stats(rec, &rs);
        l->recorded = rs.written;
    }
    capture_stop(l->cap, reason);
    capture_close(l->cap);
    l->cap = NULL;
    l->running = false;
    for (uint16_t d = 0; d < l->dcs; d++) {
        thc_buf_free(&l->tail[d]);
    }
    free(l->pos);
    free(l->last_grid);
    free(l->tail);
    free(l->tail_pos);
    l->pos = l->last_grid = l->tail_pos = NULL;
    l->tail = NULL;
    l->dcs = 0;
    l->cl.streaming = false;
    l->cl.attaching = false;
    for (size_t i = 0; i < l->item_len; i++) {
        free(l->items[i].data);
    }
    l->item_len = 0;
}


// Takes what the device has sent, and passes it on to the client.
static int pull(struct link *l)
{
    int n = 0;

    for (uint16_t d = 0; d < l->dcs; d++) {
        struct thc_buf *t = &l->tail[d];

        if (t->len > TAIL_KEEP) {
            memmove(t->data, t->data + t->len - TAIL_KEEP, TAIL_KEEP);
            l->tail_pos[d] += t->len - TAIL_KEEP;
            t->len = TAIL_KEEP;
        }
    }
    l->seen_len = 0;
    do {
        n = capture_pull(l->cap, 0);
    } while (n > 0 && l->item_len < 1024);
    forward(l);
    for (size_t i = 0; i < l->item_len; i++) {
        free(l->items[i].data);
    }
    l->item_len = 0;

    return n < 0 ? n : 0;
}


/*
 * This pull's items for the client. A joining client starts each data channel at the
 * first byte of its heartbeat of the join's grid count (or a later one, if that one was
 * missed); control messages and events follow once every data channel has begun.
 */
static void forward(struct link *l)
{
    struct client *c = &l->cl;
    bool all = true;

    if (c->fd < 0 || (!c->streaming && !c->attaching)) {
        return;
    }
    if (c->attaching) {
        // Data channels that began in an earlier pull go on with this pull's DATA.
        for (size_t i = 0; i < l->item_len; i++) {
            const struct taken *t = &l->items[i];

            if (t->type == THC_DATA && t->channel < l->dcs && c->started[t->channel]) {
                enqueue(l, t->type, t->flags, t->channel, t->time, t->data, t->len);
            }
        }
        for (uint16_t d = 0; d < l->dcs; d++) {
            for (size_t i = 0; i < l->seen_len && !c->started[d]; i++) {
                const struct seen *s = &l->seen[i];
                const struct thc_buf *t = &l->tail[d];

                if (s->dc != d || s->grid < c->grid || s->pos < l->tail_pos[d]) {
                    continue;
                }
                enqueue(l, THC_DATA, 0, d, now_ns() - l->t0,
                        t->data + (s->pos - l->tail_pos[d]),
                        (size_t)(l->tail_pos[d] + t->len - s->pos));
                c->started[d] = true;
            }
            all = all && c->started[d];
        }
        if (all) {
            c->attaching = false;
            c->streaming = true;
        }
        return;
    }

    for (size_t i = 0; i < l->item_len; i++) {
        const struct taken *t = &l->items[i];

        enqueue(l, t->type, t->flags, t->channel, t->time, t->data, t->len);
    }
}


/*
 * An item for the client. Past the queue's limit it is dropped and counted; the data
 * channel's next DATA is preceded by an overflow event saying how much went.
 */
static void enqueue(struct link *l, uint8_t type, uint8_t flags, uint16_t channel,
                    uint64_t time, const uint8_t *data, size_t len)
{
    struct client *c = &l->cl;
    struct link_msg m = {.type = type, .flags = flags, .channel = channel, .time = time,
                         .data = data, .len = len};
    size_t queued = c->out.len - c->out_off;
    bool dc = type == THC_DATA && channel < l->dcs;

    if (queued + LINK_HEAD + len > l->queue_max) {
        if (dc) {
            c->dropped[channel] += len;
            c->dropped_total += len;
        } else {
            c->dropped_control++;
            c->dropped_control_total++;
        }
        return;
    }
    if ((dc && c->dropped[channel]) || (!dc && c->dropped_control)) {
        uint8_t ev[14];
        uint64_t bytes = dc ? c->dropped[channel] : 0;
        struct link_msg o = {.type = THC_EVENT, .channel = dc ? channel : 0, .time = time,
                             .data = ev, .len = sizeof(ev)};

        ev[0] = (uint8_t)THC_EV_OVERFLOW;
        ev[1] = (uint8_t)(THC_EV_OVERFLOW >> 8);
        for (int i = 0; i < 8; i++) {
            ev[2 + i] = (uint8_t)(bytes >> (8 * i));
        }
        for (int i = 0; i < 4; i++) {
            ev[10 + i] = (uint8_t)(c->dropped_control >> (8 * i));
        }
        if (link_put(&c->out, &o) < 0) {
            drop_client(l);
            return;
        }
        if (dc) {
            c->dropped[channel] = 0;
        }
        c->dropped_control = 0;
    }
    if (link_put(&c->out, &m) < 0) {
        drop_client(l);
    }
}


static void log_clear(struct link *l)
{
    for (size_t i = 0; i < l->log_len; i++) {
        free(l->log[i].data);
    }
    l->log_len = 0;
}


static int log_add(struct link *l, uint16_t type, const uint8_t *data, size_t len)
{
    struct logged *bigger = realloc(l->log, (l->log_len + 1) * sizeof(*bigger));
    uint8_t *copy = malloc(len ? len : 1);

    if (bigger) {
        l->log = bigger;
    }
    if (!bigger || !copy) {
        free(copy);
        return -ENOMEM;
    }
    memcpy(copy, data, len);
    l->log[l->log_len++] = (struct logged){.type = type, .data = copy, .len = len};

    return 0;
}


static uint8_t state_bits(const struct link *l)
{
    return (l->running ? LINK_RUNNING : 0) |
           (l->running && l->cap && capture_recorder(l->cap) ? LINK_RECORDING : 0);
}


static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}


static void on_signal(int sig)
{
    quit = sig;
}


static void usage(FILE *out)
{
    fprintf(out,
            "usage: bus-replay link [-s PATH] [--dummy] [-o FILE] [--queue BYTES]\n"
            "\n"
            "The device, served to one client at a time (docs/link-protocol.md).\n"
            "\n"
            "  -s, --socket PATH   the socket (default $BUS_REPLAY_LINK, else\n"
            "                      $XDG_RUNTIME_DIR/bus-replay.sock, else\n"
            "                      /tmp/bus-replay-UID/link.sock)\n"
            "      --dummy         the dummy device instead of the one on this Pi\n"
            "  -o, --record FILE   record the next capture to FILE\n"
            "      --queue BYTES   the most queued for a client (default 32 MiB)\n");
}
