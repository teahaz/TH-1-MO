/*
 * The SPI control binding (protocol/spec.tex, SPI) from the Pi, with the device's pin
 * reset: a bit-banged SPI master on GPIO lines, and the IRQ line.
 *
 * SCLK, MOSI and CS are requested for one transaction at a time and are inputs otherwise,
 * nRESET only for its pulse, so a killed process leaves nothing driven. IRQ is held as an
 * input that reports falling edges; irq.fd can be polled for them.
 */

#ifndef CAPTURE_SPI_H
#define CAPTURE_SPI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "capture/gpio.h"


#define SPI_MSG_MAX 0xFFFE

struct spi_control {
    int chip;
    struct gpio_lines irq;
    uint8_t *tx;
    uint8_t *rx;
};


int spi_control_open(struct spi_control *s);
void spi_control_close(struct spi_control *s);
int spi_control_reset(struct spi_control *s);
bool spi_control_pending(const struct spi_control *s);
int spi_control_wait(struct spi_control *s, int timeout_ms);
int spi_control_transfer(struct spi_control *s, const uint8_t *msg, size_t len,
                         const uint8_t **in, size_t *in_len);

#endif
