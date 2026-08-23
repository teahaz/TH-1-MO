#include <zephyr/sleep.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>
#include <zephyr/logging/log_core.h>

#include <zephyr/drivers/gpio.h>
/* #include <zephyr/dt-bindings/gpio/gpio.h> */

#include "pinout.h"
LOG_MODULE_REGISTER(pincheck, LOG_LEVEL_INF);



int main(void)
{
    int err = 0;
    printk("Checking all pins are available...");
    if ((err = gpio_check_all(false)) == 0)
        printk("✔\n");
    else
    {
        printk("x\n");
        return -err;
    }

    printk("Setting all pins to input...");
    if ((err = gpio_set_input_all(false)) == 0)
        printk("✔\n");
    else
    {
        printk("x\n");
        return -err;
    }


    printk("Checking each pins default state...");
    int state = 0;
    int wrongstate_counter = 0;
    for (int i = 0; i < ARRAY_SIZE(pin_ios); i++)
    {
        state = gpio_pin_get_dt(&pin_ios[i]);
        if (state != pin_expected[i])
        {
            printk("x\n");
            LOG_WRN("GPIO pin %d (%s) is at state %d! Expected: %d", i, PIN_NAME(i), state, pin_expected[i]);
            wrongstate_counter++;
        }
        k_sleep(K_MSEC(10));
    }
    if (wrongstate_counter == 0)
        printk("✔\n");

    printk("Checking for bridged pins...");
    int jpin_state = 0;
    int bridgecounter = 0;

    for (int i = 0; i < ARRAY_SIZE(pin_ios); i++)
    {
        if ((err = gpio_pin_configure_dt(&pin_ios[i], GPIO_OUTPUT)) != 0)
        {
            printk("x\n");
            LOG_ERR("Failed to set pin %d (%s) to output mode! (err: %d)", i, PIN_NAME(i), err);
            return -err;
        }

        if ((err = gpio_pin_set_dt(&pin_ios[i], 1)) != 0)
        {
            printk("x\n");
            LOG_ERR("Failed to set the state of pin %d (%s)! (err: %d)", i, PIN_NAME(i), err);
            return -err;
        }

        for (int j = 0; j < ARRAY_SIZE(pin_ios); j++)
        {
            jpin_state = 0;

            if (i == j)
                continue;

            jpin_state = gpio_pin_get_dt(&pin_ios[j]);

            if (jpin_state == 1)
            {
                if (bridgecounter == 0)
                    printk("x\n");
                LOG_WRN("Pin %d (%s) is bridged with pin %d (%s)", i, PIN_NAME(i), j, PIN_NAME(j));
                bridgecounter++;
            }
        }

        if ((err = gpio_pin_set_dt(&pin_ios[i], 0)) != 0)
        {
            printk("x\n");
            LOG_ERR("Failed to set the state of pin %d (%s)! (err: %d)", i, PIN_NAME(i), err);
            return -err;
        }

        if ((err = gpio_pin_configure_dt(&pin_ios[i], GPIO_OUTPUT)) != 0)
        {
            printk("x\n");
            LOG_ERR("Failed to reset pin %d (%s) to input mode! (err: %d)", i, PIN_NAME(i), err);
            return -err;
        }
    }
    if (bridgecounter == 0)
        printk("✔\n");
}
