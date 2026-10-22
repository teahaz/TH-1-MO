/*
 * The capture loop. Every tick: spin on the DWT cycle counter until the deadline, store
 * P0->IN in the raw ring, then run one step.
 *
 * - Every HK_MASK + 1-th tick runs the housekeeping step c->hk_steps[c->hk].
 * - Otherwise the cleanup step runs. The common ones are here, inline:
 *   ST_SCAN       up to SCAN_MAX samples; with nothing to scan, a spare CRC pass
 *   ST_RECORD     drop check, the changed pin, its gap and the record's value; or the
 *                 sample's drop, which switches to ST_SCAN_DROP
 *   ST_ENCODE     the record into the output ring, and the next step
 *   ST_CRC        the running CRC-8 over the bytes written since it last ran
 *   ST_SCAN_DROP  while records cannot be written (cleanup lagging, no room, or a
 *                 heartbeat pending): scan, dropping each changed sample
 *   The rest are C functions in capture.c, c->steps[step].
 *
 * The C steps share r4 to r11 with this loop as global register variables
 * (capture.c):
 *
 *   r4  c            struct capture
 *   r5  deadline     the next tick's deadline, in DWT cycles
 *   r6  step         the next cleanup step
 *   r7  tick_cycles  CPU cycles per tick
 *   r8  idx          the tick count
 *   r9  pos          the sample being scanned
 *   r10 due          samples left before the next grid-point sample
 *   r11 prev         the previous scanned sample, masked
 *
 * The loop runs from the Code RAM alias, so it branches only PC-relative or by
 * register, and loads constants from its own literal pool.
 *
 * Entered from capture_enter() with r0 = c, which saves r4 to r11 for the caller.
 * Returns when a step sets step to ST_STOP.
 */

#include "capture_loop.h"

#define DWT_CYCCNT 0xE0001004
#define P0_BASE 0x50000000
#define P0_IN 0x510

// Without profiling, the end of a pass is the start of the next.
#ifndef CAPTURE_PROFILE
#define pass_end tick
#endif


    .syntax unified
    .thumb
    .section .ramfunc, "ax", %progbits
    .align 2
    .global capture_loop
    .type capture_loop, %function
    .thumb_func
capture_loop:
    // [sp]: the pass's start cycle, [sp, #4]: its profile index (profile builds).
    push    {r0-r2, lr}
    mov     r4, r0
    movs    r6, #ST_FIRST
    ldr     r7, [r4, #CAP_TICK]
    movs    r0, #0
    mov     r8, r0
    mov     r9, r0
    mov     r10, r0
    mov     r11, r0
    ldr     r1, =DWT_CYCCNT
    ldr     r0, [r1]
    add     r5, r0, r7

tick:
    ldr     r1, =DWT_CYCCNT
1:
    ldr     r0, [r1]
    subs    r0, r0, r5
    bmi     1b

    // Sample: raw[idx % RAW_SIZE] = P0->IN. r0 is how late it was taken.
    mov     r2, #P0_BASE
    ldr     r2, [r2, #P0_IN]
    ldr     r3, [r4, #CAP_RAW]
    ubfx    r1, r8, #0, #RAW_BITS
    str     r2, [r3, r1, lsl #2]
#ifdef CAPTURE_PROFILE
    add     r2, r0, r5
    str     r2, [sp]
    str     r6, [sp, #4]
#endif
    add     r8, r8, #1
    add     r5, r5, r7

    tst     r8, #HK_MASK
    beq     housekeeping
    cbnz    r6, not_scan

scan:
    // ST_SCAN (r3 = raw): stop at a change or a grid-point sample.
    ldr     r12, [r4, #CAP_MASK]
    sub     r0, r8, r9
    cmp     r0, #SCAN_MAX
    it      hi
    movhi   r0, #SCAN_MAX
    cbz     r0, scan_idle
2:
    ubfx    r1, r9, #0, #RAW_BITS
    ldr     r2, [r3, r1, lsl #2]
    and     r2, r2, r12
    cmp     r2, r11
    bne     scan_change
    cmp     r10, #0
    beq     scan_grid
    sub     r10, r10, #1
    add     r9, r9, #1
    subs    r0, r0, #1
    bne     2b
    b       pass_end

scan_change:
    eor     r1, r2, r11
    str     r1, [r4, #CAP_CHANGED]
    str     r2, [r4, #CAP_CUR]
    mov     r11, r2
    movs    r6, #ST_RECORD
    b       pass_end

scan_grid:
    str     r2, [r4, #CAP_CUR]
    movs    r6, #ST_HEARTBEAT
    b       pass_end

scan_idle:
    // Caught up: the spare pass goes to the CRC, if any bytes wait for it.
    ldr     r0, [r4, #CAP_CRC_POS]
    ldr     r1, [r4, #CAP_WR]
    cmp     r0, r1
    bne     crc_bytes
    b       pass_end

not_scan:
    cmp     r6, #ST_SCAN_DROP
    bhi     call_step
    tbb     [pc, r6]
3:
    .byte   0
    .byte   (st_record - 3b) / 2
    .byte   (st_encode - 3b) / 2
    .byte   (st_crc - 3b) / 2
    .byte   (st_scan_drop - 3b) / 2
    .align  1

st_record:
    // ST_RECORD: one drop check per record (README, Dropping records).
    ldr     r0, [r4, #CAP_CHANGED]
    sub     r1, r8, r9
    ldr     r2, [r4, #CAP_LAG_MAX]
    cmp     r1, r2
    bhi     drop_lag
    ldr     r2, [r4, #CAP_WR]
    ldr     r3, [r4, #CAP_RD]
    sub     r2, r2, r3
    ldr     r1, [r4, #CAP_HB_PENDING]
    cbnz    r1, record_pending
    ldr     r3, [r4, #CAP_ROOM_LIMIT]
    cmp     r2, r3
    bhi     drop_room

    // The lowest changed pin b, cleared from changed.
    rbit    r1, r0
    clz     r1, r1
    sub     r2, r0, #1
    and     r0, r0, r2
    str     r0, [r4, #CAP_CHANGED]

    // Gap: from the pin's last record if in this heartbeat epoch, else the heartbeat.
    add     r3, r4, r1, lsl #2
    ldr     r2, [r3, #CAP_REF_SEQ]
    ldr     r12, [r4, #CAP_HB_SEQ]
    cmp     r2, r12
    ite     eq
    ldreq   r2, [r3, #CAP_REF_TICK]
    ldrne   r2, [r4, #CAP_HB_TICK]
    str     r12, [r3, #CAP_REF_SEQ]
    str     r9, [r3, #CAP_REF_TICK]
    sub     r2, r9, r2

    // Value: ZigZag (2 * gap for level 1, 2 * gap - 1 for level 0), tag below it.
    ldr     r0, [r4, #CAP_CUR]
    lsr     r0, r0, r1
    and     r0, r0, #1
    eor     r0, r0, #1
    rsb     r2, r0, r2, lsl #1
    ldr     r0, [r4, #CAP_TAG_BITS]
    lsl     r2, r2, r0
    add     r3, r4, r1
    ldrb    r3, [r3, #CAP_TAG]
    orr     r2, r2, r3
    str     r2, [r4, #CAP_GAP]
    movs    r6, #ST_ENCODE
    b       pass_end

record_pending:
    // A pending heartbeat goes first (ST_RECORD_SLOW) if it fits: used + len < size.
    ldr     r3, [r4, #CAP_HB_LEN]
    add     r3, r2, r3
    lsrs    r3, r3, #OUT_BITS
    bne     drop_room
    movs    r6, #ST_RECORD_SLOW
    b       pass_end

drop_lag:
    movs    r1, #CAP_DROP_LAG
    b       drop
drop_room:
    movs    r1, #CAP_DROP_ROOM
drop:
    // r0: the sample's changed pins, all dropped together; r1: the reason's counter.
    // Bit-sliced counters: add 1 for every pin in r0.
    ldr     r2, [r4, r1]
    add     r2, r2, #1
    str     r2, [r4, r1]
    add     r3, r4, #CAP_SLICES
4:
    ldr     r2, [r3]
    and     r12, r2, r0
    eor     r2, r2, r0
    str     r2, [r3], #4
    movs    r0, r12
    bne     4b
    str     r0, [r4, #CAP_CHANGED]

    // The loss: its first and latest drops, as ticks.
    ldr     r1, [r4, #CAP_LOSS_OPEN]
    cbnz    r1, 5f
    movs    r1, #1
    str     r1, [r4, #CAP_LOSS_OPEN]
    str     r9, [r4, #CAP_LOSS_START]
5:
    str     r9, [r4, #CAP_LOSS_END]

    // Next: the sample's heartbeat at a grid point, or dropping on.
    movs    r0, #ST_HEARTBEAT
    cmp     r10, #0
    beq     6f
    sub     r10, r10, #1
    add     r9, r9, #1
    movs    r0, #ST_SCAN_DROP
6:
    // Flush the counters before they can overflow, then r0.
    ldr     r1, [r4, #CAP_SLICE_ADDS]
    add     r1, r1, #1
    str     r1, [r4, #CAP_SLICE_ADDS]
    cmp     r1, #SLICE_FLUSH_AT
    bhs     7f
    mov     r6, r0
    b       pass_end
7:
    str     r0, [r4, #CAP_AFTER_FLUSH]
    movs    r6, #ST_FLUSH_START
    b       pass_end

st_scan_drop:
    // ST_SCAN_DROP: first, can records be written again?
    ldr     r2, [r4, #CAP_WR]
    ldr     r3, [r4, #CAP_RD]
    sub     r2, r2, r3
    ldr     r1, [r4, #CAP_HB_PENDING]
    cbz     r1, 8f
    // A pending heartbeat goes first once it fits (ST_HEARTBEAT_NOW).
    ldr     r3, [r4, #CAP_HB_LEN]
    add     r3, r2, r3
    lsrs    r3, r3, #OUT_BITS
    bne     scan_drop_room
    movs    r6, #ST_HEARTBEAT_NOW
    b       pass_end
8:
    ldr     r3, [r4, #CAP_ROOM_LIMIT]
    cmp     r2, r3
    bhi     scan_drop_room
    sub     r1, r8, r9
    ldr     r3, [r4, #CAP_LAG_MAX]
    cmp     r1, r3
    bhi     scan_drop_lag
    // Room and no lag: back to recording, starting with this pass's scan.
    movs    r6, #ST_SCAN
    ldr     r3, [r4, #CAP_RAW]
    b       scan

scan_drop_lag:
    movs    r1, #CAP_DROP_LAG
    b       9f
scan_drop_room:
    movs    r1, #CAP_DROP_ROOM
9:
    // Scan up to SCAN_MAX samples; drop the first change (r1: the reason's counter).
    ldr     r3, [r4, #CAP_RAW]
    ldr     r12, [r4, #CAP_MASK]
    sub     r0, r8, r9
    cmp     r0, #SCAN_MAX
    it      hi
    movhi   r0, #SCAN_MAX
    cbz     r0, scan_drop_none
10:
    ubfx    r2, r9, #0, #RAW_BITS
    ldr     r2, [r3, r2, lsl #2]
    and     r2, r2, r12
    cmp     r2, r11
    bne     scan_drop_change
    cmp     r10, #0
    beq     scan_grid
    sub     r10, r10, #1
    add     r9, r9, #1
    subs    r0, r0, #1
    bne     10b
scan_drop_none:
    b       pass_end

scan_drop_change:
    // Levels at a grid point go in its heartbeat (spec 6.1), as for a record.
    str     r2, [r4, #CAP_CUR]
    eor     r0, r2, r11
    mov     r11, r2
    b       drop

st_encode:
    // ST_ENCODE: one 32-bit store; only W bytes count, the rest is free room that
    // later writes overwrite. Near the end of the ring, byte by byte.
    ldr     r2, [r4, #CAP_GAP]
    ldr     r0, [r4, #CAP_WR]
    ldr     r1, [r4, #CAP_OUT]
    ubfx    r12, r0, #0, #OUT_BITS
    add     r3, r12, #3
    lsrs    r3, r3, #OUT_BITS
    ldr     r3, [r4, #CAP_WIDTH]
    bne     encode_bytes
    str     r2, [r1, r12]
    add     r0, r0, r3
encode_done:
    str     r0, [r4, #CAP_WR]

    // Next: the sample's next change, its heartbeat at a grid point, or the next sample.
    ldr     r0, [r4, #CAP_CHANGED]
    cbnz    r0, encode_more
    movs    r0, #ST_HEARTBEAT
    cmp     r10, #0
    beq     encode_next
    sub     r10, r10, #1
    add     r9, r9, #1
    movs    r0, #ST_SCAN
encode_next:
    // Writing an entry ends an open loss (spec 7.2): ST_LOSS_END, then r0.
    ldr     r1, [r4, #CAP_LOSS_OPEN]
    cbnz    r1, encode_loss
    mov     r6, r0
    b       pass_end

encode_more:
    movs    r0, #ST_RECORD
    b       encode_next

encode_loss:
    str     r0, [r4, #CAP_AFTER_FLUSH]
    movs    r6, #ST_LOSS_END
    b       pass_end

encode_bytes:
    ubfx    r12, r0, #0, #OUT_BITS
    strb    r2, [r1, r12]
    add     r0, r0, #1
    lsr     r2, r2, #8
    subs    r3, r3, #1
    bne     encode_bytes
    b       encode_done

st_crc:
    // ST_CRC: once the CRC has caught up with wr, on to c->after_crc.
    ldr     r0, [r4, #CAP_CRC_POS]
    ldr     r1, [r4, #CAP_WR]
    cmp     r0, r1
    bne     crc_bytes
    ldr     r6, [r4, #CAP_AFTER_CRC]
    b       pass_end

crc_bytes:
    // r0 = crc_pos, r1 = wr. Four bytes from one load where they are all in the ring
    // before its end; otherwise up to CRC_PER_STEP one by one.
    sub     r1, r1, r0
    ldr     r2, [r4, #CAP_OUT]
    ldr     r3, [r4, #CAP_CRC8]
    ldr     r12, [r4, #CAP_CRC]
    ubfx    lr, r0, #0, #OUT_BITS
    cmp     r1, #4
    blo     crc_one
    add     r1, lr, #3
    lsrs    r1, r1, #OUT_BITS
    bne     crc_one_reload
    ldr     r1, [r2, lr]
    uxtb    lr, r1
    eor     lr, lr, r12
    ldrb    r12, [r3, lr]
    ubfx    lr, r1, #8, #8
    eor     lr, lr, r12
    ldrb    r12, [r3, lr]
    ubfx    lr, r1, #16, #8
    eor     lr, lr, r12
    ldrb    r12, [r3, lr]
    eor     lr, r12, r1, lsr #24
    ldrb    r12, [r3, lr]
    add     r0, r0, #4
    b       crc_done

crc_one_reload:
    movs    r1, #4
crc_one:
    cmp     r1, #CRC_PER_STEP
    it      hi
    movhi   r1, #CRC_PER_STEP
11:
    ubfx    lr, r0, #0, #OUT_BITS
    ldrb    lr, [r2, lr]
    eor     lr, lr, r12
    ldrb    r12, [r3, lr]
    add     r0, r0, #1
    subs    r1, r1, #1
    bne     11b
crc_done:
    str     r0, [r4, #CAP_CRC_POS]
    str     r12, [r4, #CAP_CRC]
    b       pass_end

housekeeping:
    // How late samples get: checked here, every housekeeping tick (r0).
    ldr     r1, [r4, #CAP_MAX_LATE]
    cmp     r0, r1
    it      hi
    strhi   r0, [r4, #CAP_MAX_LATE]
    ldr     r0, [r4, #CAP_HK]
#ifdef CAPTURE_PROFILE
    add     r2, r0, #32
    str     r2, [sp, #4]
#endif
    add     r0, r4, r0, lsl #2
    ldr     r0, [r0, #CAP_HK_STEPS]
    blx     r0
    b       step_done

call_step:
    add     r0, r4, r6, lsl #2
    ldr     r0, [r0, #CAP_STEPS]
    blx     r0

step_done:
    cmp     r6, #ST_STOP
    bne     pass_end
    pop     {r0-r2, pc}

#ifdef CAPTURE_PROFILE
pass_end:
    // Longest, total and count of passes per step, from the end of the spin.
    ldr     r0, =DWT_CYCCNT
    ldr     r0, [r0]
    ldr     r1, [sp]
    sub     r0, r0, r1
    ldr     r1, [sp, #4]
    ldr     r2, =debug_counters
    add     r2, r2, r1, lsl #2
    ldr     r3, [r2, #DBG_STEP_MAX]
    cmp     r0, r3
    it      hi
    strhi   r0, [r2, #DBG_STEP_MAX]
    ldr     r3, [r2, #DBG_STEP_SUM]
    add     r3, r3, r0
    str     r3, [r2, #DBG_STEP_SUM]
    ldr     r3, [r2, #DBG_STEP_COUNT]
    add     r3, r3, #1
    str     r3, [r2, #DBG_STEP_COUNT]
    b       tick
#endif

    .ltorg
    .size capture_loop, . - capture_loop
