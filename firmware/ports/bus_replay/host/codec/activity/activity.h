/*
 * Activity: packets of records on any of a set of lines, split by idle gaps, whatever the
 * protocol. A summary item per packet: its records, the lines that changed, its length. Its
 * arguments: lines= (channels joined by +, default every captured one), gap= (seconds idle
 * on all of them that end a packet, default 300e-6), max= (seconds a packet is cut at,
 * default no limit).
 */

#ifndef CODEC_ACTIVITY_H
#define CODEC_ACTIVITY_H

#include "codec/codec.h"
#include "codec/item.h"


// Packet fields.
#define ACTIVITY_RECORDS 0
#define ACTIVITY_LINES 1
#define ACTIVITY_LENGTH 2

extern const struct codec_kind activity_packet;
extern const struct codec_decoder activity_decoder;

#endif
