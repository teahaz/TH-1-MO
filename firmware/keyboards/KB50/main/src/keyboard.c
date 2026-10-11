#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_core.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/dt-bindings/gpio/gpio.h>

#include "keymap.h"
#include "keyboard.h"


LOG_MODULE_REGISTER(keyboard_helper, LOG_LEVEL_INF);


const struct gpio_dt_spec sources[NUM_SOURCES] = {
    GPIO_DT_SPEC_GET_BY_IDX(ZEPHYR_USER_NODE, source_gpios, 0), // Pin 2
    GPIO_DT_SPEC_GET_BY_IDX(ZEPHYR_USER_NODE, source_gpios, 1), // Pin 3
    GPIO_DT_SPEC_GET_BY_IDX(ZEPHYR_USER_NODE, source_gpios, 2), // Pin 21
};


const struct gpio_dt_spec inputs[NUM_INPUTS] = {
    GPIO_DT_SPEC_GET_BY_IDX(ZEPHYR_USER_NODE, input_gpios, 0),
    GPIO_DT_SPEC_GET_BY_IDX(ZEPHYR_USER_NODE, input_gpios, 1),
    GPIO_DT_SPEC_GET_BY_IDX(ZEPHYR_USER_NODE, input_gpios, 2),
    GPIO_DT_SPEC_GET_BY_IDX(ZEPHYR_USER_NODE, input_gpios, 3),
};

_Static_assert(NUM_SOURCES * NUM_INPUTS <= 64,
               "Maximum 64 keys supported at this moment.");


// Keyflags is effectively the "state" of the keyboard.
// Some functions, such as refresh_key_state() change
// this value. Such functions should be limited to functions
// directly called by the library user.
uint64_t keyflags = 0;


// Lookup tables for both the key value and the mod value
const uint8_t keyval_lookup[NUM_KEYS] = {
#define X(offset, keyval, modval, fn, funval) [offset] = keyval,
    KFLAG_LIST(X)
#undef X
};

const uint8_t modval_lookup[NUM_KEYS] = {
#define X(offset, keyval, modval, fn, funval) [offset] = modval,
    KFLAG_LIST(X)
#undef X
};


// What function should the fn key call
const fnfunc_t fnfunc_lookup[NUM_KEYS] = {
#define X(offset, keyval, modval, fn, funval) [offset] = fn,
    KFLAG_LIST(X)
#undef X
};

// What key should the fn_layer function set.
const uint8_t fnlayer_lookup[NUM_KEYS] = {
#define X(offset, keyval, modval, fn, funval) [offset] = funval,
    KFLAG_LIST(X)
#undef X
};




int configure_keys()
{
    int err;


    for (uint8_t i = 0; i < NUM_SOURCES; i++)
    {
        if (!gpio_is_ready_dt(&sources[i]))
        {
            LOG_ERR("Source pin %d not available\n", i);
            return -1;
        }

        if ((err = gpio_pin_configure_dt(&sources[i], GPIO_OUTPUT_ACTIVE)) !=0)
        {
            LOG_ERR("Failed configuring source pin %d (err: %d)\n", i, err);
            return -1;
        }
    }

    for (uint8_t i = 0; i < NUM_INPUTS; i++)
    {
        if (!gpio_is_ready_dt(&inputs[i]))
        {
            LOG_ERR("Input pin %d not available\n", i);
            return -2;
        }
        if ((err = gpio_pin_configure_dt(&inputs[i], GPIO_INPUT | GPIO_PULL_DOWN)) != 0)
        {
            LOG_ERR("Failed configuring input pin %d (err %d)\n", i, err);
            return -2;
        }
    }

    for (uint8_t i = 0; i < NUM_SOURCES; i++)
        gpio_pin_set_dt(&sources[i], 0);


    return 0;
}

// Note that this function changes global state
void refresh_key_state()
{
    for (uint8_t s = 0; s < NUM_SOURCES; s++)
    {
        gpio_pin_set_dt(&sources[s], 1);
        for (uint8_t i = 0; i < NUM_INPUTS; i++)
        {
            if (gpio_pin_get_dt(&inputs[i]) > 0)
                KFLAG_SET(s, i);
            else
                KFLAG_CLEAR(s, i);
        }
        gpio_pin_set_dt(&sources[s], 0);
    }
}




// Placeholder for function keys that do nothing
uint8_t fn_none(uint8_t offset)
{
    return offset;
}

uint8_t fn_layer(uint8_t offset)
{
    return fnlayer_lookup[offset];
}

