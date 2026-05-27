#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/toolchain.h>

#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/util_macro.h>

#include <zephyr/drivers/gpio.h>

#include <zephyr/logging/log.h>
#include <zephyr/logging/log_core.h>

#include <zephyr/dt-bindings/gpio/gpio.h>


#include "ir_subsys.h"


LOG_MODULE_REGISTER(TH_1_MO, LOG_LEVEL_INF);


// program select
static const struct gpio_dt_spec ps_inputs[] = {
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), ps_gpios, 0),
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), ps_gpios, 1),
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), ps_gpios, 2),
    GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), ps_gpios, 3),
};
#define INPUT_PINS_MASK (BIT(ps_inputs[0].pin) | BIT(ps_inputs[1].pin) |  BIT(ps_inputs[2].pin) | BIT(ps_inputs[3].pin))
gpio_port_value_t ps_input_port;
uint8_t ps_value;


int main()
{
    int err;

    for (int i = 0; i < ARRAY_SIZE(ps_inputs); i++)
    {
        if (!(err = gpio_is_ready_dt(&ps_inputs[i])))
        {
            LOG_ERR("Program select input pin %d not ready! (err: %d)", i, err);
            return -1;
        }
        if (( err = gpio_pin_configure_dt(&ps_inputs[i], GPIO_INPUT)) != 0)
        {
            LOG_ERR("Failed to configure program select pin %d! (err: %d)", i, err);
            return -1;
        }
    }


    ps_value = 0;

    for (int i = 0; i < 4; i++) {
        ps_value |= (gpio_pin_get_raw(ps_inputs[i].port, ps_inputs[i].pin) << i);
    }

    switch (ps_value)
    {
        case 4:
            if ((err = init_ir_subsys()) != 0)
                LOG_ERR("IR Rx init failed! (err: %d)", err);
            return err;


        case 0:
        case 1:
        case 2:
        case 3:
        case 5:
        case 6:
        case 7:
        case 8:
        case 9:
        case 10:
        case 11:
        case 12:
        case 13:
        case 14:
        case 15:
        default:
PS_UNIMPLEMENTED:
            LOG_ERR("Unimplemented program select value %d", ps_value);
            return -1;
    }

}


