#include <stdint.h>
#include <string.h>
#include <sys/errno.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util_macro.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_core.h>
#include <zephyr/toolchain/gcc.h>
#include <zephyr/usb/usbd_msg.h>
#include <zephyr/usb/class/hid.h>
#include <zephyr/usb/class/usbd_hid.h>
#include <zephyr/drivers/usb/udc_buf.h>


#include "usb.h"

static void kb_iface_ready(const struct device *dev, const bool ready);
static uint32_t kb_get_idle(const struct device *dev, const uint8_t id);
static void kb_set_protocol(const struct device *dev, const uint8_t proto);
static void kb_set_idle(const struct device *dev, const uint8_t id, const uint32_t duration);
static void kb_output_report(const struct device *dev, const uint16_t len, const uint8_t *const buf);
static int kb_get_report(const struct device *dev, const uint8_t type, const uint8_t id, const uint16_t len, uint8_t *const buf);
static int kb_set_report(const struct device *dev, const uint8_t type, const uint8_t id, const uint16_t len, const uint8_t *const buf);
static void msg_cb(struct usbd_context *const ctx, const struct usbd_msg *const msg);


LOG_MODULE_REGISTER(kb_usb, LOG_LEVEL_INF);



static const uint8_t hid_report_desc[] = HID_KEYBOARD_REPORT_DESC();

struct kb_report_msg {
    uint8_t report[KB_BOOT_REPORT_SIZE];
};

K_MSGQ_DEFINE(kb_report_q, sizeof(struct kb_report_msg), 2, 4);


UDC_STATIC_BUF_DEFINE(tx_report, KB_BOOT_REPORT_SIZE);

const struct device *hid_dev;
struct usbd_context *usbd_ctx;
bool hid_ready;


const struct hid_device_ops kb_ops = {
    .iface_ready   = kb_iface_ready,
    .get_report    = kb_get_report,
    .set_report    = kb_set_report,
    .set_idle      = kb_set_idle,
    .get_idle      = kb_get_idle,
    .set_protocol  = kb_set_protocol,
    .output_report = kb_output_report,
};



int usbd_setup_helper()
{
    int err;

    hid_dev = DEVICE_DT_GET_ONE(zephyr_hid_device);
    if (!device_is_ready(hid_dev))
    {
        LOG_ERR("HID device not ready");
        return -EIO;
    }

    if((err = hid_device_register(hid_dev, hid_report_desc, sizeof(hid_report_desc), &kb_ops)) != 0)
    {
        LOG_ERR("hid_device_register failed %d", err);
        return err;
    }

    // NOTE: CONFIG_USBD_HID_SET_POLLING_PERIOD has to be enabled!
    (void)hid_device_set_in_polling(hid_dev, 1000);
    err = hid_device_set_out_polling(hid_dev, 1000);
    if (err != 0 && err != -ENOTSUP)
        LOG_WRN("hid_device_set_out_polling warning: %d", err);


    usbd_ctx = app_usbd_init(msg_cb);
    if (usbd_ctx == NULL)
    {
        LOG_ERR("sample_usbd_init_device failed");
        return -ENODEV;
    }


    if (!usbd_can_detect_vbus(usbd_ctx))
        if ((err = usbd_enable(usbd_ctx)) != 0 )
        {
            LOG_ERR("Running usbd_enable failed! (err: %d)\n", err);
            return err;
        }

    return 0;
}







static void kb_iface_ready(const struct device *dev, const bool ready)
{
    ARG_UNUSED(dev);
    hid_ready = ready;
}


static int kb_get_report(const struct device *dev,
                         const uint8_t type,
                         const uint8_t id,
                         const uint16_t len,
                         uint8_t *const buf)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(type);
    ARG_UNUSED(id);
    ARG_UNUSED(len);
    ARG_UNUSED(buf);
    return -ENOTSUP;
}

static int kb_set_report(const struct device *dev,
                         const uint8_t type, const uint8_t id, const uint16_t len,
                         const uint8_t *const buf)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(id);
    ARG_UNUSED(len);
    ARG_UNUSED(buf);

    if (type != HID_REPORT_TYPE_OUTPUT)
        return -ENOTSUP;

    return 0;
}


static uint32_t kb_idle;
static void kb_set_idle(const struct device *dev, const uint8_t id, const uint32_t duration)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(id);
	kb_idle = duration;
}

static uint32_t kb_get_idle(const struct device *dev, const uint8_t id)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(id);
	return kb_idle;
}

static void kb_set_protocol(const struct device *dev, const uint8_t proto)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(proto);
}
static void kb_output_report(const struct device *dev, const uint16_t len, const uint8_t *const buf)
{
    (void)kb_set_report(dev, HID_REPORT_TYPE_OUTPUT, 0U, len, buf);
}


static void msg_cb(struct usbd_context *const ctx, const struct usbd_msg *const msg)
{
    if (usbd_can_detect_vbus(ctx))
    {
        if(msg->type == USBD_MSG_VBUS_READY)
        {
            if(usbd_enable(ctx))
                LOG_ERR("usbd_enable failed");
        }
        else if (msg->type == USBD_MSG_VBUS_REMOVED)
        {
            if (usbd_disable(ctx))
                LOG_ERR("usbd_disable failed");
        }
    }
}


void kb_hid_send_report(uint8_t *keyreport)
{
    memcpy(tx_report, keyreport, KB_BOOT_REPORT_SIZE);
    hid_device_submit_report(hid_dev, KB_BOOT_REPORT_SIZE, tx_report);
}
