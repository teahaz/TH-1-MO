#include <zephyr/sleep.h>
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>

#include <zephyr/sys/util.h>
#include <zephyr/sys/printk.h>

#include <zephyr/logging/log.h>
#include <zephyr/logging/log_core.h>

LOG_MODULE_REGISTER(pincheck_pinout, LOG_LEVEL_INF);
/* All usable GPIO pads on the MDBT50Q-1MV2 / -P1MV2 module, except:
 *  - the two pins reserved for uart0 (P0.22 TX / P0.24 RX, see app.overlay)
 *  - the two pins reserved for the 32.768kHz crystal (P0.00/XL1, P0.01/XL2)
 *  - P0.18, left wired as nRESET (see &uicr in app.overlay)
 * Order matches the pin_gpios property in app.overlay. */
const struct gpio_dt_spec pin_ios[] = {
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 0),  /* pin 3  - P1.10 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 1),  /* pin 4  - P1.11 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 2),  /* pin 5  - P1.12 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 3),  /* pin 6  - P1.13 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 4),  /* pin 7  - P1.14 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 5),  /* pin 8  - P1.15 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 6),  /* pin 9  - P0.03 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 7),  /* pin 10 - P0.29 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 8),  /* pin 11 - P0.02 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 9),  /* pin 12 - P0.31 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 10), /* pin 13 - P0.28 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 11), /* pin 14 - P0.30 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 12), /* pin 16 - P0.27 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 13), /* pin 19 - P0.26 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 14), /* pin 20 - P0.04 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 15), /* pin 21 - P0.05 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 16), /* pin 22 - P0.06 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 17), /* pin 23 - P0.07 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 18), /* pin 24 - P0.08 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 19), /* pin 25 - P1.08 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 20), /* pin 26 - P1.09 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 21), /* pin 27 - P0.11 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 22), /* pin 29 - P0.12 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 23), /* pin 36 - P0.14 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 24), /* pin 37 - P0.13 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 25), /* pin 38 - P0.16 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 26), /* pin 39 - P0.15 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 27), /* pin 41 - P0.17 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 28), /* pin 42 - P0.19 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 29), /* pin 43 - P0.21 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 30), /* pin 44 - P0.20 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 31), /* pin 45 - P0.23 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 32), /* pin 47 - P1.00 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 33), /* pin 49 - P0.25 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 34), /* pin 50 - P1.02 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 35), /* pin 52 - P0.09 (NFC1) */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 36), /* pin 54 - P0.10 (NFC2) */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 37), /* pin 56 - P1.04 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 38), /* pin 57 - P1.06 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 39), /* pin 58 - P1.07 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 40), /* pin 59 - P1.05 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 41), /* pin 60 - P1.03 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), pin_gpios, 42), /* pin 61 - P1.01 */
};

/* Real pin (port.pin) for each index into pin_ios, for display purposes. */
const char *const pin_names[] = {
    "1.10", "1.11", "1.12", "1.13", "1.14", "1.15",
    "0.03", "0.29", "0.02", "0.31", "0.28", "0.30",
    "0.27", "0.26", "0.04", "0.05", "0.06", "0.07",
    "0.08", "1.08", "1.09", "0.11", "0.12", "0.14",
    "0.13", "0.16", "0.15", "0.17", "0.19", "0.21",
    "0.20", "0.23", "1.00", "0.25", "1.02", "0.09",
    "0.10", "1.04", "1.06", "1.07", "1.05", "1.03",
    "1.01",
};

/* Expected default level for each pin once mounted on the custom PCB.
 * Indices/layout line up with pin_names above. 0 = expect low, 1 = expect high. */
const int pin_expected[] = {
    0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0,
    0,
};



const int gpio_check_all(bool log)
{
    int err = 0;

    for (int i = 0; i < ARRAY_SIZE(pin_ios); i++)
    {
        if (log)
            printk("Checking pin: %d    ", i);

        if (!(err = gpio_is_ready_dt(&pin_ios[i])))
        {
            LOG_ERR("Pin %d is not ready!",i);
            return -err;
        }

        if (log)
        {
            k_sleep(K_MSEC(10));
            printk("✔\n");
        }
    }
    return !err;
}



const int gpio_set_input_all(bool log)
{
    int err = 0;


    for (int i = 0; i < ARRAY_SIZE(pin_ios); i++)
    {
        if (log)
            printk("Setting pin: %d    ", i);

        if ((err = gpio_pin_configure_dt(&pin_ios[i], GPIO_INPUT)) != 0)
        {
            LOG_ERR("Failed to set pin %d as input! (err: %d)", i, err);
            return -err;
        }

        if (log)
        {
            k_sleep(K_MSEC(10));
            printk("✔\n");
        }
    }

    return err;
}
