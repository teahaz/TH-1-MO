#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/poll.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <asm-generic/termbits-common.h>
#include <bits/pthreadtypes.h>
#include <bits/termios-c_cc.h>
#include <bits/termios-c_cflag.h>
#include <bits/termios-tcflow.h>
#include <bits/time.h>

#include "capture/board.h"
#include "capture/control.h"
#include "capture/gpio.h"
#include "capture/local.h"
#include "capture/source.h"
#include "capture/spi.h"
#include "capture/thcfile.h"


#define BOOT_TIMEOUT_MS 2000
#define REPLY_TIMEOUT_MS 1000
#define DATA_SILENT_MS 1000

#define BLOCK_SIZE 16384
#define COALESCE_MS 10
#define QUEUE_MAX (64U << 20)

#define EVENT_MAX 160

// Data bytes as read: epoch ties them to the capture they were read in (see flush_data).
struct block {
    struct block *next;
    uint64_t epoch;
    uint64_t time;
    size_t len;
    uint8_t data[BLOCK_SIZE];
};

// A message the device sent while the host waited for another one.
struct pending {
    struct pending *next;
    uint64_t time;
    size_t len;
    uint8_t data[];
};

/*
 * The reader thread and the caller share the fields under lock. The caller owns the rest:
 * taken (the block given out by the last read) and msg (the last message given out).
 */
struct local {
    struct spi_control spi;
    int uart;
    int wake;
    int quit;
    pthread_t reader;
    bool reader_running;

    pthread_mutex_t lock;
    struct block *head;
    struct block *tail;
    size_t queued;
    uint64_t dropped;
    uint64_t epoch;
    uint64_t last_data;
    int read_error;

    struct block *taken;
    struct pending *pending;
    uint8_t *msg;
    size_t msg_len;
    uint8_t event[EVENT_MAX];
    bool capturing;
    bool silent;
    bool error_told;
    uint64_t last_time;
};


static int hello(void *src, const uint8_t **msg, size_t *len);
static int config(void *src, const uint8_t *msg, size_t len, const uint8_t **reply,
                  size_t *reply_len);
static int local_read(void *src, struct source_item *item, int timeout_ms);
static int stop(void *src);
static void local_close(void *src);
static int receive(struct local *s, uint8_t type, uint8_t reg, int timeout_ms);
static int take_message(struct local *s, const uint8_t *in, size_t len, uint8_t type,
                        uint8_t reg);
static int keep_pending(struct local *s, const uint8_t *in, size_t len);
static void drop_pending(struct local *s);
static void flush_data(struct local *s);
static int next_item(struct local *s, struct source_item *item);
static void make_event(struct local *s, struct source_item *item, uint16_t code,
                       const void *data, size_t len);
static void *reader_main(void *arg);
static void publish(struct local *s, struct block *b);
static int uart_open(void);
static uint64_t mono_ns(void);


const struct source_ops local_ops = {
    .hello = hello,
    .config = config,
    .read = local_read,
    .stop = stop,
    .close = local_close,
};


/*
 * Takes the GPIO lines and the data UART and starts the reader. Nothing is driven until
 * the first source operation. Returns -errno from the GPIO chip, the line request or the
 * UART.
 */
int local_open(void **src)
{
    struct local *s = calloc(1, sizeof(*s));
    int err = 0;

    *src = NULL;
    if (!s) {
        return -ENOMEM;
    }
    s->uart = -1;
    s->wake = -1;
    s->quit = -1;
    pthread_mutex_init(&s->lock, NULL);

    err = spi_control_open(&s->spi);
    if (err == 0) {
        s->uart = uart_open();
        err = s->uart < 0 ? s->uart : 0;
    }
    if (err == 0) {
        s->wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        s->quit = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        err = s->wake < 0 || s->quit < 0 ? -errno : 0;
    }
    if (err == 0) {
        err = -pthread_create(&s->reader, NULL, reader_main, s);
        s->reader_running = err == 0;
    }
    if (err < 0) {
        local_close(s);
        return err;
    }
    *src = s;

    return 0;
}


// A reset, then the hello the device sends after it (spec, Hello).
static int hello(void *src, const uint8_t **msg, size_t *len)
{
    struct local *s = src;
    int err = 0;

    s->capturing = false;
    drop_pending(s);
    err = spi_control_reset(&s->spi);
    err = err ? err : receive(s, MSG_HELLO, 0, BOOT_TIMEOUT_MS);
    flush_data(s);
    if (err < 0) {
        return err;
    }
    *msg = s->msg;
    *len = s->msg_len;

    return 0;
}


/*
 * Sends a configuration message and waits for its reply. An accepted write of the capture
 * register starts the capture: from then on, a silent data channel is an event.
 */
static int config(void *src, const uint8_t *msg, size_t len, const uint8_t **reply,
                  size_t *reply_len)
{
    struct local *s = src;
    struct config_reply rep;
    const uint8_t *in = NULL;
    size_t in_len = 0;
    uint8_t reg = len > 1 ? msg[1] : 0;
    int err = spi_control_transfer(&s->spi, msg, len, &in, &in_len);

    err = err ? err : take_message(s, in, in_len, MSG_CONFIG_REPLY, reg);
    if (err == 0) {
        err = receive(s, MSG_CONFIG_REPLY, reg, REPLY_TIMEOUT_MS);
    } else if (err == 1) {
        err = 0;
    }
    if (err < 0) {
        return err;
    }

    if (reg == REG_CAPTURE && len > 2 && msg[2] == 1 &&
        config_reply_parse(s->msg, s->msg_len, &rep) == 0 && rep.status == 0) {
        pthread_mutex_lock(&s->lock);
        s->last_data = mono_ns();
        pthread_mutex_unlock(&s->lock);
        s->capturing = true;
        s->silent = false;
    }
    *reply = s->msg;
    *reply_len = s->msg_len;

    return 0;
}


/*
 * The next item, waiting up to timeout_ms: data bytes as read, messages the device sent,
 * and events (overflow, data error, data silent, control error). Times never go back.
 */
static int local_read(void *src, struct source_item *item, int timeout_ms)
{
    struct local *s = src;
    uint64_t end = mono_ns() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000000ULL;

    free(s->taken);
    s->taken = NULL;

    for (;;) {
        struct pollfd p[2] = {{.fd = s->wake, .events = POLLIN},
                              {.fd = s->spi.irq.fd, .events = POLLIN}};
        uint64_t now = 0;
        uint64_t wait = 0;
        uint64_t count = 0;
        int n = next_item(s, item);

        if (n != 0) {
            if (n > 0) {
                item->time = item->time > s->last_time ? item->time : s->last_time;
                s->last_time = item->time;
            }
            return n;
        }

        now = mono_ns();
        if (now >= end) {
            return 0;
        }
        wait = (end - now + 999999) / 1000000;
        if (s->capturing && !s->silent) {
            // Wake up in time to tell a silent data channel.
            wait = wait > DATA_SILENT_MS ? DATA_SILENT_MS : wait;
        }
        if (poll(p, 2, (int)wait) < 0 && errno != EINTR) {
            return -errno;
        }
        if (read(s->wake, &count, sizeof(count)) < 0) {
            count = 0;
        }
        gpio_drain_events(&s->spi.irq);
    }
}


// Ends the capture with a reset, and takes the hello that follows so IRQ is released.
static int stop(void *src)
{
    struct local *s = src;
    int err = 0;

    s->capturing = false;
    drop_pending(s);
    err = spi_control_reset(&s->spi);
    if (err == 0) {
        receive(s, MSG_HELLO, 0, BOOT_TIMEOUT_MS);
    }
    flush_data(s);

    return err;
}


// Stops the reader and lets every line go; the device is left as it is.
static void local_close(void *src)
{
    struct local *s = src;
    uint64_t one = 1;

    if (!s) {
        return;
    }
    if (s->reader_running) {
        if (write(s->quit, &one, sizeof(one)) < 0) {
            pthread_cancel(s->reader);
        }
        pthread_join(s->reader, NULL);
    }
    flush_data(s);
    drop_pending(s);
    if (s->uart >= 0) {
        close(s->uart);
    }
    if (s->wake >= 0) {
        close(s->wake);
    }
    if (s->quit >= 0) {
        close(s->quit);
    }
    spi_control_close(&s->spi);
    pthread_mutex_destroy(&s->lock);
    free(s->taken);
    free(s->msg);
    free(s);
}


/*
 * Waits for a message of type (for a configuration reply: about register reg) and keeps
 * it in s->msg. Other messages are kept for read. -ETIMEDOUT if none came.
 */
static int receive(struct local *s, uint8_t type, uint8_t reg, int timeout_ms)
{
    uint64_t end = mono_ns() + (uint64_t)timeout_ms * 1000000ULL;

    for (;;) {
        const uint8_t *in = NULL;
        size_t in_len = 0;
        uint64_t now = mono_ns();
        int err = now < end ? spi_control_wait(&s->spi, (int)((end - now) / 1000000)) : 0;

        if (err <= 0) {
            return err < 0 ? err : -ETIMEDOUT;
        }
        err = spi_control_transfer(&s->spi, NULL, 0, &in, &in_len);
        err = err ? err : take_message(s, in, in_len, type, reg);
        if (err != 0) {
            return err < 0 ? err : 0;
        }
    }
}


/*
 * A message from the device: 1 if it is the one waited for (now in s->msg), 0 if not or
 * if there was none (another is kept for read).
 */
static int take_message(struct local *s, const uint8_t *in, size_t len, uint8_t type,
                        uint8_t reg)
{
    uint8_t *copy = NULL;

    if (len == 0) {
        return 0;
    }
    if (in[0] != type || (type == MSG_CONFIG_REPLY && (len < 2 || in[1] != reg))) {
        return keep_pending(s, in, len);
    }
    copy = realloc(s->msg, len);
    if (!copy) {
        return -ENOMEM;
    }
    memcpy(copy, in, len);
    s->msg = copy;
    s->msg_len = len;

    return 1;
}


static int keep_pending(struct local *s, const uint8_t *in, size_t len)
{
    struct pending *p = malloc(sizeof(*p) + len);
    struct pending **at = &s->pending;

    if (!p) {
        return -ENOMEM;
    }
    p->next = NULL;
    p->time = mono_ns();
    p->len = len;
    memcpy(p->data, in, len);
    while (*at) {
        at = &(*at)->next;
    }
    *at = p;

    return 0;
}


static void drop_pending(struct local *s)
{
    while (s->pending) {
        struct pending *next = s->pending->next;

        free(s->pending);
        s->pending = next;
    }
}


/*
 * Drops the data read so far, after a reset: the UART's input is flushed first, then the
 * epoch moves on, so bytes the reader is still gathering from before are dropped too.
 */
static void flush_data(struct local *s)
{
    if (s->uart >= 0) {
        tcflush(s->uart, TCIFLUSH);
    }
    pthread_mutex_lock(&s->lock);
    s->epoch++;
    while (s->head) {
        struct block *next = s->head->next;

        free(s->head);
        s->head = next;
    }
    s->tail = NULL;
    s->queued = 0;
    s->dropped = 0;
    pthread_mutex_unlock(&s->lock);
}


/*
 * What there is to give out now, oldest first: a read error (as an event, then as the
 * error), dropped data, data, kept messages, a message IRQ announces, a silent data
 * channel. 1 with an item, 0 if nothing is due.
 */
static int next_item(struct local *s, struct source_item *item)
{
    struct block *b = NULL;
    uint64_t dropped = 0;
    uint64_t last_data = 0;
    int read_error = 0;

    pthread_mutex_lock(&s->lock);
    read_error = s->read_error;
    dropped = s->dropped;
    s->dropped = 0;
    b = s->head;
    if (b) {
        s->head = b->next;
        s->tail = s->head ? s->tail : NULL;
        s->queued -= b->len;
    }
    last_data = s->last_data;
    pthread_mutex_unlock(&s->lock);

    if (dropped) {
        uint8_t data[12] = {0};

        for (int i = 0; i < 8; i++) {
            data[i] = (uint8_t)(dropped >> (8 * i));
        }
        make_event(s, item, THC_EV_OVERFLOW, data, sizeof(data));
        // The block, if any, goes out on the next read.
        if (b) {
            pthread_mutex_lock(&s->lock);
            b->next = s->head;
            s->head = b;
            s->tail = s->tail ? s->tail : b;
            s->queued += b->len;
            pthread_mutex_unlock(&s->lock);
        }
        return 1;
    }
    if (b) {
        s->taken = b;
        s->silent = false;
        *item = (struct source_item){.type = THC_DATA, .channel = 0, .time = b->time,
                                     .data = b->data, .len = b->len};
        return 1;
    }
    if (read_error) {
        if (!s->error_told) {
            char data[EVENT_MAX - 2];
            int len = 0;

            for (int i = 0; i < 4; i++) {
                data[i] = (char)((uint32_t)read_error >> (8 * i));
            }
            len = snprintf(&data[4], sizeof(data) - 4, "reading %s: %s", BOARD_DATA_PORT,
                           strerror(-read_error));
            make_event(s, item, THC_EV_DATA_ERROR, data, 4 + (size_t)len);
            s->error_told = true;
            return 1;
        }
        return read_error;
    }

    if (s->pending) {
        struct pending *p = s->pending;
        uint8_t *copy = realloc(s->msg, p->len);

        if (!copy) {
            return -ENOMEM;
        }
        memcpy(copy, p->data, p->len);
        s->msg = copy;
        s->msg_len = p->len;
        s->pending = p->next;
        *item = (struct source_item){.type = THC_CONTROL, .time = p->time, .data = s->msg,
                                     .len = s->msg_len};
        free(p);
        return 1;
    }
    if (spi_control_pending(&s->spi)) {
        const uint8_t *in = NULL;
        size_t in_len = 0;
        int err = spi_control_transfer(&s->spi, NULL, 0, &in, &in_len);

        if (err < 0) {
            char text[EVENT_MAX - 2];
            int len = snprintf(text, sizeof(text), "reading a message: %s", strerror(-err));

            make_event(s, item, THC_EV_CONTROL_ERROR, text, (size_t)len);
            return 1;
        }
        if (in_len) {
            err = keep_pending(s, in, in_len);
            return err < 0 ? err : next_item(s, item);
        }
    }
    if (s->capturing && !s->silent && mono_ns() - last_data >= DATA_SILENT_MS * 1000000ULL) {
        uint32_t ms = (uint32_t)((mono_ns() - last_data) / 1000000);
        uint8_t data[4];

        for (int i = 0; i < 4; i++) {
            data[i] = (uint8_t)(ms >> (8 * i));
        }
        make_event(s, item, THC_EV_DATA_SILENT, data, sizeof(data));
        s->silent = true;
        return 1;
    }

    return 0;
}


// An event item: its code, then data. Data channel events are about data channel 0.
static void make_event(struct local *s, struct source_item *item, uint16_t code,
                       const void *data, size_t len)
{
    len = len > EVENT_MAX - 2 ? EVENT_MAX - 2 : len;
    s->event[0] = (uint8_t)code;
    s->event[1] = (uint8_t)(code >> 8);
    memcpy(&s->event[2], data, len);
    *item = (struct source_item){.type = THC_EVENT, .channel = 0, .time = mono_ns(),
                                 .data = s->event, .len = 2 + len};
}


/*
 * Reads the data UART until told to quit. Bytes gather into a block for up to
 * COALESCE_MS, then go to the queue; the block's time is when its last byte was read.
 */
static void *reader_main(void *arg)
{
    struct local *s = arg;
    struct block *b = NULL;
    uint64_t first = 0;

    for (;;) {
        struct pollfd p[2] = {{.fd = s->uart, .events = POLLIN},
                              {.fd = s->quit, .events = POLLIN}};
        int wait = -1;
        ssize_t n = 0;

        if (b) {
            uint64_t age = (mono_ns() - first) / 1000000;

            wait = age >= COALESCE_MS ? 0 : (int)(COALESCE_MS - age);
        }
        if (poll(p, 2, wait) < 0 && errno != EINTR) {
            break;
        }
        if (p[1].revents & POLLIN) {
            break;
        }
        if (p[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            pthread_mutex_lock(&s->lock);
            s->read_error = -EIO;
            pthread_mutex_unlock(&s->lock);
            break;
        }

        if (p[0].revents & POLLIN) {
            if (!b) {
                b = malloc(sizeof(*b));
                if (!b) {
                    break;
                }
                pthread_mutex_lock(&s->lock);
                b->epoch = s->epoch;
                pthread_mutex_unlock(&s->lock);
                b->len = 0;
                first = mono_ns();
            }
            n = read(s->uart, &b->data[b->len], BLOCK_SIZE - b->len);
            if (n < 0 && errno != EAGAIN && errno != EINTR) {
                pthread_mutex_lock(&s->lock);
                s->read_error = -errno;
                pthread_mutex_unlock(&s->lock);
                break;
            }
            if (n > 0) {
                b->len += (size_t)n;
                b->time = mono_ns();
            } else if (b->len == 0) {
                free(b);
                b = NULL;
            }
        }

        if (b && (b->len == BLOCK_SIZE || mono_ns() - first >= COALESCE_MS * 1000000ULL)) {
            publish(s, b);
            b = NULL;
        }
    }

    if (b && b->len) {
        publish(s, b);
    } else {
        free(b);
    }
    publish(s, NULL);

    return NULL;
}


// Queues a block (or with NULL only wakes the caller); bytes past QUEUE_MAX are dropped.
static void publish(struct local *s, struct block *b)
{
    uint64_t one = 1;

    pthread_mutex_lock(&s->lock);
    if (b && b->epoch != s->epoch) {
        free(b);
        b = NULL;
    } else if (b && s->queued + b->len > QUEUE_MAX) {
        s->dropped += b->len;
        s->last_data = b->time;
        free(b);
        b = NULL;
    } else if (b) {
        b->next = NULL;
        if (s->tail) {
            s->tail->next = b;
        } else {
            s->head = b;
        }
        s->tail = b;
        s->queued += b->len;
        s->last_data = b->time;
    }
    pthread_mutex_unlock(&s->lock);

    if (write(s->wake, &one, sizeof(one)) < 0) {
        // The counter is full: the caller is woken already.
    }
}


// The data UART: raw 8N1 at BOARD_DATA_SPEED, non-blocking, its input flushed.
static int uart_open(void)
{
    struct termios tio;
    int fd = open(BOARD_DATA_PORT, O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
    int err = 0;

    if (fd < 0) {
        return -errno;
    }
    if (tcgetattr(fd, &tio) < 0) {
        err = -errno;
    } else {
        cfmakeraw(&tio);
        cfsetispeed(&tio, BOARD_DATA_SPEED);
        cfsetospeed(&tio, BOARD_DATA_SPEED);
        tio.c_cflag |= CLOCAL | CREAD;
        tio.c_cflag &= ~(CSTOPB | CRTSCTS);
        tio.c_cc[VMIN] = 0;
        tio.c_cc[VTIME] = 0;
        if (tcsetattr(fd, TCSANOW, &tio) < 0) {
            err = -errno;
        }
    }
    if (err < 0) {
        close(fd);
        return err;
    }
    tcflush(fd, TCIOFLUSH);

    return fd;
}


static uint64_t mono_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
