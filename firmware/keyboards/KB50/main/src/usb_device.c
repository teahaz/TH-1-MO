/*
 * This file is responsible for how the USB device appears
 * to the host. Its not responsible for sending any USB signals
 * and is kept separate so it can be interchanged.
 */
#include <stdint.h>
#include <sys/errno.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util_macro.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_core.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/toolchain/gcc.h>
#include <zephyr/usb/usbd_msg.h>
#include <zephyr/usb/class/hid.h>
#include <zephyr/usb/class/usbd_hid.h>
#include <zephyr/drivers/usb/udc_buf.h>



LOG_MODULE_REGISTER(app_usbd, LOG_LEVEL_INF);


USBD_DEVICE_DEFINE(app_usbd,
                   DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
                   0xbead, // VID
                   0x0001); // PID


USBD_DESC_LANG_DEFINE(app_lang);
USBD_DESC_MANUFACTURER_DEFINE(app_mfr, "bread");
USBD_DESC_PRODUCT_DEFINE(app_product, "bread HID keyboard");
USBD_DESC_SERIAL_NUMBER_DEFINE(app_sn);



USBD_DESC_CONFIG_DEFINE(fs_cfg_desc, "FS Configuration");


static const uint8_t cfg_attrib = 0;


USBD_CONFIGURATION_DEFINE(app_fs_config,
                          cfg_attrib,
                          50, // CONFIG_USB_MAX_POWER
                          &fs_cfg_desc);



static int add_basic_descriptors(void)
{
    int err;

    if ((err = usbd_add_descriptor(&app_usbd, &app_lang)) != 0)
        return err;

    if ((err = usbd_add_descriptor(&app_usbd, &app_mfr)) != 0)
        return err;

    if ((err = usbd_add_descriptor(&app_usbd, &app_product)) != 0)
        return err;

    if ((err = usbd_add_descriptor(&app_usbd, &app_sn)) != 0)
        return err;

    return 0;
}


struct usbd_context *app_usbd_init(usbd_msg_cb_t msg_cb)
{
    int err;

    if ((err = add_basic_descriptors()) != 0)
    {
        LOG_ERR("Descriptor assignments failed! (err: %d)", err);
        return NULL;
    }

    if ((err = usbd_add_configuration(&app_usbd, USBD_SPEED_FS, &app_fs_config)) != 0)
    {
        LOG_ERR("Add fs config failed! (err %d)", err);
        return NULL;
    }

    if ((err = usbd_register_all_classes(&app_usbd, USBD_SPEED_FS, 1, NULL)))
    {
        LOG_ERR("Register classes failed! (err: %d)", err);
        return NULL;
    }


    if (msg_cb)
        if ((err = usbd_msg_register_cb(&app_usbd, msg_cb)) != 0)
        {
            LOG_ERR("Msg CB register failed! (err: %d)", err);
            return NULL;
        }


    if ((err = usbd_init(&app_usbd)) != 0)
    {
        LOG_ERR("usbd_init failed! (err: %d)", err);
    }


    return &app_usbd;
}
