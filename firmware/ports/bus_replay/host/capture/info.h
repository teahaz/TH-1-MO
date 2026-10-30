/*
 * Builds a capture info (common/capture_info.h) from the hello and the configuration the
 * device accepted.
 */

#ifndef CAPTURE_INFO_H
#define CAPTURE_INFO_H

#include <stdint.h>

#include "capture/control.h"
#include "common/capture_info.h"


int capture_info_init(struct capture_info *info, const struct hello *h, uint32_t rate,
                      uint32_t c, const uint8_t *enabled_set, const uint8_t *split_set);
int capture_info_from_hello(struct capture_info *info, const struct hello *h);
int capture_info_copy(struct capture_info *dst, const struct capture_info *src);
void capture_info_free(struct capture_info *info);
unsigned int tag_bits(unsigned int b);
unsigned int record_width(uint32_t c, unsigned int t);

#endif
