#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/printk.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_core.h>

#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/att.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/assigned_numbers.h>

#include "th_ble.h"
#include "bt_company_ids.h"

LOG_MODULE_REGISTER(TH_BLE, LOG_LEVEL_INF);

static struct bt_conn *active_conn;
static int MIN_RSSI = 0;


/* static void connected(struct bt_conn *conn, uint8_t err); */
static bool data_cb(struct bt_data *data, void *user_data);
/* static void disconnected(struct bt_conn *conn, uint8_t reason); */
static void device_found (const bt_addr_le_t *addr, int8_t rssi, uint8_t type, struct net_buf_simple *ad);


static struct bt_le_scan_param scan_param = {
    .type      = BT_LE_SCAN_TYPE_ACTIVE,
    .options   = BT_LE_SCAN_OPT_NONE,
    .interval  = BT_GAP_SCAN_FAST_INTERVAL,
    .window    = BT_GAP_SCAN_FAST_WINDOW
};


static bool scanner_running = true;
struct bt_uuid_16 uuid = BT_UUID_INIT_16(0);
struct bt_gatt_discover_params discover_params = {0};


/* BT_CONN_CB_DEFINE(conn_callbacks) = { */
/*     .connected = connected, */
/*     .disconnected = disconnected, */
/* }; */
/*  */
int8_t ble_initialise()
{
    int err;
    if ((err = bt_enable(NULL)))
    {
        LOG_ERR("Bluetooth module initialisation failed! (err: %d)\n", err);
        return -1;
    }
    return 0;
}


int8_t scan_once(int min_rssi)
{
    int err;
    MIN_RSSI = min_rssi;
    if ((err = bt_le_scan_start(&scan_param, device_found)))
    {
        LOG_ERR("Failed to start scanning! (err: %d)", err);
        return -2;
    }
}

int8_t scan_start(int min_rssi)
{
    int err;
    MIN_RSSI = min_rssi;
    while (true)
    {
        k_usleep(100000);
        if (scanner_running)
        {
            if ((err = bt_le_scan_start(&scan_param, device_found)))
            {
                LOG_ERR("Failed to start scanning! (err: %d)", err);
                k_usleep(1000000);
                continue;
            }
            scanner_running = false;
        }
    }
    return 0;
}

void scan_stop()
{
    scanner_running = false;
}


static void device_found (const bt_addr_le_t *addr, int8_t rssi, uint8_t type, struct net_buf_simple *ad)
{
    int err;
    if (active_conn)
    {
        LOG_INF("Ignoring connection while other in place.");
        return;
    }

    char addr_str[BT_ADDR_LE_STR_LEN];
    char name[256] = "[Unknown]";

    bt_data_parse(ad, data_cb, name);
    bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));

    if ((err = bt_le_scan_stop()) != 0)
    {
        printk("ERROR: Failed to stop scanning... (err: %d)\n", err);
        return;
    }

    if (rssi < MIN_RSSI)
        return;

    LOG_INF("%s %s %d", name, addr_str, rssi);

    /* if ((err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN, BT_LE_CONN_PARAM_DEFAULT, &active_conn) != 0)) */
    /* { */
    /*     printk("Failed to connect to target.. (err: %d)\n", err); */
    /*     return; */
    /* } */
    scanner_running = true;

}


static bool data_cb(struct bt_data *data, void *user_data)
{
    char *name = user_data;
    uint8_t len;
    uint8_t name_len = 0;

    switch (data->type)
    {
        case BT_DATA_MANUFACTURER_DATA:
            // Need at least 2 bytes for company ID
            if (data->data_len < 3) {
                return true; // continue parsing
            }

            // Company ID is little-endian in the first 2 bytes
            uint16_t company_id = data->data[0] | (data->data[1] << 8);

            int cname_len;
            // returns 0 or negative if not found
            if ((cname_len = search_company_id(company_id, name, MAX_NAME_LEN/2)) > 0)
                name_len += cname_len;

            uint8_t br_type = data->data[2];

            switch (br_type)
            {
                case 0x12:
                    snprintf((char *)name+name_len, MAX_NAME_LEN - name_len,  "%s", "FindMy device ");
                    name_len += 14;
                    break;
                default:
                    snprintf((char *)name+name_len, MAX_NAME_LEN - name_len,  "%s", "device ");
                    name_len += 7;
            }

            name[name_len] = '\0';
            return true;

        case BT_DATA_NAME_COMPLETE:
        case BT_DATA_NAME_SHORTENED:
            // NOTE: 30 is the number that was defined in the original
            // bluetooth/sample/observer program. Not sure why.
            len = MIN(data->data_len, 30-1);
            memcpy(name, data->data, len);
            name[len] = '\0';
            return false;
        default: 
            return true;

    }
}

uint8_t search_company_id(uint16_t cid, char *c_name, size_t c_name_size)
{
    for (int i = 0; i < BT_COMPANY_IDS_COUNT; i++)
    {
        if (bt_company_ids[i].code == cid)
        {
            return snprintf(c_name, c_name_size, "%s", bt_company_ids[i].name);
        }
    }
    return -1;
}
