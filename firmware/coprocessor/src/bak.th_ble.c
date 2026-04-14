#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/printk.h>

#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/att.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/assigned_numbers.h>


static void connected(struct bt_conn *conn, uint8_t err);
static void disconnected(struct bt_conn *conn, uint8_t reason);
static bool data_cb(struct bt_data *data, void *user_data);
static void device_found (const bt_addr_le_t *addr, int8_t rssi, uint8_t type, struct net_buf_simple *ad);
static uint8_t discover_callback(struct bt_conn *conn, const struct bt_gatt_attr *attr, struct bt_gatt_discover_params *params);


static struct bt_conn *target_conn;
static struct bt_le_scan_param scan_param = {
    .type      = BT_LE_SCAN_TYPE_ACTIVE,
    .options   = BT_LE_SCAN_OPT_NONE,
    .interval  = BT_GAP_SCAN_FAST_INTERVAL,
    .window    = BT_GAP_SCAN_FAST_WINDOW
};

struct bt_uuid_16 uuid = BT_UUID_INIT_16(0);
struct bt_gatt_discover_params discover_params = {0};
static bool restart_scanner = true;

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
};


int ble_scan()
{
    int err;

    err = bt_enable(NULL);
    if (err)
    {
        printk("Bluetooth init failed.. (err: %d)\n", err);
        return -1;
    }

    while (true)
    {
        if (restart_scanner)
        {
            printk("Starting up scanner...\n");
            err = bt_le_scan_start(&scan_param, device_found);
            if (err)
            {
                printk("Failed to start scanning... (err: %d)\n", err);
                return -2;
            }
            restart_scanner = false;
        }
        k_sleep(K_SECONDS(1));
    }
    return 0;
}




static bool data_cb(struct bt_data *data, void *user_data)
{
    char *name = user_data;
    uint8_t len;

    switch (data->type)
    {
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


static void device_found (const bt_addr_le_t *addr, int8_t rssi, uint8_t type, struct net_buf_simple *ad)
{
    int err;
    if (target_conn)
    {
        printk("ignoring\n");
        return;
    }

    char addr_str[BT_ADDR_LE_STR_LEN];
    char name[] = "[Unknown]";

    bt_data_parse(ad, data_cb, name);
    bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));

    if ((err = bt_le_scan_stop()) != 0)
    {
        printk("ERROR: Failed to stop scanning... (err: %d)\n", err);
        return;
    }


    /* if (rssi < -60) */
    /* { */
    /*     printk("Target found, but signal connection too low. (rssi: %d)\nRetrying in 1 second...\n", rssi); */
    /*     k_sleep(K_SECONDS(1)); */
    /*     restart_scanner = true; */
    /*     return; */
    /* } */


    printk("Target found!\n");

    if ((err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN, BT_LE_CONN_PARAM_DEFAULT, &target_conn) != 0))
    {
        printk("Failed to connect to target.. (err: %d)\n", err);
        return;
    }

}




static void connected(struct bt_conn *conn, uint8_t err)
{
    printk("Connected callback\n");
    int error;

    discover_params.uuid = NULL;
    discover_params.func = discover_callback;
    discover_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
    discover_params.end_handle   = BT_ATT_LAST_ATTRIBUTE_HANDLE;
    discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

    if ((error = bt_gatt_discover(conn, &discover_params)) != 0)
    {
        printk("Attribute discovery failed.. (err: %d)\n", err);
        return;
    }
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    printk("disconnected callback\n");
    printk("client disconnected. Reason: %d", reason);
    restart_scanner = true;
    target_conn = NULL;
    k_sleep(K_SECONDS(1));
    return;
}


static uint8_t discover_callback(struct bt_conn *conn, const struct bt_gatt_attr *attr, struct bt_gatt_discover_params *params)
{
    printk("Discover callback\n");
    if (!attr)
    {
        printk("Discovery complete!\n");
        (void)memset(params, 0, sizeof(*params));
        return BT_GATT_ITER_STOP;
    }

    char uuid_str[BT_UUID_STR_LEN];

    bt_uuid_to_str(attr->uuid, uuid_str, sizeof(uuid_str));
    printk("[ATTRIBUTE] Handle: %u, UUID: %s\n", attr->handle, uuid_str);


    if (params->type == BT_GATT_DISCOVER_PRIMARY || params->type == BT_GATT_DISCOVER_SECONDARY)
    {
        struct bt_gatt_chrc *val = (struct bt_gatt_chrc *)attr->user_data;
        printk("\tType: Service\n");
        printk("\tValue Handle: %u\n", val->value_handle);
        const struct bt_uuid *uuidl = val->uuid;
        bt_uuid_to_str(uuidl, uuid_str, sizeof(uuid_str));
        printk("\tUUID: %s\n", uuid_str);

    }
    else if (params->type == BT_GATT_DISCOVER_CHARACTERISTIC)
    {
        struct bt_gatt_chrc *val = (struct bt_gatt_chrc *)attr->user_data;
        printk("\tType: Characteristic\n");
        printk("\tValue Handle: %u\n", val->value_handle);
        printk("\tProperties: 0x%02x\n", val->properties);
    }
    else if (params->type == BT_GATT_DISCOVER_ATTRIBUTE)
    {
        struct bt_gatt_chrc *val = (struct bt_gatt_chrc *)attr->user_data;
        printk("\tType: Characteristic\n");
        printk("\tValue Handle: %u\n", val->value_handle);
        printk("\tProperties: 0x%02x\n", val->properties);
    }
    else if (params->type == BT_GATT_DISCOVER_DESCRIPTOR)
        printk("\tType: Descriptor");

    return BT_GATT_ITER_CONTINUE;
}


