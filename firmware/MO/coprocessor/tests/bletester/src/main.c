#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/data.h>
#include <zephyr/bluetooth/assigned_numbers.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_core.h>

LOG_MODULE_REGISTER(bletester, LOG_LEVEL_INF);

static const uint8_t ad_flags = BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR;

static const struct bt_data ad[] = {
	BT_DATA(BT_DATA_FLAGS, &ad_flags, sizeof(ad_flags)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

int main(void)
{
	int err;

	LOG_INF("bletester starting");

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("bt_enable failed (err: %d)", err);
		return err;
	}

	LOG_INF("Bluetooth initialized, advertising as \"%s\"", CONFIG_BT_DEVICE_NAME);

	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err) {
		LOG_ERR("Advertising failed to start (err: %d)", err);
		return err;
	}

	LOG_INF("Advertising started successfully");

	return 0;
}
