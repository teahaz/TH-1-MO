/*
 * Control channel before capture.
 *
 * SPIS1 on the MOD lines (SCK P1.13, MOSI P1.11, MISO P1.12, CSN P1.10), polled. Framing
 * is spec 9.2.1: Length (2 bytes), message, CRC-16/CCITT-FALSE. IRQ is P0.26, driven low
 * while a message waits and released otherwise; the Pi's pull-up holds it high.
 *
 * The END_ACQUIRE short hands the semaphore back to the CPU after each transaction.
 * Neither ACQUIRED nor END proves a transaction happened (a refused one fires the short
 * too), so a transaction counts only when the CPU owns the semaphore (SEMSTAT) and rx_buf
 * no longer starts with Length 0xFFFF, which no host sends.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/util_macro.h>
#include <zephyr/toolchain.h>

#include <zephyr/logging/log.h>
#include <zephyr/logging/log_core.h>

#include <mdk/nrf52840.h>
#include <mdk/nrf52840_bitfields.h>
#include <nrfx.h>

#include "capture.h"
#include "control.h"


LOG_MODULE_REGISTER(CONTROL, LOG_LEVEL_INF);


#define SPI_SCK_PIN (32 + 13) // P1.13, MOD0
#define SPI_MOSI_PIN (32 + 11) // P1.11, MOD1
#define SPI_MISO_PIN (32 + 12) // P1.12, MOD2
#define SPI_CSN_PIN (32 + 10) // P1.10, MOD3

#define MSG_MAX 512
#define FRAME_MAX (MSG_MAX + 4)
#define QUEUE_SIZE 4

#define VERSION_MAJOR 1
#define VERSION_MINOR 0
#define MODEL "TH-1 MO bus replay"
#define FIRMWARE "0.1.0"

#define MSG_HELLO 0x01
#define MSG_CONFIG 0x02
#define MSG_CONFIG_REPLY 0x03

#define REG_CAPTURE 0x00
#define REG_SAMPLE_RATE 0x01
#define REG_C 0x02
#define REG_ENABLED 0x03
#define REG_SPLIT 0x04
#define REG_COUNT 5

#define TYPE_BOOL 0x00
#define TYPE_U32 0x01
#define TYPE_CHANNEL_SET 0x02

#define KIND_ANY 0x00
#define KIND_RANGE 0x01
#define KIND_LIST 0x02
#define KIND_DIVIDER 0x03

#define FLAG_WRITABLE BIT(0)

#define STATUS_ACCEPTED 0x00
#define STATUS_UNKNOWN 0x01
#define STATUS_LENGTH 0x03
#define STATUS_NOT_ALLOWED 0x04
#define STATUS_STARTED 0x05
#define STATUS_RATE 0x06
#define STATUS_C_SHORT 0x07
#define STATUS_NO_CHANNEL 0x08
#define STATUS_SPLIT 0x09

#define CHANNEL_SET_SIZE DIV_ROUND_UP(CHANNEL_COUNT, 8)
#define CHANNEL_SET_MASK (BIT(CHANNEL_COUNT) - 1)

#define DEFAULT_SAMPLE_RATE 500000
#define DEFAULT_C 1000
#define DEFAULT_ENABLED 0x000F


struct message {
    uint16_t len;
    uint8_t data[MSG_MAX];
};

struct writer {
    uint8_t *buf;
    uint32_t len;
};


static void spis_configure(void);
static void irq_line_set(bool asserted);
static void rx_handle(uint32_t amount);
static void config_handle(const uint8_t *msg, uint32_t len);
static uint8_t config_write(uint8_t reg, const uint8_t *value, uint32_t len, const char **text);
static uint8_t start_check(const char **text);
static uint32_t reg_value_put(uint8_t reg, uint8_t *out);
static void tx_prepare(void);
static void hello_queue(void);
static void put_u8(struct writer *w, uint8_t v);
static void put_u16(struct writer *w, uint16_t v);
static void put_u32(struct writer *w, uint32_t v);
static void put_str(struct writer *w, const char *s);
static struct message *queue_tail_slot(void);
static void queue_push(void);
static uint16_t crc16(const uint8_t *data, uint32_t len);


static uint8_t tx_buf[FRAME_MAX] __attribute__((section("RAM8")));
static uint8_t rx_buf[FRAME_MAX] __attribute__((section("RAM8")));

static struct message queue[QUEUE_SIZE];
static uint32_t queue_head;
static uint32_t queue_tail;
static uint32_t tx_len; // Bytes of the frame in tx_buf, 0 if none.

static struct capture_config config = {
    .sample_rate = DEFAULT_SAMPLE_RATE,
    .c = DEFAULT_C,
    .enabled = DEFAULT_ENABLED,
    .split = 0,
};
static bool capture_on;

// Silkscreen names, in channel order.
static const char *const channel_names[CHANNEL_COUNT] = {
    "GP0.20", "GP0.21", "GP0.17", "GP0.19", "GP0.15", "GP0.16", "GP0.14",
    "GP0.13", "GP0.05", "GP0.07", "GP0.06", "GP0.08", "GP0.04",
};


void control_init(void)
{
    spis_configure();
    hello_queue();
    tx_prepare();
    irq_line_set(true);
    NRF_SPIS1->TASKS_RELEASE = 1;
}


// One polling pass. True once capture was accepted and its reply clocked out (spec 8.6).
bool control_service(void)
{
    if (!NRF_SPIS1->EVENTS_ACQUIRED) {
        return false;
    }
    NRF_SPIS1->EVENTS_ACQUIRED = 0;
    NRF_SPIS1->EVENTS_END = 0;

    if ((NRF_SPIS1->SEMSTAT & SPIS_SEMSTAT_SEMSTAT_Msk) != SPIS_SEMSTAT_SEMSTAT_CPU) {
        return false;
    }

    if (rx_buf[0] != 0xFF || rx_buf[1] != 0xFF) {
        NRF_SPIS1->STATUS = SPIS_STATUS_OVERREAD_Msk | SPIS_STATUS_OVERFLOW_Msk;

        // The device's message counts as sent only if all of it was clocked out.
        if (tx_len > 0 && NRF_SPIS1->TXD.AMOUNT >= tx_len) {
            queue_tail++;
        }
        rx_handle(NRF_SPIS1->RXD.AMOUNT);
    }

    if (capture_on && queue_head == queue_tail) {
        // Keep the semaphore: the capture loop takes the SPIS over from here.
        irq_line_set(false);
        return true;
    }

    tx_prepare();
    irq_line_set(queue_head != queue_tail);
    rx_buf[0] = 0xFF;
    rx_buf[1] = 0xFF;
    NRF_SPIS1->TASKS_RELEASE = 1;

    return false;
}


const struct capture_config *control_config(void)
{
    return &config;
}


static void spis_configure(void)
{
    // CSN pulled up so a released Pi pin (pull-down) does not select the slave.
    NRF_P1->PIN_CNF[SPI_CSN_PIN - 32] =
        (GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos) |
        (GPIO_PIN_CNF_PULL_Pullup << GPIO_PIN_CNF_PULL_Pos);
    NRF_P0->PIN_CNF[IRQ_PIN] = (GPIO_PIN_CNF_INPUT_Disconnect << GPIO_PIN_CNF_INPUT_Pos);
    NRF_P0->OUTCLR = BIT(IRQ_PIN);

    NRF_SPIS1->ENABLE = SPIS_ENABLE_ENABLE_Disabled;
    NRF_SPIS1->PSEL.SCK = SPI_SCK_PIN;
    NRF_SPIS1->PSEL.MOSI = SPI_MOSI_PIN;
    NRF_SPIS1->PSEL.MISO = SPI_MISO_PIN;
    NRF_SPIS1->PSEL.CSN = SPI_CSN_PIN;
    NRF_SPIS1->CONFIG = 0; // Mode 0, MSB first.
    NRF_SPIS1->DEF = 0xFF;
    NRF_SPIS1->ORC = 0x00;
    NRF_SPIS1->INTENCLR = 0xFFFFFFFF;
    NRF_SPIS1->SHORTS = SPIS_SHORTS_END_ACQUIRE_Msk;
    rx_buf[0] = 0xFF;
    rx_buf[1] = 0xFF;
    NRF_SPIS1->RXD.PTR = (uint32_t)rx_buf;
    NRF_SPIS1->RXD.MAXCNT = FRAME_MAX;
    NRF_SPIS1->EVENTS_END = 0;
    NRF_SPIS1->EVENTS_ACQUIRED = 0;
    NRF_SPIS1->ENABLE = SPIS_ENABLE_ENABLE_Enabled;
    NRF_SPIS1->TASKS_ACQUIRE = 1;

    while (!NRF_SPIS1->EVENTS_ACQUIRED) {
    }
    NRF_SPIS1->EVENTS_ACQUIRED = 0;
}


// Asserted: driven low. Released: pin disconnected, the Pi's pull-up holds it high.
static void irq_line_set(bool asserted)
{
    if (asserted) {
        NRF_P0->DIRSET = BIT(IRQ_PIN);
    } else {
        NRF_P0->DIRCLR = BIT(IRQ_PIN);
    }
}


static void rx_handle(uint32_t amount)
{
    uint32_t len = 0;

    if (amount < 2) {
        return;
    }

    len = sys_get_le16(rx_buf);
    if (len == 0) {
        return;
    }
    if (len > MSG_MAX || amount < len + 4) {
        LOG_WRN("Short host frame (len %u, amount %u)", len, amount);
        return;
    }
    if (crc16(rx_buf, len + 2) != sys_get_le16(&rx_buf[len + 2])) {
        LOG_WRN("Host frame CRC mismatch");
        return;
    }

    // A receiver ignores a message of a type it does not know (spec 8).
    if (rx_buf[2] == MSG_CONFIG) {
        config_handle(&rx_buf[2], len);
    }
}


static void config_handle(const uint8_t *msg, uint32_t len)
{
    struct message *slot = queue_tail_slot();
    struct writer w = {0};
    const char *text = "";
    uint8_t reg = 0xFF;
    uint8_t status = 0;

    if (slot == NULL) {
        LOG_WRN("Reply queue full, dropping configuration reply!");
        return;
    }

    if (len < 2) {
        status = STATUS_LENGTH;
        text = "message too short";
    } else {
        reg = msg[1];
        status = config_write(reg, &msg[2], len - 2, &text);
    }

    w.buf = slot->data;
    put_u8(&w, MSG_CONFIG_REPLY);
    put_u8(&w, reg);
    put_u8(&w, status);
    put_str(&w, text);
    w.len += reg_value_put(reg, &w.buf[w.len]);
    slot->len = w.len;
    queue_push();
    LOG_INF("Write register 0x%02x: status 0x%02x", reg, status);
}


static uint8_t config_write(uint8_t reg, const uint8_t *value, uint32_t len, const char **text)
{
    uint32_t v = 0;
    uint8_t status = 0;

    if (reg >= REG_COUNT) {
        return STATUS_UNKNOWN;
    }
    if (capture_on) {
        return STATUS_STARTED;
    }

    switch (reg) {
    case REG_CAPTURE:
        if (len != 1) {
            return STATUS_LENGTH;
        }
        if (value[0] != 1) {
            *text = "capture can only be set on";
            return STATUS_NOT_ALLOWED;
        }
        status = start_check(text);
        if (status != STATUS_ACCEPTED) {
            return status;
        }
        capture_on = true;
        return STATUS_ACCEPTED;

    case REG_SAMPLE_RATE:
        if (len != 4) {
            return STATUS_LENGTH;
        }
        v = sys_get_le32(value);
        if (v == 0 || CPU_HZ % v != 0 || CPU_HZ / v < MIN_DIVISOR) {
            *text = "rate must be 64 MHz / d, d >= 128";
            return STATUS_NOT_ALLOWED;
        }
        config.sample_rate = v;
        return STATUS_ACCEPTED;

    case REG_C:
        if (len != 4) {
            return STATUS_LENGTH;
        }
        v = sys_get_le32(value);
        if (v < C_MIN || v > C_MAX) {
            return STATUS_NOT_ALLOWED;
        }
        config.c = v;
        return STATUS_ACCEPTED;

    case REG_ENABLED:
    case REG_SPLIT:
        if (len != CHANNEL_SET_SIZE) {
            return STATUS_LENGTH;
        }
        v = sys_get_le16(value);
        if (v & ~CHANNEL_SET_MASK) {
            *text = "no such channel";
            return STATUS_NOT_ALLOWED;
        }
        if (reg == REG_ENABLED) {
            config.enabled = v;
        } else {
            config.split = v;
        }
        return STATUS_ACCEPTED;

    default:
        return STATUS_UNKNOWN;
    }
}


// Combined limits, checked on start (spec 8.5).
static uint8_t start_check(const char **text)
{
    uint64_t send_time = 0;

    if (config.enabled == 0) {
        return STATUS_NO_CHANNEL;
    }
    if (POPCOUNT(config.split) > DATA_CHANNEL_COUNT - 1) {
        return STATUS_SPLIT;
    }

    // The register already rejects these rates; this is the spec's start check.
    if (CPU_HZ / config.sample_rate < MIN_DIVISOR) {
        return STATUS_RATE;
    }

    // C * (1 / rate) must cover two serial heartbeats at the data baud rate. With less,
    // one UART transfer can take most of C and a pending heartbeat can miss its slot.
    send_time = (uint64_t)capture_heartbeat_size(&config) * UART_BITS_PER_BYTE *
                config.sample_rate;
    if ((uint64_t)config.c * DATA_BAUD < 2 * send_time) {
        *text = "C shorter than two heartbeats";
        return STATUS_C_SHORT;
    }

    return STATUS_ACCEPTED;
}


// Writes the register's current value; returns its size, 0 for an unknown register.
static uint32_t reg_value_put(uint8_t reg, uint8_t *out)
{
    switch (reg) {
    case REG_CAPTURE:
        out[0] = capture_on ? 1 : 0;
        return 1;
    case REG_SAMPLE_RATE:
        sys_put_le32(config.sample_rate, out);
        return 4;
    case REG_C:
        sys_put_le32(config.c, out);
        return 4;
    case REG_ENABLED:
        sys_put_le16(config.enabled, out);
        return CHANNEL_SET_SIZE;
    case REG_SPLIT:
        sys_put_le16(config.split, out);
        return CHANNEL_SET_SIZE;
    default:
        return 0;
    }
}


// Frames the head of the queue into tx_buf, or sends Length 0.
static void tx_prepare(void)
{
    const struct message *m = &queue[queue_tail % QUEUE_SIZE];
    uint16_t crc = 0;

    NRF_SPIS1->TXD.PTR = (uint32_t)tx_buf;
    if (queue_head == queue_tail) {
        sys_put_le16(0, tx_buf);
        tx_len = 0;
        NRF_SPIS1->TXD.MAXCNT = 2;
        return;
    }

    sys_put_le16(m->len, tx_buf);
    memcpy(&tx_buf[2], m->data, m->len);
    crc = crc16(tx_buf, m->len + 2);
    sys_put_le16(crc, &tx_buf[m->len + 2]);
    tx_len = m->len + 4;
    NRF_SPIS1->TXD.MAXCNT = tx_len;
}


static void hello_queue(void)
{
    struct message *slot = queue_tail_slot();
    struct writer w = {.buf = slot->data};

    put_u8(&w, MSG_HELLO);
    put_u8(&w, VERSION_MAJOR);
    put_u8(&w, VERSION_MINOR);
    put_str(&w, MODEL);
    put_str(&w, FIRMWARE);
    put_u8(&w, 8);
    put_u32(&w, NRF_FICR->DEVICEID[0]);
    put_u32(&w, NRF_FICR->DEVICEID[1]);
    put_u32(&w, OUT_SIZE);
    put_u16(&w, CHANNEL_COUNT);
    put_u8(&w, DATA_CHANNEL_COUNT);
    for (size_t i = 0; i < ARRAY_SIZE(channel_names); i++) {
        put_str(&w, channel_names[i]);
    }
    put_u8(&w, REG_COUNT);

    put_u8(&w, REG_CAPTURE);
    put_u8(&w, TYPE_BOOL);
    put_u8(&w, FLAG_WRITABLE);
    put_str(&w, "capture");
    put_str(&w, "");
    put_u8(&w, KIND_LIST);
    put_u16(&w, 1);
    put_u8(&w, 1);
    w.len += reg_value_put(REG_CAPTURE, &w.buf[w.len]);

    put_u8(&w, REG_SAMPLE_RATE);
    put_u8(&w, TYPE_U32);
    put_u8(&w, FLAG_WRITABLE);
    put_str(&w, "sample rate");
    put_str(&w, "Hz");
    put_u8(&w, KIND_DIVIDER);
    put_u32(&w, CPU_HZ);
    put_u32(&w, MIN_DIVISOR);
    put_u32(&w, MAX_DIVISOR);
    w.len += reg_value_put(REG_SAMPLE_RATE, &w.buf[w.len]);

    put_u8(&w, REG_C);
    put_u8(&w, TYPE_U32);
    put_u8(&w, FLAG_WRITABLE);
    put_str(&w, "C");
    put_str(&w, "ticks");
    put_u8(&w, KIND_RANGE);
    put_u32(&w, C_MIN);
    put_u32(&w, C_MAX);
    w.len += reg_value_put(REG_C, &w.buf[w.len]);

    put_u8(&w, REG_ENABLED);
    put_u8(&w, TYPE_CHANNEL_SET);
    put_u8(&w, FLAG_WRITABLE);
    put_str(&w, "enabled channels");
    put_str(&w, "");
    put_u8(&w, KIND_ANY);
    w.len += reg_value_put(REG_ENABLED, &w.buf[w.len]);

    put_u8(&w, REG_SPLIT);
    put_u8(&w, TYPE_CHANNEL_SET);
    put_u8(&w, FLAG_WRITABLE);
    put_str(&w, "split");
    put_str(&w, "");
    put_u8(&w, KIND_ANY);
    w.len += reg_value_put(REG_SPLIT, &w.buf[w.len]);

    slot->len = w.len;
    queue_push();
    LOG_INF("Hello queued (%u bytes)", w.len);
}


static void put_u8(struct writer *w, uint8_t v)
{
    w->buf[w->len++] = v;
}


static void put_u16(struct writer *w, uint16_t v)
{
    sys_put_le16(v, &w->buf[w->len]);
    w->len += 2;
}


static void put_u32(struct writer *w, uint32_t v)
{
    sys_put_le32(v, &w->buf[w->len]);
    w->len += 4;
}


static void put_str(struct writer *w, const char *s)
{
    size_t len = strlen(s);

    put_u8(w, (uint8_t)len);
    memcpy(&w->buf[w->len], s, len);
    w->len += len;
}


// The next free queue slot, or NULL when the queue is full. queue_push() then adds it.
static struct message *queue_tail_slot(void)
{
    if (queue_head - queue_tail >= QUEUE_SIZE) {
        return NULL;
    }

    return &queue[queue_head % QUEUE_SIZE];
}


static void queue_push(void)
{
    queue_head++;
}


// CRC-16/CCITT-FALSE: polynomial 0x1021, initial 0xFFFF.
static uint16_t crc16(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFF;

    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }

    return crc;
}
