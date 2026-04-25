#include <stdint.h>
#include <string.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>

#include <zephyr/logging/log.h>
#include <zephyr/logging/log_core.h>

#include "th_ble.h"


#define UART_DEVICE_NODE DT_CHOSEN(zephyr_shell_uart)S


#define MSG_SIZE 128


// store up to 10 messages aligned to 4-byte boundary in a queue
K_MSGQ_DEFINE(uart_msgq, MSG_SIZE, 10, 4);
LOG_MODULE_REGISTER(TH_IO, LOG_LEVEL_INF);


static int rx_buf_pos;
static char rx_buf[MSG_SIZE];

const struct device *const uart_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

void print_uart(char *buf);
void serial_cb(const struct device *dev, void *user_data);
int8_t argument_dispatch(char *input);

int main(void)
{
    int err;
    char tx_buf[MSG_SIZE];

    if (!device_is_ready(uart_dev))
    {
        LOG_ERR("Uart device is not ready!");
        return -1;
    }
    // configure interrupt and callback to receive data
    err = uart_irq_callback_user_data_set(uart_dev, serial_cb, NULL);
    if (err < 0)
    {
        if (err == -ENOTSUP)
            LOG_ERR("Interrupt-driver UART API support not enabled!");
        else if (err == -ENOSYS)
            LOG_ERR("UART device does not support interrupt-driven API!");
        else
            LOG_ERR("Error setting UART callback! (err: %d)", err);

        return 0;
    }

    uart_irq_rx_enable(uart_dev);
    LOG_INF("System initialised!");

    while (k_msgq_get(&uart_msgq, &tx_buf, K_FOREVER) == 0)
    {
        // I don't trust this thing
        tx_buf[MSG_SIZE-1] = '\0';
        print_uart(tx_buf);
        print_uart("\r\n");

        argument_dispatch(tx_buf);
    }
    return 0;

}


/*
 * Read characters from UART until line end is detected. Afterwards push the
 * data to the message queue.
 */
void serial_cb(const struct device *dev, void *user_data)
{
    uint8_t c;

    if (!uart_irq_update(uart_dev))
        return;

    if (!uart_irq_rx_ready(uart_dev))
        return;

    /* read until FIFO empty */
    while (uart_fifo_read(uart_dev, &c, 1) == 1)
    {
        if ((c == '\n' || c == '\r') && rx_buf_pos > 0)
        {
            /* terminate string */
            rx_buf[rx_buf_pos] = '\0';

            /* if queue is full, message is silently dropped */
            k_msgq_put(&uart_msgq, &rx_buf, K_NO_WAIT);

            /* reset the buffer (it was copied to the msgq) */
            rx_buf_pos = 0;
        }
        else if (rx_buf_pos < (sizeof(rx_buf) - 1))
        {
            rx_buf[rx_buf_pos++] = c;
        }
        /* else: characters beyond buffer size are dropped */
    }
}

/*
 * Print a null-terminated string character by character to the UART interface
 */
void print_uart(char *buf)
{
    int msg_len = strlen(buf);

    for (int i = 0; i < msg_len; i++) {
        uart_poll_out(uart_dev, buf[i]);
    }
}


/*
 * Dispatch the base level command (first word) to the 
 * correct sub-dispatcher.
 */
int8_t argument_dispatch(char *input)
{
    char *unprocessed;
    char *token = strtok_r(input, " ", &unprocessed);

    if (token == NULL)
        return -EINVAL;


    if (strcmp(token, "ble") == 0)
    {
        ble_argument_dispatch(unprocessed);
        return 0;
    }


    if (strcmp(input, "help") == 0)
    {
        LOG_INF("help");
        return 0;
    }

    LOG_WRN("Invalid command! (%s %s)", input, unprocessed);
    return -ENOSYS;
}
