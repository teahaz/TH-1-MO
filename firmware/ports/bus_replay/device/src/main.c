/*
 * Bus replay capture device: boot, configuration over the SPI control binding, then the
 * capture loop until reset.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/notify.h>
#include <zephyr/sys/onoff.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/util_macro.h>

#include <zephyr/drivers/clock_control/nrf_clock_control.h>

#include <zephyr/logging/log.h>
#include <zephyr/logging/log_core.h>
#include <zephyr/logging/log_ctrl.h>

#include <mdk/nrf52840.h>
#include <mdk/nrf52840_bitfields.h>
#include <nrfx.h>

#include "capture.h"
#include "control.h"


LOG_MODULE_REGISTER(BUS_REPLAY, LOG_LEVEL_INF);


#define DATA_TX_PIN 25 // P0.25
#define DATA_RX_PIN (32 + 2) // P1.02


static int hfxo_start(void);
static void channels_configure(void);
static void data_uart_configure(void);
static void dwt_start(void);


int main(void)
{
    const struct capture_config *cfg = NULL;
    uint32_t ticks = 0;
    int err = 0;

    err = hfxo_start();
    if (err < 0) {
        LOG_ERR("Failed to start HFXO! (err: %d)", err);
        return err;
    }

    channels_configure();
    data_uart_configure();
    dwt_start();
    control_init();
    LOG_INF("Ready, waiting for configuration");

    while (!control_service()) {
        k_yield();
    }

    cfg = control_config();
    capture_prepare(cfg);
    LOG_INF("Capture starting: rate %u Hz, C %u, channels 0x%04x, W %u", cfg->sample_rate,
            cfg->c, cfg->enabled, capture_record_width(cfg));

    // From here only fatal errors are logged, synchronously, with interrupts off until
    // the host's reset.
    LOG_PANIC();
    (void)irq_lock();
    err = capture_run(&ticks);
    LOG_ERR("Capture stopped at tick %u! (err: %d)", ticks, err);

    for (;;) {
    }
}


// Capture runs from the crystal oscillator; starts it and waits until it runs.
static int hfxo_start(void)
{
    struct onoff_manager *mgr = z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_HF);
    struct onoff_client cli = {0};
    int res = 0;
    int err = 0;

    sys_notify_init_spinwait(&cli.notify);

    err = onoff_request(mgr, &cli);
    if (err < 0) {
        return err;
    }

    do {
        err = sys_notify_fetch_result(&cli.notify, &res);
    } while (err == -EAGAIN);

    if ((NRF_CLOCK->HFCLKSTAT & CLOCK_HFCLKSTAT_SRC_Msk) != CLOCK_HFCLKSTAT_SRC_Xtal) {
        return -EIO;
    }

    return res;
}


/*
 * Sense pins: inputs, input buffer connected, no pull. Replay pins keep their reset state
 * (input buffer disconnected, not driven).
 */
static void channels_configure(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(channels); i++) {
        NRF_P0->PIN_CNF[channels[i].sense] =
            (GPIO_PIN_CNF_DIR_Input << GPIO_PIN_CNF_DIR_Pos) |
            (GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos) |
            (GPIO_PIN_CNF_PULL_Disabled << GPIO_PIN_CNF_PULL_Pos);
    }
}


// Data UART (UARTE1) at 1 Mbaud, 8N1, driven by registers. Zephyr's uart1 is disabled.
static void data_uart_configure(void)
{
    NRF_UARTE1->ENABLE = UARTE_ENABLE_ENABLE_Disabled;
    NRF_P0->OUTSET = BIT(DATA_TX_PIN);
    NRF_P0->DIRSET = BIT(DATA_TX_PIN);
    NRF_UARTE1->PSEL.TXD = DATA_TX_PIN;
    NRF_UARTE1->PSEL.RXD = DATA_RX_PIN;
    NRF_UARTE1->PSEL.RTS = UARTE_PSEL_RTS_CONNECT_Msk;
    NRF_UARTE1->PSEL.CTS = UARTE_PSEL_CTS_CONNECT_Msk;
    NRF_UARTE1->BAUDRATE = UARTE_BAUDRATE_BAUDRATE_Baud1M;
    NRF_UARTE1->CONFIG = 0;
    NRF_UARTE1->INTENCLR = 0xFFFFFFFF;
    NRF_UARTE1->ENABLE = UARTE_ENABLE_ENABLE_Enabled;
    NRF_UARTE1->EVENTS_ENDTX = 0;
}


// The DWT cycle counter paces the ticks.
static void dwt_start(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

