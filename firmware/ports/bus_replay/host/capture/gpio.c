#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <unistd.h>

#include <linux/gpio.h>

#include "capture/gpio.h"


#define CONSUMER "bus-replay"


static uint64_t mode_flags(enum gpio_mode mode);


int gpio_open(const char *chip)
{
    int fd = open(chip, O_RDWR | O_CLOEXEC);

    return fd < 0 ? -errno : fd;
}


/*
 * Requests count lines. Line i is offsets[i] with modes[i]; bit i of values is its initial
 * output level. Bits in the other calls are line indexes too.
 */
int gpio_request(int chip, unsigned int count, const unsigned int *offsets,
                 const enum gpio_mode *modes, uint64_t values, struct gpio_lines *lines)
{
    struct gpio_v2_line_request req = {0};
    uint64_t outputs = 0;
    unsigned int attrs = 0;

    lines->fd = -1;
    if (count == 0 || count > GPIO_LINES_MAX) {
        return -EINVAL;
    }

    strncpy(req.consumer, CONSUMER, sizeof(req.consumer) - 1);
    req.num_lines = count;
    req.config.flags = mode_flags(modes[0]);
    for (unsigned int i = 0; i < count; i++) {
        req.offsets[i] = offsets[i];
        if (modes[i] == GPIO_OUTPUT || modes[i] == GPIO_OPEN_DRAIN) {
            outputs |= 1ULL << i;
        }
        if (modes[i] != modes[0]) {
            // One attribute per line whose mode differs from the default.
            req.config.attrs[attrs].attr.id = GPIO_V2_LINE_ATTR_ID_FLAGS;
            req.config.attrs[attrs].attr.flags = mode_flags(modes[i]);
            req.config.attrs[attrs].mask = 1ULL << i;
            attrs++;
        }
    }
    if (outputs) {
        req.config.attrs[attrs].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
        req.config.attrs[attrs].attr.values = values & outputs;
        req.config.attrs[attrs].mask = outputs;
        attrs++;
    }
    req.config.num_attrs = attrs;

    if (ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
        return -errno;
    }
    lines->fd = req.fd;
    lines->count = count;

    return 0;
}


int gpio_set(const struct gpio_lines *lines, uint64_t mask, uint64_t bits)
{
    struct gpio_v2_line_values v = {.bits = bits, .mask = mask};

    return ioctl(lines->fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v) < 0 ? -errno : 0;
}


int gpio_get(const struct gpio_lines *lines, uint64_t mask, uint64_t *bits)
{
    struct gpio_v2_line_values v = {.mask = mask};

    if (ioctl(lines->fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &v) < 0) {
        return -errno;
    }
    *bits = v.bits;

    return 0;
}


void gpio_drain_events(const struct gpio_lines *lines)
{
    struct gpio_v2_line_event ev[16];
    struct pollfd p = {.fd = lines->fd, .events = POLLIN};

    while (poll(&p, 1, 0) > 0 && (p.revents & POLLIN)) {
        if (read(lines->fd, ev, sizeof(ev)) <= 0) {
            return;
        }
    }
}


// Every line back to an input before the request goes, so nothing is left driven.
void gpio_release(struct gpio_lines *lines)
{
    struct gpio_v2_line_config cfg = {.flags = GPIO_V2_LINE_FLAG_INPUT};

    if (lines->fd < 0) {
        return;
    }
    ioctl(lines->fd, GPIO_V2_LINE_SET_CONFIG_IOCTL, &cfg);
    close(lines->fd);
    lines->fd = -1;
}


static uint64_t mode_flags(enum gpio_mode mode)
{
    switch (mode) {
    case GPIO_INPUT_FALLING:
        return GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_EDGE_FALLING;
    case GPIO_OUTPUT:
        return GPIO_V2_LINE_FLAG_OUTPUT;
    case GPIO_OPEN_DRAIN:
        return GPIO_V2_LINE_FLAG_OUTPUT | GPIO_V2_LINE_FLAG_OPEN_DRAIN;
    default:
        return GPIO_V2_LINE_FLAG_INPUT;
    }
}
