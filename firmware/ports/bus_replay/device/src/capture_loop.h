/*
 * Shared by capture.c and capture_loop.S: step numbers and the offsets of the fields the
 * assembly loop uses in struct capture. capture.c checks the offsets at build time.
 */

#ifndef CAPTURE_LOOP_H
#define CAPTURE_LOOP_H

/*
 * Cleanup steps. ST_SCAN to ST_SCAN_DROP run inline in the assembly loop; the others
 * are C functions in capture.c.
 */
#define ST_SCAN 0
#define ST_RECORD 1
#define ST_ENCODE 2
#define ST_CRC 3
#define ST_SCAN_DROP 4
#define ST_FIRST 5
#define ST_HEARTBEAT_NOW 6
#define ST_RECORD_SLOW 7
#define ST_WRITE 8
#define ST_CHECK 9
#define ST_FLUSH_START 10
#define ST_HEARTBEAT 11
#define ST_HEARTBEAT_PACK 12
#define ST_HEARTBEAT_TRY 13
#define ST_LOSS_END 14
#define ST_FLUSH 15
#define ST_FLUSH_END 16
#define ST_SNAP 17
#define ST_STOP 18
#define ST_COUNT 19

#define HK_COUNT 16

#define RAW_BITS 12
#define RAW_SIZE (1 << RAW_BITS)
#define OUT_BITS 17
#define SCAN_MAX 3
#define HK_MASK 15
#define CRC_PER_STEP 2

// struct capture, first fields, all 32-bit.
#define CAP_TICK 0
#define CAP_MASK 4
#define CAP_HK 8
#define CAP_CHANGED 12
#define CAP_CUR 16
#define CAP_RAW 20
#define CAP_WR 24
#define CAP_RD 28
#define CAP_CRC 32
#define CAP_CRC_POS 36
#define CAP_ROOM_LIMIT 40
#define CAP_WIDTH 44
#define CAP_TAG_BITS 48
#define CAP_HB_SEQ 52
#define CAP_HB_TICK 56
#define CAP_GAP 60
#define CAP_BIT 64
#define CAP_HB_PENDING 68
#define CAP_LOSS_OPEN 72
#define CAP_OUT 76
#define CAP_CRC8 80
#define CAP_LAG_MAX 84
#define CAP_AFTER_FLUSH 88
#define CAP_AFTER_CRC 92
#define CAP_SLICE_ADDS 96
#define CAP_LOSS_START 100
#define CAP_LOSS_END 104
#define CAP_DROP_LAG 108
#define CAP_DROP_ROOM 112
#define CAP_HB_LEN 116
#define CAP_MAX_LATE 120
#define CAP_SLICES 124
#define SLICES 8
#define SLICE_FLUSH_AT 128
#define CAP_TAG (CAP_SLICES + 4 * SLICES)
#define CAP_REF_SEQ (CAP_TAG + 32)
#define CAP_REF_TICK (CAP_REF_SEQ + 4 * 32)
#define CAP_STEPS (CAP_REF_TICK + 4 * 32)
#define CAP_HK_STEPS (CAP_STEPS + 4 * ST_COUNT)

// struct debug_counters, profile tables (CAPTURE_PROFILE builds).
#define DBG_STEP_MAX 48
#define DBG_STEP_SUM (DBG_STEP_MAX + 4 * 64)
#define DBG_STEP_COUNT (DBG_STEP_SUM + 4 * 64)

#endif
