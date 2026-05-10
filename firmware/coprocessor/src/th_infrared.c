#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/toolchain.h>

#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/pwm/pwm.h>

#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/counter.h>

#include <zephyr/sys/util.h>
#include <zephyr/sys/util_macro.h>

#include <zephyr/logging/log.h>
#include <zephyr/logging/log_core.h>


LOG_MODULE_REGISTER(TH_IR, LOG_LEVEL_INF);


// TX config
#define IR_CARRIER_HZ   38000U
#define IR_PERIOD_NS    PWM_HZ(IR_CARRIER_HZ)
#define IR_PULSE_NS     (IR_PERIOD_NS / 3U)   /* ~33% duty */

// RX config
#define MAX_EDGES        500
#define FRAME_GAP_MS     150
#define EDGE_QUEUE_DEPTH 64 // ISR thread handoff buffer


static const struct pwm_dt_spec ir_tx_pwm = PWM_DT_SPEC_GET(DT_NODELABEL(ir_tx_led));
static const struct gpio_dt_spec ir_rx_pin = GPIO_DT_SPEC_GET(DT_NODELABEL(ir_rx_pin), gpios);
static const struct device *const rx_counter_dev = DEVICE_DT_GET(DT_NODELABEL(timer2));

static bool ir_tx_setup_done = false;
static bool ir_rx_setup_done = false;
static bool ir_rx_watch_once = true;

static int8_t ir_tx_setup();
static int8_t ir_tx(const uint32_t *duration_us, size_t count);
static int ir_txt_process_sample_count(char *input);
static int ir_txt_process(char *input, uint32_t *raw_frame, int num_samples);

static int8_t ir_rx_setup();
static int8_t rx_init_handler();
static int8_t rx_deinit_handler();
static int8_t rx_watch_once();
static int8_t rx_watch_continuous();
static void emit_frame();
static void frame_complete_handler(struct k_work *work);
static inline uint32_t ticks_to_us(uint32_t delta_ticks);

//RX ISR config
struct edge_event {
    uint32_t ticks;
};
struct gpio_callback ir_rx_cb_data;
K_MSGQ_DEFINE(edge_msgq, sizeof(struct edge_event), EDGE_QUEUE_DEPTH, 4);
static void ir_rx_isr(const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins);

// RX frame config
static uint32_t edge_tick[MAX_EDGES];
static size_t edge_count;
// Can cache the frequency at startup so ticks_to_us() doesn't
// have to call into the driver on every edge.
static uint32_t counter_freq;


// For the idle gap detector
static K_WORK_DELAYABLE_DEFINE(frame_complete_work, frame_complete_handler);


/* ----------------------------- */
/*           dispatch            */
/* ----------------------------- */

int8_t ir_argument_dispatch(char *input)
{
    int err;
    char *unprocessed;
    char *token = strtok_r(input, " ", &unprocessed);

    if (token == NULL)
    {
        LOG_WRN("Incomplete IR command! (%s)", input);
        return -EINVAL;
    }


    if (strcmp(token, "play") == 0)
    {
        if (!ir_tx_setup_done)
        {
            if ((err = ir_tx_setup()) != 0)
            {
                LOG_ERR("IR_RX setup failed! (err: %d)", err);
                return err;
            }
        }

        int num_samples = ir_txt_process_sample_count(unprocessed);
        uint32_t raw_frame[num_samples];
        if ((err = ir_txt_process(unprocessed, raw_frame, num_samples)) != 0)
        {
            LOG_ERR("String processing error! (err: %d)", err);
            return err;
        }

        return ir_tx(raw_frame, num_samples);
    }
    else if (strcmp(token, "record") == 0)
    {
        if (!ir_rx_setup_done)
        {
            if ((err = ir_rx_setup()) != 0)
            {
                LOG_ERR("IR_RX setup failed! (err: %d)", err);
                return err;
            }
        }

        token = strtok_r(NULL, " ", &unprocessed);
        if (token == NULL)
        {
            LOG_INF("Waiting for IR signal...");
            return rx_watch_once();
        }
        else if (strcmp(token, "con") == 0)
        {
            LOG_INF("Waiting for IR signal...");
            return rx_watch_continuous();
        }
        else if (strcmp(token, "stop") == 0)
        {
            LOG_INF("No longer recording IR");
            return rx_deinit_handler();
        }
    }

    return 0;
}



/* ----------------------------- */
/*              IR TX            */
/* ----------------------------- */

static int8_t ir_tx_setup()
{
    int err;

    if (!(err = pwm_is_ready_dt(&ir_tx_pwm)))
    {
        LOG_ERR("PWM device %s is not ready! (err: %d)", ir_tx_pwm.dev->name, err);
        return err;
    }


    /*
     * Make sure the carrier starts off. Setting period+pulse with
     * pulse=0 establishes the period without producing output.
     */
    if ((err = pwm_set_dt(&ir_tx_pwm, IR_PERIOD_NS, 0)) != 0)
    {
        LOG_ERR("PWM initial set failed! (err: %d)", err);
        return err;
    }

    LOG_INF("IR_TX module ready! Period=%ld ns, pulse=%ld ns", IR_PERIOD_NS, IR_PULSE_NS);

    ir_tx_setup_done = true;

    return 0;
}


static int ir_txt_process(char *input, uint32_t *raw_frame, int num_samples)
{
    int n;
    int consumed = 0;

    for (size_t i = 0; i < num_samples; i++)
    {
        if ((n = sscanf(input, "%u%n", &raw_frame[i], &consumed)) != 1)
        {
            LOG_ERR("Error processing user string, invalid data at index %d", i);
            return -EINVAL;
        }
        input += consumed;
    }


    return 0;
}

static int ir_txt_process_sample_count(char *input)
{
    int samples = 0;
    char current;
    char previous = ' ';
    size_t inputlen = strlen(input);

    for (size_t i = 0; i < inputlen; i++)
    {
        current = input[i];

        if (current != ' ' && previous == ' ')
            samples++;

        previous = input[i];
    }
    return samples;
}




// NOTE this function takes a binary input.
// I will either need to change it to take a text input or make
// a different function to translate it (probably the second).
static int8_t ir_tx(const uint32_t *duration_us, size_t count)
{

    if (!ir_tx_setup_done)
    {
        LOG_ERR("IR_TX function called before IR pins are setup!");
        return -1;
    }

    int err;

    for (size_t i = 0; i < count; i++)
    {
        const uint32_t us = duration_us[i];


        // Even offsets in the array are marks, odd ones are 0s
        if ((i & 1U) == 0U)
        {
            if ((err = pwm_set_pulse_dt(&ir_tx_pwm, IR_PULSE_NS)) != 0)
            {
                LOG_ERR("PWM pulse generation failed! (err: %d)", err);
                return err;
            }
            k_busy_wait(us);

            if ((err = pwm_set_pulse_dt(&ir_tx_pwm, 0)) != 0)
            {
                LOG_ERR("PWM off failed! (err: %d)", err);
                return err;
            }
        }
        else
        {
            k_busy_wait(us);
        }
    }
    return 0;
}


/* ----------------------------- */
/*              IR RX            */
/* ----------------------------- */

static int8_t ir_rx_setup()
{
    int err;

    if (!(err = gpio_is_ready_dt(&ir_rx_pin)))
    {
        LOG_ERR("IR_RX GPIO not ready! (err: %d)", err);
        return -ENODEV;
    }


    if (!(err = device_is_ready(rx_counter_dev)))
    {
        LOG_ERR("IR_RX counter device not ready! (err: %d)", err);
        return -ENODEV;
    }

    // Checking for 0 to avoid div by zero errors coming up
    // later. Although 0 in itself is most likely invalid anyway.
    if ((counter_freq = counter_get_frequency(rx_counter_dev)) == 0)
    {
        LOG_ERR("IR counter reports an invalid frequency of 0");
        return -EINVAL;
    }

    // Start the counter
    // the counter should count up at 1mhz. After bit more
    // than an hour it should overflow and restart. This is
    // not a concern.
    if ((err = counter_start(rx_counter_dev)) != 0)
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


    gpio_init_callback(&ir_rx_cb_data, ir_rx_isr, BIT(ir_rx_pin.pin));
    gpio_add_callback(ir_rx_pin.port, &ir_rx_cb_data);

    LOG_INF("IR receiver device ready!");
    ir_rx_setup_done = true;

    return 0;
}


static int8_t rx_init_handler()
{
    int err;
    // The VS1838B receiver will give a digital output
    // corresponding to the received IR signal.
    if ((err = gpio_pin_interrupt_configure_dt(&ir_rx_pin, GPIO_INT_EDGE_BOTH)) != 0)
    {
        LOG_ERR("Failed to configure GPIO edge interrupt! (err: %d)", err);
        return err;
    }
    return 0;
}


static int8_t rx_deinit_handler()
{
    int err;
    if ((err = gpio_pin_interrupt_configure_dt(&ir_rx_pin, GPIO_INT_DISABLE)))
    {
        LOG_ERR("Error occurred while trying to de-register the IR handler! (err: %d)", err);
        return err;
    }
    return 0;
}


static int8_t rx_watch_once()
{
    if (!ir_rx_setup_done)
    {
        LOG_ERR("IR_RX function called before receiver is setup!");
        return -EINVAL;
    }
    // ISR thread disables itself if true
    ir_rx_watch_once = true;

    return rx_init_handler();
}

static int8_t rx_watch_continuous()
{
    if (!ir_rx_setup_done)
    {
        LOG_ERR("IR_RX function called before receiver is setup!");
        return -EINVAL;
    }
    // ISR thread disables itself if true
    ir_rx_watch_once = false;

    return rx_init_handler();
}


// ISR thread
static void ir_rx_isr(const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins)
{
    ARG_UNUSED(port);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);

    struct edge_event ev;

    if (counter_get_value(rx_counter_dev, &ev.ticks) != 0)
        return;


    (void)k_msgq_put(&edge_msgq, &ev, K_NO_WAIT);
}



static inline uint32_t ticks_to_us(uint32_t delta_ticks)
{
    return (uint32_t)(((uint64_t)delta_ticks * 1000000U) / counter_freq);
}

static void emit_frame()
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

    if (ir_rx_watch_once)
        rx_deinit_handler();
}


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
