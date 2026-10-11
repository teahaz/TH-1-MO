#ifndef LOCAL_KEYMAP_HEADER
#define LOCAL_KEYMAP_HEADER

#include <zephyr/usb/class/hid.h>



/*
 * This file serves the single purpose to
 * define what actions each key does on the
 * keyboard.
 *
 * Everything is set by the KFLAG_LIST macro, which identifies
 * keys by their offset in the keyflags bitmask. (This is the
 * bitmask used internally to track key states.)
 *
 * Setting up this macro is a bit complex initially, but only needs
 * to be done once for a keyboard. To get the offset of any key,
 * do the following steps:
 *
 * 1. Enable logging over UART (if it isn't already)
 * 2. Initially set up the macro in whatever order,
 *    making sure that there is an uninterrupted
 *    offset count. (ie: there are no skipped numbers)
 * 3. Push a key.
 * 4. Check what offset is printed to UART.
 * 4. Update the macro.
 */


// This macro defines what each physical key does. In order to get
// the offset for configuring a keyboard
// | offset | key value | mod value | fn_function | Layer key (0 if N/A)
#define NUM_KEYS 12
#define KFLAG_LIST(X)\
        X( 0,         HID_KEY_A,                            0,        fn_layer, HID_KEY_1) \
        X( 1,         HID_KEY_D,                            0,        fn_layer, HID_KEY_4) \
        X( 2, HID_KEY_BACKSPACE,                            0,        fn_layer, HID_KEY_7) \
        X( 3,                 0,                            0,         fn_none,         0) \
        X( 4,         HID_KEY_B,                            0,        fn_layer, HID_KEY_2) \
        X( 5,         HID_KEY_E,                            0,        fn_layer, HID_KEY_5) \
        X( 6,     HID_KEY_SPACE,                            0,        fn_layer, HID_KEY_8) \
        X( 7,                 0,   HID_KBD_MODIFIER_LEFT_CTRL,        fn_layer, HID_KEY_0) \
        X( 8,         HID_KEY_C,                            0,        fn_layer, HID_KEY_3) \
        X( 9,         HID_KEY_F,                            0,        fn_layer, HID_KEY_6) \
        X(10,     HID_KEY_ENTER,                            0,        fn_layer, HID_KEY_9) \
        X(11,                 0,    HID_KBD_MODIFIER_LEFT_ALT,         fn_none,         0)
// NOTE: FN key must always be set
#define FN_KEY 3

#endif

