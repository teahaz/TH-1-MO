#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "capture/control.h"
#include "capture/info.h"
#include "common/capture_info.h"


static int mapping(struct capture_info *info);


/*
 * Fills info for a capture of hello's device at rate and c, with the given enabled and
 * split channel sets (ceil(N/8) bytes each; split may be NULL for empty). Returns -EINVAL
 * for a set naming a channel the device does not have, or a split set of more than D - 1
 * channels (spec, Mapping), -ENOMEM if out of memory.
 */
int capture_info_init(struct capture_info *info, const struct hello *h, uint32_t rate,
                      uint32_t c, const uint8_t *enabled_set, const uint8_t *split_set)
{
    int err = 0;

    memset(info, 0, sizeof(*info));
    if (h->data_channels == 0) {
        return -EINVAL;
    }
    info->rate = rate;
    info->c = c;
    info->channels = h->channels;
    info->data_channels = h->data_channels;
    info->set_size = ((size_t)h->channels + 7) / 8;

    info->names = calloc(h->channels ? h->channels : 1, sizeof(*info->names));
    info->enabled_set = calloc(info->set_size ? info->set_size : 1, 1);
    info->split_set = calloc(info->set_size ? info->set_size : 1, 1);
    if (!info->names || !info->enabled_set || !info->split_set) {
        err = -ENOMEM;
        goto fail;
    }
    for (unsigned int i = 0; i < h->channels; i++) {
        info->names[i] = strdup(h->names[i]);
        if (!info->names[i]) {
            err = -ENOMEM;
            goto fail;
        }
    }
    memcpy(info->enabled_set, enabled_set, info->set_size);
    if (split_set) {
        memcpy(info->split_set, split_set, info->set_size);
    }

    // Bits past channel N - 1 name channels the device does not have.
    for (unsigned int ch = h->channels; ch < 8 * info->set_size; ch++) {
        if (channel_set_has(info->enabled_set, ch) || channel_set_has(info->split_set, ch)) {
            err = -EINVAL;
            goto fail;
        }
    }

    err = mapping(info);
    if (err < 0) {
        goto fail;
    }

    return 0;

fail:
    capture_info_free(info);
    return err;
}


// A capture info with the hello's current register values: the device's defaults.
int capture_info_from_hello(struct capture_info *info, const struct hello *h)
{
    const struct reg *rate = hello_reg(h, REG_SAMPLE_RATE);
    const struct reg *c = hello_reg(h, REG_C);
    const struct reg *enabled = hello_reg(h, REG_ENABLED);
    const struct reg *split = hello_reg(h, REG_SPLIT);
    size_t set_size = ((size_t)h->channels + 7) / 8;

    if (!rate || !c || !enabled || enabled->size != set_size ||
        (split && split->size != set_size)) {
        return -EPROTO;
    }

    return capture_info_init(info, h, reg_u32(rate), reg_u32(c), enabled->value,
                             split ? split->value : NULL);
}


// A copy of src that owns everything it points at, for capture_info_free().
int capture_info_copy(struct capture_info *dst, const struct capture_info *src)
{
    struct hello h = {.channels = src->channels, .data_channels = src->data_channels,
                      .names = src->names};

    return capture_info_init(dst, &h, src->rate, src->c, src->enabled_set, src->split_set);
}


void capture_info_free(struct capture_info *info)
{
    if (info->names) {
        for (unsigned int i = 0; i < info->channels; i++) {
            free(info->names[i]);
        }
        free(info->names);
    }
    free(info->enabled_set);
    free(info->split_set);
    free(info->enabled);
    free(info->block);
    free(info->tag);
    free(info->width);
    memset(info, 0, sizeof(*info));
}


// Bits of the channel tag for a block of b logical channels: ceil(log2 b), 0 for 0 or 1.
unsigned int tag_bits(unsigned int b)
{
    unsigned int t = 0;

    while ((1U << t) < b) {
        t++;
    }

    return t;
}


// W: the smallest number of bytes that holds (2C + 1) * 2^t - 1 (spec, Record).
unsigned int record_width(uint32_t c, unsigned int t)
{
    uint64_t max = ((2 * (uint64_t)c + 1) << t) - 1;
    unsigned int width = 1;

    while (width < 8 && max >= (1ULL << (8 * width))) {
        width++;
    }

    return width;
}


/*
 * The enabled channels in order, and their blocks (spec, Mapping): automatic for an empty
 * split set, otherwise a new block at every channel in it.
 */
static int mapping(struct capture_info *info)
{
    unsigned int d_count = info->data_channels;
    unsigned int splits = 0;
    unsigned int d = 0;
    unsigned int i = 0;

    for (unsigned int ch = 0; ch < info->channels; ch++) {
        info->n += channel_set_has(info->enabled_set, ch);
        splits += channel_set_has(info->split_set, ch);
    }
    if (splits > d_count - 1) {
        return -EINVAL;
    }

    info->enabled = calloc(info->n ? info->n : 1, sizeof(*info->enabled));
    info->block = calloc(d_count + 1, sizeof(*info->block));
    info->tag = calloc(d_count, sizeof(*info->tag));
    info->width = calloc(d_count, sizeof(*info->width));
    if (!info->enabled || !info->block || !info->tag || !info->width) {
        return -ENOMEM;
    }
    for (unsigned int ch = 0; ch < info->channels; ch++) {
        if (channel_set_has(info->enabled_set, ch)) {
            info->enabled[i++] = (uint16_t)ch;
        }
    }

    if (splits == 0) {
        unsigned int at = 0;

        for (d = 0; d < d_count; d++) {
            info->block[d] = (uint16_t)at;
            at += info->n / d_count + (d < info->n % d_count ? 1 : 0);
        }
    } else {
        // Block d + 1 starts at the first enabled channel at or after the d-th split channel.
        i = 0;
        d = 0;
        info->block[0] = 0;
        for (unsigned int ch = 0; ch < info->channels; ch++) {
            if (channel_set_has(info->split_set, ch)) {
                while (i < info->n && info->enabled[i] < ch) {
                    i++;
                }
                info->block[++d] = (uint16_t)i;
            }
        }
        for (d = d + 1; d < d_count; d++) {
            info->block[d] = info->n;
        }
    }
    info->block[d_count] = info->n;

    for (d = 0; d < d_count; d++) {
        info->tag[d] = (uint8_t)tag_bits(info->block[d + 1] - info->block[d]);
        info->width[d] = (uint8_t)record_width(info->c, info->tag[d]);
    }

    return 0;
}
