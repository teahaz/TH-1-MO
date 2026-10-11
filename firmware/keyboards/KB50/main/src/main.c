#include <stdint.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_core.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/dt-bindings/gpio/gpio.h>

#include "usb.h"
#include "keymap.h"
#include "keyboard.h"


LOG_MODULE_REGISTER(keybaord_main, LOG_LEVEL_INF);
void add_key_to_report(uint8_t key, uint8_t *keyreport);


int main(void)
{
    int err;
    if ((err = usbd_setup_helper()) != 0)
    {
        LOG_ERR("Failed to register USB device! (err: %d)\n", err);
        return err;
    }

    LOG_INF("USBD ready!");

    while (!hid_ready)
    {
        LOG_INF("Waiting for HID device to be ready...\n");
        k_sleep(K_SECONDS(1));
    }

    LOG_INF("HID ready!");

    if ((err = configure_keys()) != 0)
    {
        LOG_ERR("Failed to configure initial key states! (err: %d)", err);
        return err;
    }

    LOG_INF("Starting keyscan!");
    uint8_t offset_tmp = 0;
    uint64_t keyflags_tmp = 0;
    // by storing the previous keyflags we can avoid
    // computing if nothing changed.
    uint64_t keyflags_prev = 0;
    uint8_t keyreport[KB_BOOT_REPORT_SIZE] = {0};
    while (1)
    {
        // This checks what key is pressed
        refresh_key_state();

        if (keyflags_prev != keyflags)
        {
            memset(keyreport, 0, KB_BOOT_REPORT_SIZE);

            if(KFLAG_TEST(FN_KEY))
            {

                keyflags_tmp = keyflags;
                while (keyflags_tmp)
                {
                    printk("fn + offset: %d\n", offset_tmp);
                    offset_tmp = __builtin_ctzll(keyflags_tmp);
                    if (offset_tmp == FN_KEY)
                    {
                        keyflags_tmp &= (keyflags_tmp -1);
                        continue;
                    }

                    uint8_t ret = fnfunc_lookup[offset_tmp](offset_tmp);
                    // Some function keys just do their own thing.
                    // They signal this by returning -1.
                    // In this case we don't want to send anything.
                    if (ret < 0)
                        goto loop_end;

                    for (int i = 2; i < KB_BOOT_REPORT_SIZE; i++)
                    {
                        if (keyreport[i] == 0)
                        {
                            keyreport[i] = ret;
                            break;
                        }
                    }

                    keyflags_tmp &= (keyflags_tmp -1);
                }
            } else
            {
                keyflags_tmp = keyflags;
                while (keyflags_tmp)
                {
                    offset_tmp = __builtin_ctzll(keyflags_tmp);
                    printk("offset: %d\n", offset_tmp);

                    // Both the mod and regular keys can be added
                    // since if they are not set they are just 0.
                    keyreport[0] += modval_lookup[offset_tmp];
                    for (int i = 2; i < KB_BOOT_REPORT_SIZE; i++)
                    {
                        if (keyreport[i] == 0)
                        {
                            keyreport[i] = keyval_lookup[offset_tmp];
                            break;
                        }
                    }

                    // This bit of magic unsets the last flag
                    // so it ctzll can calculate the next offset.
                    keyflags_tmp &= (keyflags_tmp -1);
                }
            }


            for (int j = 0; j < KB_BOOT_REPORT_SIZE; j++)
                printk("%02x ", keyreport[j]);
            printk("\n");
            kb_hid_send_report(keyreport);
            keyflags_prev = keyflags;
        }

loop_end:
        k_usleep(10000);
    }

}
