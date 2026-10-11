#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/toolchain.h>

#include <zephyr/sys/util.h>
#include <zephyr/sys/util_macro.h>

#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/dt-bindings/pwm/pwm.h>

#include <zephyr/logging/log.h>
#include <zephyr/logging/log_core.h>


LOG_MODULE_REGISTER(TH_1_MO_IR, LOG_LEVEL_INF);


// command message queue on uart1
#define MSG_SIZE 1040
static int rx_buf_pos;
static char rx_buf[MSG_SIZE];
K_MSGQ_DEFINE(uart_message_q, MSG_SIZE, 10, 4);
static const struct device *const uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart1));
static void command_callback(const struct device *unused1, void *unused2);
/* static void command_callback(const struct device *unused1, struct uart_event *unused2, void *unused3); */


// uart tx
#define IR_CARRIER_HZ   38000U
#define IR_PERIOD_NS    PWM_HZ(IR_CARRIER_HZ)
#define IR_PULSE_NS     (IR_PERIOD_NS / 3U)   /* ~33% duty */
static const struct pwm_dt_spec ir_tx_pwm = PWM_DT_SPEC_GET(DT_NODELABEL(ir_tx_led));

static int ir_txt_process_sample_count(char *input);
static int8_t ir_tx(const uint32_t *duration_us, size_t count);
static int ir_txt_process(char *input, uint32_t *raw_frame, int num_samples);

// uart rx
#define MAX_EDGES        500
#define FRAME_GAP_MS     150
#define EDGE_QUEUE_DEPTH 64 // ISR thread handoff buffer

static const struct device *const rx_counter_dev = DEVICE_DT_GET(DT_NODELABEL(timer3));
static const struct gpio_dt_spec ir_rx_pin = GPIO_DT_SPEC_GET(DT_NODELABEL(ir_rx_pin), gpios);

//RX ISR config
struct edge_event {
    uint32_t ticks;
};
struct gpio_callback ir_rx_cb_data;
K_MSGQ_DEFINE(edge_msgq, sizeof(struct edge_event), EDGE_QUEUE_DEPTH, 4);

// rx frame config
static uint32_t edge_tick[MAX_EDGES];
static size_t edge_count;
// Can cache the frequency at startup so ticks_to_us() doesn't
// have to call into the driver on every edge.
static uint32_t counter_freq;

static int8_t rx_init_handler();
static int8_t rx_deinit_handler();


static void emit_frame();
static void frame_complete_handler(struct k_work *work);
static inline uint32_t ticks_to_us(uint32_t delta_ticks);
static void ir_rx_consumer_thread(void *a, void *b, void *c);
static void ir_rx_isr(const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins);

// For the idle gap detector
static K_WORK_DELAYABLE_DEFINE(frame_complete_work, frame_complete_handler);

// general
static void print_uart(char *buf);



int8_t init_ir_subsys()
{

    int err;
    char cmd_buf[MSG_SIZE];

    if (!(err = device_is_ready(uart_dev)))
    {
        LOG_ERR("Input uart device is not ready! (err: %d)", err);
        return err;
    }


    if ((err = uart_irq_callback_user_data_set(uart_dev, command_callback, NULL)) < 0)
    {
        if (err == -ENOTSUP)
            LOG_ERR("Interrupt-driven UART API support not enabled! (err: %d)", err);
        else if (err == -ENOSYS)
            LOG_ERR("UART device does not support interrupt-driven API! (err: %d)", err);
        else
            LOG_ERR("Error setting uart callback! (err: %d)", err);
        return err;
    }

    uart_irq_rx_enable(uart_dev);
    LOG_INF("Uart1 initialised!");


    if (!(err = pwm_is_ready_dt(&ir_tx_pwm)))
    {
        LOG_ERR("PWM device %s is not ready or does not exist! (err: %d)", ir_tx_pwm.dev->name, err);
        return err;
    }


    if ((err = pwm_set_dt(&ir_tx_pwm, IR_PERIOD_NS, 0)) != 0)
    {
        LOG_ERR("PWM initial set failed! (err: %d)", err);
        return err;
    }

    LOG_INF("tx module ready! Period=%ld ns, pulse=%ld ns", IR_PERIOD_NS, IR_PULSE_NS);


    if (!(err = gpio_is_ready_dt(&ir_rx_pin)))
    {
        LOG_ERR("rx GPIO not ready! (err: %d)", err);
        return -ENODEV;
    }


    if (!(err = device_is_ready(rx_counter_dev)))
    {
        LOG_ERR("Counter device not ready! (err: %d)", err);
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

    if ((err = gpio_pin_configure_dt(&ir_rx_pin, GPIO_INPUT)) != 0)
    {
        LOG_ERR("Failed to configure receiver pin! (err: %d)", err);
        return err;
    }


    // TODO: not done here
    if ((err = gpio_pin_interrupt_configure_dt(&ir_rx_pin, GPIO_INT_EDGE_BOTH)) != 0)
    {
        LOG_ERR("Failed to setup interrupt for rx! (err: %d)", err);
        return err;
    }

    gpio_init_callback(&ir_rx_cb_data, ir_rx_isr, BIT(ir_rx_pin.pin));

    if ((err = gpio_add_callback(ir_rx_pin.port, &ir_rx_cb_data)) != 0)
    {
        LOG_ERR("Failed to add callback to interrupt on rx! (err: %d)", err);
        return err;
    }

    LOG_INF("rx module ready!");


    while (k_msgq_get(&uart_message_q, &cmd_buf, K_FOREVER) == 0)
    {
        cmd_buf[MSG_SIZE-1] = '\0';

        rx_deinit_handler();

        LOG_INF("Sending IR command: %s", cmd_buf);

        int num_samples = ir_txt_process_sample_count(cmd_buf);
        uint32_t raw_frame[num_samples];

        if ((err = ir_txt_process(cmd_buf, raw_frame, num_samples)) != 0)
        {
            LOG_ERR("String processing error! (err: %d)", err);
            return err;
        }

        ir_tx(raw_frame, num_samples);

        LOG_INF("Sent IR command!");

        // making sure init and deinit don't happen
        // without break; Don't think it will, but
        // don't like the idea of it happening.
        // Can remove/reduce if command rate too low.
        k_sleep(K_SECONDS(0.5));
        rx_init_handler();
        k_sleep(K_SECONDS(0.5));
    }

    return 0;
}


static void command_callback(const struct device *unused1, void *unused2)
{
    ARG_UNUSED(unused1);
    ARG_UNUSED(unused2);

    uint8_t c;

    if (!uart_irq_update(uart_dev))
        return;

    while (uart_fifo_read(uart_dev, &c, 1) == 1)
    {
        if ((c == '\n' || c == '\r') && rx_buf_pos > 0)
        {
            rx_buf[rx_buf_pos] = '\0';

            k_msgq_put(&uart_message_q, &rx_buf, K_NO_WAIT);

            rx_buf_pos = 0;
        }
        else if (rx_buf_pos < (sizeof(rx_buf) - 1))
        {
            rx_buf[rx_buf_pos++] = c;
        }
    }
}

static void print_uart(char *buf)
{
    int msg_len = strlen(buf);
    for (int i = 0; i < msg_len; i++) {
        uart_poll_out(uart_dev, buf[i]);
    }
}






// ------------------ IR TX Stuff ---------------------
//
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
static int8_t ir_tx(const uint32_t *duration_us, size_t count)
{
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


// ------------------ IR RX Stuff ---------------------
//

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


static void emit_frame()
{
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

    /* offset += snprintf(output_buf + offset, sizeof(output_buf) - offset, "data:"); */

    for (size_t i = 0; i < n_durations; i++) {
        uint32_t delta = edge_tick[i + 1] - edge_tick[i];
        offset += snprintf(output_buf + offset, sizeof(output_buf) - offset,
                           " %u", ticks_to_us(delta));
    }

    print_uart(output_buf);
    print_uart("\r\n");
}


static inline uint32_t ticks_to_us(uint32_t delta_ticks)
{
    return (uint32_t)(((uint64_t)delta_ticks * 1000000U) / counter_freq);
}


// Run when the IR has been silent for FRAME_GAP_MS
static void frame_complete_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    emit_frame();

    edge_count = 0;
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
