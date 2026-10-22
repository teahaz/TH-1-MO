/*
 * Capture setup and the capture loop's C steps (device/README.md, Structure to
 * Debugging). The loop itself and its common steps are in capture_loop.S.
 *
 * Each step is a few loads and stores, or one or two peripheral accesses, so the sampler
 * never misses a tick. The steps share r4 to r11 with the loop as global register
 * variables. They run from RAM through the Code RAM alias, so they call nothing directly
 * (calls are PC-relative); the step tables hold alias addresses.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/cdefs.h>

#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/util_macro.h>
#include <zephyr/toolchain.h>

#include <mdk/nrf52840.h>
#include <mdk/nrf52840_bitfields.h>
#include <nrfx.h>

#include "capture.h"
#include "capture_loop.h"


// Checked every housekeeping round (well under 256 ticks); lag grows one per tick.
#define RAW_LAP_LIMIT (RAW_SIZE - 256)
#define OUT_MASK (OUT_SIZE - 1)
#define UART_CAP 256
#define LAG_DEFAULT 1024

#define CAP_RX_SIZE 64
// Loss report: type, sequence, start and end positions, a lost count per channel.
#define REPORT_LEN(n) (1 + 4 + 8 + 8 + 4 * (n))
#define REPORT_FRAME_MAX (REPORT_LEN(CHANNEL_COUNT) + 4)
#define MSG_LOSS_REPORT 0x04
#define IRQ_PIN 26 // P0.26

#define SRAM_BASE 0x20000000UL
#define CODE_RAM_BASE 0x00800000UL
#define DEBUG_MAGIC 0xB05C0DE5


enum hk {
    HK_UART_POLL,
    HK_UART_DONE,
    HK_UART_SETUP,
    HK_UART_COUNT,
    HK_UART_GO,
    HK_SPIS_POLL,
    HK_SPIS_ACQUIRED,
    HK_SPIS_SEMAPHORE,
    HK_SPIS_AMOUNT,
    HK_SPIS_DECIDE,
    HK_SPIS_COPY,
    HK_SPIS_CRC,
    HK_SPIS_LOAD,
    HK_SPIS_RELEASE,
    HK_DEBUG,
    HK_COUNT,
};

struct capture;
typedef void (*loop_fn)(struct capture *s);
typedef void (*step_fn)(void);

// Read by the Pi over SWD while the loop runs; lag_max may be written to force drops.
struct debug_counters {
    uint32_t magic;
    uint32_t ticks;
    uint32_t max_late;
    uint32_t max_lag;
    uint32_t lag_max;
    uint32_t heartbeats;
    uint32_t drop_room;
    uint32_t drop_lag;
    uint32_t reports;
    uint32_t bytes;
    uint32_t error;
#ifdef CAPTURE_PROFILE
    // Per pass, in cycles from the end of the spin to the end of the step: longest,
    // total and count. Cleanup steps at 0 to 31, housekeeping steps at 32 to 63.
    uint32_t step_max[64];
    uint32_t step_sum[64];
    uint32_t step_count[64];
#endif
};

// Loop state outside the registers, addressed from c.
struct capture {
    // Used by the assembly loop, at the offsets in capture_loop.h.
    uint32_t tick_cycles;
    uint32_t mask;
    uint32_t hk;
    uint32_t changed;
    uint32_t cur;
    uint32_t *raw;
    uint32_t wr;
    uint32_t rd;
    uint32_t crc;
    uint32_t crc_pos;
    uint32_t room_limit;
    uint32_t width;
    uint32_t tag_bits;
    uint32_t heartbeat_seq;
    uint32_t heartbeat_tick;
    uint32_t gap;
    uint32_t heartbeat_pending;
    uint32_t loss_open;
    uint8_t *out;
    const uint8_t *crc8;
    uint32_t lag_max;
    uint32_t after_flush;
    uint32_t after_crc;
    uint32_t slice_adds;
    uint32_t loss_start;
    uint32_t loss_end;
    uint32_t drop_lag;
    uint32_t drop_room;
    uint32_t heartbeat_len;
    uint32_t max_late;
    uint32_t slices[SLICES];
    uint8_t tag[32];
    uint32_t ref_seq[32];
    uint32_t ref_tick[32];
    step_fn steps[ST_COUNT];
    step_fn hk_steps[HK_COUNT];

    int error;

    // Configuration, fixed for the capture.
    uint32_t c;
    uint32_t enabled_count;
    uint32_t report_len;
    uint32_t uart_cap;
    uint8_t enabled_bits[CHANNEL_COUNT];

    // Entry being written by the C writer, low byte first: a first part, a second part
    // (a heartbeat's data after its marker), then its check byte if any.
    uint32_t value;
    uint32_t write_left;
    uint32_t second;
    uint32_t second_len;
    bool write_check;
    uint32_t after_write;

    uint32_t grid;
    uint32_t heartbeat_data;

    // The UART transfer in flight, and the next one's length.
    uint32_t inflight;
    uint32_t next_len;

    // Lost records per channel, flushed from the bit-sliced counters.
    uint32_t lost[CHANNEL_COUNT];
    uint32_t flush_i;
    bool flush_snap;

    // A finished loss handed to housekeeping: its range and the counters at its end.
    bool next_valid;
    bool snap_busy;
    bool copy_busy;
    uint32_t next_start_grid;
    uint32_t next_start_off;
    uint32_t next_end_grid;
    uint32_t next_end_off;
    uint32_t next_lost[CHANNEL_COUNT];

    // Loss report in the SPIS transmit buffer.
    bool acquire_requested;
    uint32_t report_seq;
    uint32_t loaded_len;
    uint32_t report_start_grid;
    uint32_t report_start_off;
    uint32_t copy_i;
    uint32_t crc_i;
    uint32_t crc16;

    // Byte lookup tables for the heartbeat bitmask, one per byte of P0.
    uint16_t bitmask_lut[4][256];
    uint8_t crc8_table[256];
    uint16_t crc16_table[256];
};


#define CAP_FIELD(field, offset) BUILD_ASSERT(offsetof(struct capture, field) == (offset))

CAP_FIELD(tick_cycles, CAP_TICK);
CAP_FIELD(mask, CAP_MASK);
CAP_FIELD(hk, CAP_HK);
CAP_FIELD(changed, CAP_CHANGED);
CAP_FIELD(cur, CAP_CUR);
CAP_FIELD(raw, CAP_RAW);
CAP_FIELD(wr, CAP_WR);
CAP_FIELD(rd, CAP_RD);
CAP_FIELD(crc, CAP_CRC);
CAP_FIELD(crc_pos, CAP_CRC_POS);
CAP_FIELD(room_limit, CAP_ROOM_LIMIT);
CAP_FIELD(width, CAP_WIDTH);
CAP_FIELD(tag_bits, CAP_TAG_BITS);
CAP_FIELD(heartbeat_seq, CAP_HB_SEQ);
CAP_FIELD(heartbeat_tick, CAP_HB_TICK);
CAP_FIELD(gap, CAP_GAP);
CAP_FIELD(heartbeat_pending, CAP_HB_PENDING);
CAP_FIELD(loss_open, CAP_LOSS_OPEN);
CAP_FIELD(out, CAP_OUT);
CAP_FIELD(crc8, CAP_CRC8);
CAP_FIELD(lag_max, CAP_LAG_MAX);
CAP_FIELD(after_flush, CAP_AFTER_FLUSH);
CAP_FIELD(after_crc, CAP_AFTER_CRC);
CAP_FIELD(slice_adds, CAP_SLICE_ADDS);
CAP_FIELD(loss_start, CAP_LOSS_START);
CAP_FIELD(loss_end, CAP_LOSS_END);
CAP_FIELD(drop_lag, CAP_DROP_LAG);
CAP_FIELD(drop_room, CAP_DROP_ROOM);
CAP_FIELD(heartbeat_len, CAP_HB_LEN);
CAP_FIELD(max_late, CAP_MAX_LATE);
CAP_FIELD(slices, CAP_SLICES);
CAP_FIELD(tag, CAP_TAG);
CAP_FIELD(ref_seq, CAP_REF_SEQ);
CAP_FIELD(ref_tick, CAP_REF_TICK);
CAP_FIELD(steps, CAP_STEPS);
CAP_FIELD(hk_steps, CAP_HK_STEPS);
BUILD_ASSERT(OUT_SIZE == BIT(OUT_BITS));
#ifdef CAPTURE_PROFILE
BUILD_ASSERT(offsetof(struct debug_counters, step_max) == DBG_STEP_MAX);
BUILD_ASSERT(offsetof(struct debug_counters, step_sum) == DBG_STEP_SUM);
BUILD_ASSERT(offsetof(struct debug_counters, step_count) == DBG_STEP_COUNT);
BUILD_ASSERT(ST_COUNT <= 32);
#endif


// Owned by the assembly loop; capture_loop.S describes them.
register struct capture *c __asm__("r4");
register uint32_t deadline __asm__("r5");
register uint32_t step __asm__("r6");
register uint32_t tick_cycles __asm__("r7");
register uint32_t idx __asm__("r8");
register uint32_t pos __asm__("r9");
register uint32_t due __asm__("r10");
register uint32_t prev __asm__("r11");


static uint32_t enabled_count(const struct capture_config *cfg);
static uint32_t tag_bits_for(uint32_t n);
static uint32_t uart_cap_for(const struct capture_config *cfg);
static void tables_build(struct capture *s, uint16_t enabled);
static void steps_build(struct capture *s);
static void spis_take_over(void);
static void *alias_of(void *fn);
static void capture_enter(struct capture *s, loop_fn loop);

void capture_loop(struct capture *s);

static void st_first(void);
static void st_heartbeat_now(void);
static void st_record_slow(void);
static void st_write(void);
static void st_check(void);
static void st_flush_start(void);
static void st_heartbeat(void);
static void st_heartbeat_pack(void);
static void st_heartbeat_try(void);
static void st_loss_end(void);
static void st_flush(void);
static void st_flush_end(void);
static void st_snap(void);
static void st_stop(void);

static void hk_uart_poll(void);
static void hk_uart_done(void);
static void hk_uart_setup(void);
static void hk_uart_count(void);
static void hk_uart_go(void);
static void hk_spis_poll(void);
static void hk_spis_acquired(void);
static void hk_spis_semaphore(void);
static void hk_spis_amount(void);
static void hk_spis_decide(void);
static void hk_spis_copy(void);
static void hk_spis_crc(void);
static void hk_spis_load(void);
static void hk_spis_release(void);
static void hk_debug(void);


const uint8_t channel_pins[CHANNEL_COUNT] = {20, 21, 17, 19, 15, 16, 14, 13, 5, 7, 6, 8, 4};

static uint32_t raw_ring[RAW_SIZE] __attribute__((section("LOOPRAM")));
static struct capture state __attribute__((section("LOOPRAM")));

static uint8_t out_ring[OUT_SIZE] __aligned(4) __attribute__((section("RAM8")));
static uint8_t report_buf[REPORT_FRAME_MAX] __aligned(4) __attribute__((section("RAM8")));
static uint8_t empty_buf[4] __aligned(4) __attribute__((section("RAM8")));
static uint8_t cap_rx_buf[CAP_RX_SIZE] __aligned(4) __attribute__((section("RAM8")));

struct debug_counters debug_counters __attribute__((section("RAM7")));


// W: the smallest number of bytes that holds (2C + 1) * 2^t - 1 (spec 5.2).
uint32_t capture_record_width(const struct capture_config *cfg)
{
    uint64_t max = ((uint64_t)2 * cfg->c + 1) * BIT64(tag_bits_for(enabled_count(cfg))) - 1;
    uint32_t width = 1;

    while (width < 8 && max >= BIT64(8 * width)) {
        width++;
    }

    return width;
}


// A serial heartbeat: W + 1 + ceil(n / 8) bytes plus its check byte (spec 9.1.1).
uint32_t capture_heartbeat_size(const struct capture_config *cfg)
{
    return capture_record_width(cfg) + 1 + DIV_ROUND_UP(enabled_count(cfg), 8) + 1;
}


void capture_prepare(const struct capture_config *cfg)
{
    struct capture *s = &state;
    uint32_t n = enabled_count(cfg);

    memset(s, 0, sizeof(*s));
    s->tick_cycles = CPU_HZ / cfg->sample_rate;
    s->c = cfg->c;
    s->width = capture_record_width(cfg);
    s->tag_bits = tag_bits_for(n);
    s->enabled_count = n;
    // The heartbeat entry, without its check byte.
    s->heartbeat_len = capture_heartbeat_size(cfg) - 1;
    s->report_len = REPORT_LEN(n);
    s->uart_cap = uart_cap_for(cfg);
    // The assembly writes a record as one 32-bit store (capture_loop.S, ST_ENCODE).
    s->room_limit = OUT_SIZE - 4;
    s->crc = 0xFF;
    // The number of the last grid point reached; tick 0 makes it 0.
    s->grid = UINT32_MAX;
    s->raw = raw_ring;
    s->out = out_ring;
    s->crc8 = s->crc8_table;
    s->lag_max = LAG_DEFAULT;
    tables_build(s, cfg->enabled);
    steps_build(s);

    debug_counters = (struct debug_counters){0};
    debug_counters.magic = DEBUG_MAGIC;
    debug_counters.lag_max = LAG_DEFAULT;

    NRF_UARTE1->EVENTS_ENDTX = 0;
    spis_take_over();
}


/*
 * Runs the capture until a fatal error. Interrupts must be off. Returns the error and
 * the tick it stopped at.
 */
int capture_run(uint32_t *ticks)
{
    capture_enter(&state, alias_of(capture_loop));
    *ticks = debug_counters.ticks;

    return state.error;
}


static uint32_t enabled_count(const struct capture_config *cfg)
{
    return POPCOUNT(cfg->enabled);
}


// Bits of the channel tag below a record's value (spec 5.2).
static uint32_t tag_bits_for(uint32_t n)
{
    return n > 1 ? LOG2CEIL(n) : 0;
}


/*
 * Room in the output ring comes back only when a UART transfer ends. A pending heartbeat
 * must be written before the next grid point, so a transfer lasts at most half of C, and
 * is at least one heartbeat long.
 */
static uint32_t uart_cap_for(const struct capture_config *cfg)
{
    uint64_t per_c = (uint64_t)cfg->c * DATA_BAUD / ((uint64_t)UART_BITS_PER_BYTE *
                                                     cfg->sample_rate);

    return CLAMP(per_c / 2, capture_heartbeat_size(cfg), UART_CAP);
}


static void tables_build(struct capture *s, uint16_t enabled)
{
    uint32_t i = 0;
    uint32_t crc = 0;

    for (uint32_t ch = 0; ch < CHANNEL_COUNT; ch++) {
        if (enabled & BIT(ch)) {
            s->mask |= BIT(channel_pins[ch]);
        }
    }

    // Enabled channels in channel order give the tag and the bitmask bit.
    for (uint32_t ch = 0; ch < CHANNEL_COUNT; ch++) {
        if (!(enabled & BIT(ch))) {
            continue;
        }
        s->tag[channel_pins[ch]] = (uint8_t)i;
        s->enabled_bits[i] = channel_pins[ch];
        i++;
    }

    for (uint32_t b = 0; b < 4; b++) {
        for (uint32_t v = 0; v < 256; v++) {
            uint16_t bits = 0;

            for (uint32_t j = 0; j < 8; j++) {
                uint32_t pin = 8 * b + j;

                if ((v & BIT(j)) && (s->mask & BIT(pin))) {
                    bits |= BIT(s->tag[pin]);
                }
            }
            s->bitmask_lut[b][v] = bits;
        }
    }

    for (uint32_t v = 0; v < 256; v++) {
        crc = v;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? ((crc << 1) ^ 0x07) : (crc << 1);
        }
        s->crc8_table[v] = (uint8_t)crc;

        crc = v << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) : (crc << 1);
        }
        s->crc16_table[v] = (uint16_t)crc;
    }
}


static void steps_build(struct capture *s)
{
    // ST_SCAN to ST_SCAN_DROP run inline in the assembly loop.
    s->steps[ST_FIRST] = alias_of(st_first);
    s->steps[ST_HEARTBEAT_NOW] = alias_of(st_heartbeat_now);
    s->steps[ST_RECORD_SLOW] = alias_of(st_record_slow);
    s->steps[ST_WRITE] = alias_of(st_write);
    s->steps[ST_CHECK] = alias_of(st_check);
    s->steps[ST_FLUSH_START] = alias_of(st_flush_start);
    s->steps[ST_HEARTBEAT] = alias_of(st_heartbeat);
    s->steps[ST_HEARTBEAT_PACK] = alias_of(st_heartbeat_pack);
    s->steps[ST_HEARTBEAT_TRY] = alias_of(st_heartbeat_try);
    s->steps[ST_LOSS_END] = alias_of(st_loss_end);
    s->steps[ST_FLUSH] = alias_of(st_flush);
    s->steps[ST_FLUSH_END] = alias_of(st_flush_end);
    s->steps[ST_SNAP] = alias_of(st_snap);
    s->steps[ST_STOP] = alias_of(st_stop);

    s->hk_steps[HK_UART_POLL] = alias_of(hk_uart_poll);
    s->hk_steps[HK_UART_DONE] = alias_of(hk_uart_done);
    s->hk_steps[HK_UART_SETUP] = alias_of(hk_uart_setup);
    s->hk_steps[HK_UART_COUNT] = alias_of(hk_uart_count);
    s->hk_steps[HK_UART_GO] = alias_of(hk_uart_go);
    s->hk_steps[HK_SPIS_POLL] = alias_of(hk_spis_poll);
    s->hk_steps[HK_SPIS_ACQUIRED] = alias_of(hk_spis_acquired);
    s->hk_steps[HK_SPIS_SEMAPHORE] = alias_of(hk_spis_semaphore);
    s->hk_steps[HK_SPIS_AMOUNT] = alias_of(hk_spis_amount);
    s->hk_steps[HK_SPIS_DECIDE] = alias_of(hk_spis_decide);
    s->hk_steps[HK_SPIS_COPY] = alias_of(hk_spis_copy);
    s->hk_steps[HK_SPIS_CRC] = alias_of(hk_spis_crc);
    s->hk_steps[HK_SPIS_LOAD] = alias_of(hk_spis_load);
    s->hk_steps[HK_SPIS_RELEASE] = alias_of(hk_spis_release);
    s->hk_steps[HK_DEBUG] = alias_of(hk_debug);
}


// The control side left the SPIS acquired by the CPU; point it at the capture buffers.
static void spis_take_over(void)
{
    sys_put_le16(0, empty_buf);
    cap_rx_buf[0] = 0xFF;
    cap_rx_buf[1] = 0xFF;
    NRF_SPIS1->EVENTS_ACQUIRED = 0;
    NRF_SPIS1->EVENTS_END = 0;
    NRF_SPIS1->RXD.PTR = (uint32_t)cap_rx_buf;
    NRF_SPIS1->RXD.MAXCNT = CAP_RX_SIZE;
    NRF_SPIS1->TXD.PTR = (uint32_t)empty_buf;
    NRF_SPIS1->TXD.MAXCNT = 2;
    NRF_SPIS1->TASKS_RELEASE = 1;
}


static void *alias_of(void *fn)
{
    return (void *)((uintptr_t)fn - SRAM_BASE + CODE_RAM_BASE);
}


// Saves r4 to r11 for the caller around the loop, which owns them.
static __attribute__((naked)) void capture_enter(struct capture *s, loop_fn loop)
{
    __asm__ volatile("push {r3-r11, lr}\n"
                     "blx r1\n"
                     "pop {r3-r11, pc}\n");
}


#define ROOM() (OUT_SIZE - (c->wr - c->rd))

/*
 * Starts writing an entry: len bytes of value, then second_len bytes of second. Writing
 * an entry ends an open loss (spec 7.2): the drop counters are then flushed and handed
 * to housekeeping before going on.
 */
#define WRITE_START(v, len, v2, len2, check, next)                                     \
    do {                                                                               \
        c->value = (v);                                                                \
        c->write_left = (len);                                                         \
        c->second = (v2);                                                              \
        c->second_len = (len2);                                                        \
        c->write_check = (check);                                                      \
        if (c->loss_open) {                                                            \
            c->loss_open = 0;                                                          \
            c->flush_i = 0;                                                            \
            c->flush_snap = true;                                                      \
            c->after_flush = (next);                                                   \
            c->after_write = ST_FLUSH;                                                 \
        } else {                                                                       \
            c->after_write = (next);                                                   \
        }                                                                              \
        step = ST_WRITE;                                                               \
    } while (0)

// Marker (W zero bytes), then grid index and bitmask, then the check byte.
#define HEARTBEAT_START(next)                                                          \
    do {                                                                               \
        c->heartbeat_pending = 0;                                                      \
        debug_counters.heartbeats++;                                                   \
        WRITE_START(0, c->width, c->heartbeat_data, c->heartbeat_len - c->width, true, \
                    (next));                                                           \
    } while (0)

#define FATAL(code)                                                                    \
    do {                                                                               \
        debug_counters.error = (code);                                                 \
        debug_counters.ticks = idx;                                                    \
        c->error = (code);                                                             \
        step = ST_STOP;                                                                \
    } while (0)

// optimize is GCC only; clang (clangd) does not know it.
#ifdef __clang__
#define STEP static __ramfunc void
#else
#define STEP static __ramfunc __attribute__((optimize("O2"))) void
#endif


// Tick 0 has no records; the capture begins with its heartbeat.
STEP st_first(void)
{
    prev = raw_ring[0] & c->mask;
    c->cur = prev;
    step = ST_HEARTBEAT;
}


// A pending heartbeat that fits now goes before anything else (spec 6.4).
STEP st_heartbeat_now(void)
{
    HEARTBEAT_START(ST_SCAN);
}


// A pending heartbeat that fits now goes before the record (spec 6.4).
STEP st_record_slow(void)
{
    HEARTBEAT_START(ST_RECORD);
}


// Up to two bytes of the entry per step.
STEP st_write(void)
{
    uint32_t wr = c->wr;
    uint32_t v = c->value;
    uint32_t left = c->write_left;

    out_ring[wr & OUT_MASK] = (uint8_t)v;
    wr++;
    if (left > 1) {
        out_ring[wr & OUT_MASK] = (uint8_t)(v >> 8);
        wr++;
        left -= 2;
    } else {
        left = 0;
    }
    c->wr = wr;
    c->value = v >> 16;

    if (left == 0 && c->second_len) {
        c->value = c->second;
        left = c->second_len;
        c->second_len = 0;
    }
    c->write_left = left;
    if (left == 0) {
        step = c->write_check ? ST_CHECK : c->after_write;
    }
}


// The check byte, once the CRC-8 covers every byte before it (ST_CRC).
STEP st_check(void)
{
    if (c->crc_pos != c->wr) {
        c->after_crc = ST_CHECK;
        step = ST_CRC;
        return;
    }

    out_ring[c->wr & OUT_MASK] = (uint8_t)c->crc;
    c->wr++;
    c->crc_pos = c->wr;
    c->crc = 0xFF;
    step = c->after_write;
}


// The drop counters near overflow: flush them, then c->after_flush.
STEP st_flush_start(void)
{
    c->flush_i = 0;
    c->flush_snap = false;
    step = ST_FLUSH;
}


STEP st_heartbeat(void)
{
    if (c->heartbeat_pending) {
        FATAL(CAPTURE_ERR_HEARTBEAT_LATE);
        return;
    }

    c->grid++;
    c->heartbeat_pending = 1;
    c->heartbeat_seq++;
    c->heartbeat_tick = pos;
    pos++;
    due = c->c - 1;
    step = ST_HEARTBEAT_PACK;
}


// The grid index and the levels at the grid point (spec 6.1).
STEP st_heartbeat_pack(void)
{
    uint32_t s = c->cur;
    uint32_t v = 0;

    v = c->bitmask_lut[0][s & 0xFF] | c->bitmask_lut[1][(s >> 8) & 0xFF] |
        c->bitmask_lut[2][(s >> 16) & 0xFF] | c->bitmask_lut[3][s >> 24];
    c->heartbeat_data = (uint8_t)c->grid | (v << 8);
    step = ST_HEARTBEAT_TRY;
}


STEP st_heartbeat_try(void)
{
    if (ROOM() > c->heartbeat_len) {
        HEARTBEAT_START(ST_SCAN);
    } else {
        step = ST_SCAN_DROP;
    }
}


// A record written by the assembly path ended an open loss (spec 7.2).
STEP st_loss_end(void)
{
    c->loss_open = 0;
    c->flush_i = 0;
    c->flush_snap = true;
    step = ST_FLUSH;
}


// Half a channel per step: four slices into its lost-records counter.
STEP st_flush(void)
{
    uint32_t b = c->enabled_bits[c->flush_i >> 1];
    uint32_t n = (c->flush_i & 1) * 4;
    uint32_t v = 0;

    v = ((c->slices[n] >> b) & 1) | (((c->slices[n + 1] >> b) & 1) << 1) |
        (((c->slices[n + 2] >> b) & 1) << 2) | (((c->slices[n + 3] >> b) & 1) << 3);
    c->lost[c->flush_i >> 1] += v << n;
    if (++c->flush_i == 2 * c->enabled_count) {
        step = ST_FLUSH_END;
    }
}


STEP st_flush_end(void)
{
    // Written out: a loop would become a call to memset.
    c->slices[0] = 0;
    c->slices[1] = 0;
    c->slices[2] = 0;
    c->slices[3] = 0;
    c->slices[4] = 0;
    c->slices[5] = 0;
    c->slices[6] = 0;
    c->slices[7] = 0;
    c->slice_adds = 0;
    c->flush_i = 0;
    step = c->flush_snap ? ST_SNAP : c->after_flush;
}


// The loss is over: hand its range and the counters to housekeeping.
STEP st_snap(void)
{
    if (c->copy_busy) {
        return;
    }

    c->snap_busy = true;
    for (uint32_t n = 0; n < 3 && c->flush_i < c->enabled_count; n++, c->flush_i++) {
        c->next_lost[c->flush_i] = c->lost[c->flush_i];
    }
    if (c->flush_i < c->enabled_count) {
        return;
    }

    // Positions: grid points are every C ticks from tick 0 (spec 6.5).
    if (!c->next_valid) {
        c->next_start_grid = c->loss_start / c->c;
        c->next_start_off = c->loss_start % c->c;
    }
    c->next_end_grid = c->loss_end / c->c;
    c->next_end_off = c->loss_end % c->c;
    c->next_valid = true;
    c->snap_busy = false;
    step = c->after_flush;
}


STEP st_stop(void)
{
}


STEP hk_uart_poll(void)
{
    if (c->inflight) {
        c->hk = NRF_UARTE1->EVENTS_ENDTX ? HK_UART_DONE : HK_SPIS_POLL;
    } else {
        c->hk = c->wr != c->rd ? HK_UART_SETUP : HK_SPIS_POLL;
    }
}


STEP hk_uart_done(void)
{
    NRF_UARTE1->EVENTS_ENDTX = 0;
    c->rd += c->inflight;
    c->inflight = 0;
    c->hk = c->wr != c->rd ? HK_UART_SETUP : HK_SPIS_POLL;
}


// Capped, and split where the ring wraps.
STEP hk_uart_setup(void)
{
    uint32_t n = c->wr - c->rd;
    uint32_t v = OUT_SIZE - (c->rd & OUT_MASK);

    c->next_len = MIN(n, MIN(v, c->uart_cap));
    NRF_UARTE1->TXD.PTR = (uint32_t)&out_ring[c->rd & OUT_MASK];
    c->hk = HK_UART_COUNT;
}


STEP hk_uart_count(void)
{
    NRF_UARTE1->TXD.MAXCNT = c->next_len;
    c->inflight = c->next_len;
    c->hk = HK_UART_GO;
}


STEP hk_uart_go(void)
{
    NRF_UARTE1->TASKS_STARTTX = 1;
    c->hk = HK_SPIS_POLL;
}


STEP hk_spis_poll(void)
{
    if (c->next_valid && !c->acquire_requested) {
        NRF_SPIS1->TASKS_ACQUIRE = 1;
        c->acquire_requested = true;
        c->hk = HK_DEBUG;
    } else {
        c->hk = NRF_SPIS1->EVENTS_ACQUIRED ? HK_SPIS_ACQUIRED : HK_DEBUG;
    }
}


STEP hk_spis_acquired(void)
{
    NRF_SPIS1->EVENTS_ACQUIRED = 0;
    c->hk = HK_SPIS_SEMAPHORE;
}


STEP hk_spis_semaphore(void)
{
    if ((NRF_SPIS1->SEMSTAT & SPIS_SEMSTAT_SEMSTAT_Msk) != SPIS_SEMSTAT_SEMSTAT_CPU) {
        c->hk = HK_DEBUG;
        return;
    }

    c->acquire_requested = false;
    if (c->loaded_len && (cap_rx_buf[0] != 0xFF || cap_rx_buf[1] != 0xFF)) {
        c->hk = HK_SPIS_AMOUNT;
    } else {
        c->hk = HK_SPIS_DECIDE;
    }
}


// The report counts as sent only if all of it was clocked out.
STEP hk_spis_amount(void)
{
    if (NRF_SPIS1->TXD.AMOUNT >= c->loaded_len) {
        c->report_seq++;
        c->loaded_len = 0;
        debug_counters.reports++;
    }
    c->hk = HK_SPIS_DECIDE;
}


// A report not yet sent takes the new loss end and counters (merging).
STEP hk_spis_decide(void)
{
    if (!c->next_valid || c->snap_busy) {
        c->hk = HK_SPIS_LOAD;
        return;
    }

    c->copy_busy = true;
    c->next_valid = false;
    if (!c->loaded_len) {
        c->report_start_grid = c->next_start_grid;
        c->report_start_off = c->next_start_off;
    }
    report_buf[0] = (uint8_t)c->report_len;
    report_buf[1] = (uint8_t)(c->report_len >> 8);
    report_buf[2] = MSG_LOSS_REPORT;
    UNALIGNED_PUT(c->report_seq, (uint32_t *)&report_buf[3]);
    UNALIGNED_PUT(c->report_start_grid, (uint32_t *)&report_buf[7]);
    UNALIGNED_PUT(c->report_start_off, (uint32_t *)&report_buf[11]);
    c->copy_i = 0;
    c->hk = HK_SPIS_COPY;
}


STEP hk_spis_copy(void)
{
    if (c->copy_i == 0) {
        UNALIGNED_PUT(c->next_end_grid, (uint32_t *)&report_buf[15]);
        UNALIGNED_PUT(c->next_end_off, (uint32_t *)&report_buf[19]);
    }
    for (uint32_t n = 0; n < 3 && c->copy_i < c->enabled_count; n++, c->copy_i++) {
        UNALIGNED_PUT(c->next_lost[c->copy_i], (uint32_t *)&report_buf[23 + 4 * c->copy_i]);
    }
    if (c->copy_i == c->enabled_count) {
        c->copy_busy = false;
        c->crc_i = 0;
        c->crc16 = 0xFFFF;
        c->hk = HK_SPIS_CRC;
    }
}


STEP hk_spis_crc(void)
{
    uint32_t v = c->crc16;
    uint32_t i = c->crc_i;
    uint32_t end = c->report_len + 2;

    for (uint32_t n = 0; n < 3 && i < end; n++, i++) {
        v = ((v << 8) ^ c->crc16_table[((v >> 8) ^ report_buf[i]) & 0xFF]) & 0xFFFF;
    }
    c->crc16 = v;
    c->crc_i = i;
    if (i == end) {
        report_buf[i] = (uint8_t)v;
        report_buf[i + 1] = (uint8_t)(v >> 8);
        c->loaded_len = c->report_len + 4;
        c->hk = HK_SPIS_LOAD;
    }
}


STEP hk_spis_load(void)
{
    if (c->loaded_len) {
        NRF_SPIS1->TXD.PTR = (uint32_t)report_buf;
        NRF_SPIS1->TXD.MAXCNT = c->loaded_len;
    } else {
        NRF_SPIS1->TXD.PTR = (uint32_t)empty_buf;
        NRF_SPIS1->TXD.MAXCNT = 2;
    }
    c->hk = HK_SPIS_RELEASE;
}


// Host messages are ignored during capture (README, Deviations).
STEP hk_spis_release(void)
{
    if (c->loaded_len) {
        NRF_P0->DIRSET = BIT(IRQ_PIN);
    } else {
        NRF_P0->DIRCLR = BIT(IRQ_PIN);
    }
    cap_rx_buf[0] = 0xFF;
    cap_rx_buf[1] = 0xFF;
    NRF_SPIS1->TASKS_RELEASE = 1;
    c->hk = HK_DEBUG;
}


STEP hk_debug(void)
{
    uint32_t v = 0;

    // The sampler must not lap cleanup in the raw ring (README, Raw ring).
    if (idx - pos > RAW_LAP_LIMIT) {
        FATAL(CAPTURE_ERR_LAPPED);
        return;
    }

    // How late a sample has been taken, and how far cleanup is behind.
    debug_counters.max_late = c->max_late;
    c->lag_max = debug_counters.lag_max;
    v = idx - pos;
    if (v > debug_counters.max_lag) {
        debug_counters.max_lag = v;
    }
    debug_counters.ticks = idx;
    debug_counters.bytes = c->wr;
    debug_counters.drop_lag = c->drop_lag;
    debug_counters.drop_room = c->drop_room;
    c->hk = HK_UART_POLL;
}
