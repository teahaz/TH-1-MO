#ifndef TH_BLE_H
#define TH_BLE_H
#include <stdint.h>
#include <stdlib.h>


#define MAX_NAME_LEN 256

int8_t ble_initialise();
int8_t ble_argument_dispatch(char *input);

int8_t scan_stop();
int8_t scan_start();
int8_t search_company_id(uint16_t cid, char *c_name, size_t c_name_size);

#endif
