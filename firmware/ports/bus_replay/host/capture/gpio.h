/*
 * Pi GPIO through the kernel's character device (linux/gpio.h, uAPI v2), so no libgpiod is
 * needed. A request holds a few lines; releasing it returns every line to an input first.
 */

#ifndef CAPTURE_GPIO_H
#define CAPTURE_GPIO_H

#include <stdint.h>


#define GPIO_LINES_MAX 8

/*
 * GPIO_INPUT_FALLING is an input that also reports falling edges as events on the
 * request's fd, so a caller can poll() it.
 */
enum gpio_mode {
    GPIO_INPUT,
    GPIO_INPUT_FALLING,
    GPIO_OUTPUT,
    GPIO_OPEN_DRAIN,
};

struct gpio_lines {
    int fd;
    unsigned int count;
};


int gpio_open(const char *chip);
int gpio_request(int chip, unsigned int count, const unsigned int *offsets,
                 const enum gpio_mode *modes, uint64_t values, struct gpio_lines *lines);
int gpio_set(const struct gpio_lines *lines, uint64_t mask, uint64_t bits);
int gpio_get(const struct gpio_lines *lines, uint64_t mask, uint64_t *bits);
void gpio_drain_events(const struct gpio_lines *lines);
void gpio_release(struct gpio_lines *lines);

#endif
