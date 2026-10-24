/*
 * A capture as the device runs it: the accepted configuration and what follows from it by
 * the spec's rules (protocol/spec.tex, Mapping and Record). Nothing here assumes a channel
 * count, numbering or names; they come from the hello. Built by capture_info_init() in
 * capture/info.h.
 */

#ifndef COMMON_CAPTURE_INFO_H
#define COMMON_CAPTURE_INFO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


/*
 * The device's channel count N and data channel count D are from the hello; names has N
 * entries. The enabled and split sets are channel sets of set_size bytes (channel i is bit
 * i % 8 of byte i / 8).
 *
 * enabled lists the n enabled channels in ascending channel number: logical channel i is
 * channel enabled[i]. Data channel d carries logical channels block[d] to block[d + 1] - 1
 * (block has D + 1 entries), with tag[d] tag bits and records width[d] bytes wide.
 */
struct capture_info {
    uint32_t rate;
    uint32_t c;
    uint16_t channels;
    uint8_t data_channels;
    char **names;

    size_t set_size;
    uint8_t *enabled_set;
    uint8_t *split_set;

    uint16_t n;
    uint16_t *enabled;
    uint16_t *block;
    uint8_t *tag;
    uint8_t *width;
};


static inline bool channel_set_has(const uint8_t *set, unsigned int ch)
{
    return (set[ch / 8] >> (ch % 8)) & 1;
}


static inline void channel_set_put(uint8_t *set, unsigned int ch, bool in)
{
    if (in) {
        set[ch / 8] |= (uint8_t)(1U << (ch % 8));
    } else {
        set[ch / 8] &= (uint8_t)~(1U << (ch % 8));
    }
}

#endif
