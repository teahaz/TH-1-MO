#ifndef LOCAL_KEYBOARD_HEADER
#define LOCAL_KEYBOARD_HEADER

#include <stdint.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/usb/class/hid.h>

// Keyflags is effectively the "state" of the keyboard.
// Some functions, such as refresh_key_state() change
// this value. Such functions should be limited to functions
// directly called by the library user.
extern uint64_t keyflags;

#define NUM_INPUTS 4
#define NUM_SOURCES 3

#define USERNODE DT_PATH(zephyr_user)
#define ZEPHYR_USER_NODE DT_PATH(zephyr_user)


// Macros for the keyflag bitmask
#define KBIT(source, input) ((source) * NUM_INPUTS + (input))

#define KFLAG_SET(source, input) \
    (keyflags |=  (1ULL << KBIT(source, input)))

#define KFLAG_CLEAR(source, input) \
    (keyflags &= ~(1ULL << KBIT(source, input)))

#define KFLAG_TEST(offset) (((keyflags) >> (offset)) & 1ULL)


// Function type for fn functions.
typedef uint8_t (*fnfunc_t)(uint8_t);

extern const uint8_t keyval_lookup[];
extern const uint8_t modval_lookup[];
extern const fnfunc_t fnfunc_lookup[];
extern const uint8_t fnlayer_lookup[];


extern const struct gpio_dt_spec sources[];
extern const struct gpio_dt_spec inputs[];

int configure_keys();
void refresh_key_state();


// Function keys
uint8_t fn_none(uint8_t offset);
uint8_t fn_layer(uint8_t offset);
#endif
