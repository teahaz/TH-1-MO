#ifndef TH_BLE_H
#define TH_BLE_H
#include <stdint.h>
#include <stdlib.h>


#define MAX_NAME_LEN 256

int8_t ble_initialise();

void scan_stop();
int8_t scan_start();
int8_t scan_once(int min_rssi);
uint8_t search_company_id(uint16_t cid, char *c_name, size_t c_name_size);

#endif
