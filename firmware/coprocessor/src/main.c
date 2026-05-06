#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/counter.h>

#include <zephyr/sys/util.h>
#include <zephyr/sys/util_macro.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/dt-bindings/pwm/pwm.h>

#include <zephyr/toolchain.h>

#include <zephyr/logging/log.h>
#include <zephyr/logging/log_core.h>


LOG_MODULE_REGISTER(ir_record, LOG_LEVEL_INF);


/* #define IR_NODE          DT_NODELABEL(ir_rx) */
/* #define COUNTER_NODE     DT_NODELABEL(timer2) */
#define MAX_EDGES        500
#define FRAME_GAP_MS     150
#define EDGE_QUEUE_DEPTH 64 // ISR thread handoff buffer


static const struct gpio_dt_spec ir_rx_pin     = GPIO_DT_SPEC_GET(DT_NODELABEL(ir_rx_pin), gpios);
static const struct device *const counter_dev  = DEVICE_DT_GET(DT_NODELABEL(timer2));


/* ----------------------------- */
/*          ISR config           */
/* ----------------------------- */
struct edge_event {
    uint32_t ticks;
};
static struct gpio_callback ir_rx_cb_data;

K_MSGQ_DEFINE(edge_msgq, sizeof(struct edge_event), EDGE_QUEUE_DEPTH, 4);

static void ir_rx_isr(const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins);


/* ----------------------------- */
/*        Frame config           */
/* ----------------------------- */
static uint32_t edge_tick[MAX_EDGES];
static size_t edge_count;
// Can cache the frequency at startup so ticks_to_us() doesn't
// have to call into the driver on every edge.
static uint32_t counter_freq;

static void frame_complete_handler(struct k_work *work);
static inline uint32_t ticks_to_us(uint32_t delta_ticks);
static void emit_frame(void);

// For the idle gap detector
static K_WORK_DELAYABLE_DEFINE(frame_complete_work, frame_complete_handler);




int main(void)
{
    int err;

    if (!(err = gpio_is_ready_dt(&ir_rx_pin)))
    {
        LOG_ERR("IR GPIO not ready! (err: %d)", err);
        return -ENODEV;
    }


    if (!(err = device_is_ready(counter_dev)))
    {
        LOG_ERR("IR counter device not ready! (err: %d)", err);
        return -ENODEV;
    }


    // Checking for 0 to avoid div by zero errors coming up
    // later. Although 0 in itself is most likely invalid anyway.
    if ((counter_freq = counter_get_frequency(counter_dev)) == 0)
    {
        LOG_ERR("IR counter reports an invalid frequency of 0");
        return -EINVAL;
    }

    // Start the counter
    // the counter should count up at 1mhz. After bit more
    // than an hour it should overflow and restart. This is
    // not a concern.
    if ((err = counter_start(counter_dev)) != 0)
    {
        LOG_ERR("Starting IR counter failed! (err: %d)", err);
        return err;
    }
    LOG_INF("IR counter device ready!");

    if ((err = gpio_pin_configure_dt(&ir_rx_pin, GPIO_INPUT)) != 0)
    {
        LOG_ERR("Failed to configure IR receiver pin! (err: %d)", err);
        return err;
    }


    // The VS1838B receiver will give a digital output
    // corresponding to the received IR signal.
    if ((err = gpio_pin_interrupt_configure_dt(&ir_rx_pin, GPIO_INT_EDGE_BOTH)) != 0)
    {
        LOG_ERR("Failed to configure GPIO edge interrupt! (err: %d)", err);
        return err;
    }
    LOG_INF("IR receiver device ready!");


    gpio_init_callback(&ir_rx_cb_data, ir_rx_isr, BIT(ir_rx_pin.pin));
    gpio_add_callback(ir_rx_pin.port, &ir_rx_cb_data);


    return 0;

}




/* ----------------------------- */
/*          ISR Activity         */
/* ----------------------------- */
static void ir_rx_isr(const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins)
{
    ARG_UNUSED(port);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);

    struct edge_event ev;

    if (counter_get_value(counter_dev, &ev.ticks) != 0)
        return;


    (void)k_msgq_put(&edge_msgq, &ev, K_NO_WAIT);
}





/* ----------------------------- */
/*             Frame             */
/* ----------------------------- */




static inline uint32_t ticks_to_us(uint32_t delta_ticks)
{
    return (uint32_t)(((uint64_t)delta_ticks * 1000000U) / counter_freq);
}

static void emit_frame(void)
{
    // incomplete packet
    if (edge_count < 2)
        return;


    size_t n_durations = edge_count -1;

    // The format requires alternating mark/space pairs. If
    // we somehow get an odd number (incomplete packet),
    // then just drop the last one.
    if (n_durations & 1U)
        n_durations--;

    if (n_durations == 0)
        return;


    static char output_buf[3600];
    int offset = 0;

    offset += snprintf(output_buf + offset, sizeof(output_buf) - offset, "data:");

    for (size_t i = 0; i < n_durations; i++) {
        uint32_t delta = edge_tick[i + 1] - edge_tick[i];
        offset += snprintf(output_buf + offset, sizeof(output_buf) - offset,
                           " %u", ticks_to_us(delta));
    }

    LOG_INF("Captured IR packet");
    LOG_INF("type: raw");
    LOG_INF("frequency: 38000");
    LOG_INF("duty_cycle: 0.330000");
    LOG_INF("%s", output_buf);
}


// Run when the IR has been silent for FRAME_GAP_MS
static void frame_complete_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    emit_frame();

    edge_count = 0;
}


/* ----------------------------- */
/*       Consumer Thread         */
/* ----------------------------- */

static void ir_rx_consumer_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);


    struct edge_event ev;


    while (1)
    {
        k_msgq_get(&edge_msgq, &ev, K_FOREVER);

        if (edge_count < MAX_EDGES)
            edge_tick[edge_count++] = ev.ticks;
        else
            LOG_WRN("Truncated frame due to MAX_EDGES limit!");

        // reset the frame end counter
        k_work_reschedule(&frame_complete_work, K_MSEC(FRAME_GAP_MS));
    }

}

// Always running with cooperative priority
K_THREAD_DEFINE(ir_rx_consumer_tid, 1024, ir_rx_consumer_thread,
        NULL, NULL, NULL,
        K_PRIO_COOP(7), 0, 0);
