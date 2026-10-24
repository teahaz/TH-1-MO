#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <time.h>
#include <unistd.h>

#include <bits/time.h>

#include "capture/board.h"
#include "capture/gpio.h"
#include "capture/spi.h"


#define RESET_LOW_MS 10
#define NOT_READY_RETRIES 50

// A transaction's frame: Length, Message, Check.
#define FRAME_MAX (SPI_MSG_MAX + 4)

// Line indexes in a transaction's request.
#define SPI_SCLK (1ULL << 0)
#define SPI_MOSI (1ULL << 1)
#define SPI_CS (1ULL << 2)
#define SPI_MISO (1ULL << 3)

#define NOT_READY 1


static int transaction(struct spi_control *s, size_t tx_len, size_t *in_len);
static uint8_t spi_byte(const struct gpio_lines *spi, uint8_t out);
static uint16_t crc16(const uint8_t *data, size_t len);
static void sleep_ms(int ms);
static int64_t now_ms(void);


int spi_control_open(struct spi_control *s)
{
    static const unsigned int irq_pin = BOARD_PIN_IRQ;
    static const enum gpio_mode irq_mode = GPIO_INPUT_FALLING;
    int err = 0;

    s->irq.fd = -1;
    s->tx = malloc(FRAME_MAX);
    s->rx = malloc(FRAME_MAX);
    s->chip = gpio_open(BOARD_GPIO_CHIP);
    if (!s->tx || !s->rx) {
        err = -ENOMEM;
    } else if (s->chip < 0) {
        err = s->chip;
    } else {
        err = gpio_request(s->chip, 1, &irq_pin, &irq_mode, 0, &s->irq);
    }
    if (err < 0) {
        spi_control_close(s);
    }

    return err;
}


void spi_control_close(struct spi_control *s)
{
    gpio_release(&s->irq);
    if (s->chip >= 0) {
        close(s->chip);
        s->chip = -1;
    }
    free(s->tx);
    free(s->rx);
    s->tx = NULL;
    s->rx = NULL;
}


// nRESET low for a moment, open drain, then back to an input.
int spi_control_reset(struct spi_control *s)
{
    static const unsigned int pin = BOARD_PIN_RESET;
    static const enum gpio_mode mode = GPIO_OPEN_DRAIN;
    struct gpio_lines line;
    int err = gpio_request(s->chip, 1, &pin, &mode, 0, &line);

    if (err < 0) {
        return err;
    }
    sleep_ms(RESET_LOW_MS);
    gpio_release(&line);
    gpio_drain_events(&s->irq);

    return 0;
}


// True while the device holds IRQ low: a message waits.
bool spi_control_pending(const struct spi_control *s)
{
    uint64_t bits = 1;

    if (gpio_get(&s->irq, 1, &bits) < 0) {
        return false;
    }

    return (bits & 1) == 0;
}


// Waits up to timeout_ms for IRQ: 1 once a message waits, 0 if none came.
int spi_control_wait(struct spi_control *s, int timeout_ms)
{
    int64_t end = now_ms() + timeout_ms;

    for (;;) {
        struct pollfd p = {.fd = s->irq.fd, .events = POLLIN};
        int64_t left = end - now_ms();

        gpio_drain_events(&s->irq);
        if (spi_control_pending(s)) {
            return 1;
        }
        if (left <= 0) {
            return 0;
        }
        if (poll(&p, 1, (int)left) < 0 && errno != EINTR) {
            return -errno;
        }
    }
}


/*
 * One message each way (spec, SPI): sends msg (len 0 for none) and gives the device's
 * message in *in, *in_len 0 if it sent none. *in is valid until the next call. Retries
 * while the device is not ready; -EAGAIN if it never is, -EBADMSG for a failed check,
 * -EPROTO for a Length over the maximum.
 */
int spi_control_transfer(struct spi_control *s, const uint8_t *msg, size_t len,
                         const uint8_t **in, size_t *in_len)
{
    size_t tx_len = 2;
    int err = 0;

    *in = NULL;
    *in_len = 0;
    if (len > SPI_MSG_MAX) {
        return -EINVAL;
    }
    s->tx[0] = (uint8_t)len;
    s->tx[1] = (uint8_t)(len >> 8);
    if (len) {
        uint16_t crc = 0;

        memcpy(&s->tx[2], msg, len);
        crc = crc16(s->tx, 2 + len);
        s->tx[2 + len] = (uint8_t)crc;
        s->tx[3 + len] = (uint8_t)(crc >> 8);
        tx_len = len + 4;
    }

    for (int i = 0; i < NOT_READY_RETRIES; i++) {
        err = transaction(s, tx_len, in_len);
        if (err != NOT_READY) {
            break;
        }
        sleep_ms(1);
    }
    if (err == NOT_READY) {
        return -EAGAIN;
    }
    if (err == 0 && *in_len) {
        *in = &s->rx[2];
    }

    return err;
}


/*
 * Clocks tx_len bytes of s->tx out, padded with 0x00 until the device's frame is through
 * too, into s->rx. NOT_READY on Length 0xFFFF.
 */
static int transaction(struct spi_control *s, size_t tx_len, size_t *in_len)
{
    static const unsigned int pins[] = {BOARD_PIN_SCLK, BOARD_PIN_MOSI, BOARD_PIN_CS,
                                        BOARD_PIN_MISO};
    static const enum gpio_mode modes[] = {GPIO_OUTPUT, GPIO_OUTPUT, GPIO_OUTPUT, GPIO_INPUT};
    struct gpio_lines spi;
    size_t total = 0;
    uint16_t dlen = 0;
    int err = 0;

    *in_len = 0;
    // CS high (idle) from the start, so the request itself does not select the device.
    err = gpio_request(s->chip, 4, pins, modes, SPI_CS, &spi);
    if (err < 0) {
        return err;
    }

    gpio_set(&spi, SPI_CS, 0);
    s->rx[0] = spi_byte(&spi, s->tx[0]);
    s->rx[1] = spi_byte(&spi, s->tx[1]);
    dlen = (uint16_t)(s->rx[0] | s->rx[1] << 8);
    if (dlen == 0xFFFF) {
        err = NOT_READY;
    } else {
        total = dlen ? (size_t)dlen + 4 : 2;
        total = tx_len > total ? tx_len : total;
        for (size_t i = 2; i < total; i++) {
            s->rx[i] = spi_byte(&spi, i < tx_len ? s->tx[i] : 0);
        }
    }
    gpio_set(&spi, SPI_SCLK | SPI_MOSI | SPI_CS, SPI_CS);
    gpio_release(&spi);

    if (err != 0 || dlen == 0) {
        return err;
    }
    if (crc16(s->rx, 2 + dlen) != (uint16_t)(s->rx[2 + dlen] | s->rx[3 + dlen] << 8)) {
        return -EBADMSG;
    }
    *in_len = dlen;

    return 0;
}


// Mode 0, MSB first: data out with SCLK low, sampled after the rising edge.
static uint8_t spi_byte(const struct gpio_lines *spi, uint8_t out)
{
    uint64_t bits = 0;
    uint8_t got = 0;

    for (int i = 7; i >= 0; i--) {
        gpio_set(spi, SPI_SCLK | SPI_MOSI, (out >> i) & 1 ? SPI_MOSI : 0);
        gpio_set(spi, SPI_SCLK, SPI_SCLK);
        gpio_get(spi, SPI_MISO, &bits);
        got = (uint8_t)(got << 1 | ((bits & SPI_MISO) ? 1 : 0));
    }

    return got;
}


// CRC-16/CCITT-FALSE: polynomial 0x1021, initial 0xFFFF.
static uint16_t crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;

    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)(data[i] << 8);
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }

    return crc;
}


static void sleep_ms(int ms)
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};

    nanosleep(&ts, NULL);
}


static int64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
