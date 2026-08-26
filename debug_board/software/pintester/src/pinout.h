#ifndef PINOUT_H_
#define PINOUT_H_

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>

extern const struct gpio_dt_spec pin_ios[43];
extern const char *const pin_names[43];
extern const int pin_expected[43];

/* Real pin (port.pin) for a given index into pin_ios, for display purposes. */
#define PIN_NAME(idx) (pin_names[(idx)])

const int gpio_check_all(bool log);
const int gpio_set_input_all(bool log);

#endif /* PINOUT_H_ */
