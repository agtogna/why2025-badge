#ifndef BLE_PARSE_H
#define BLE_PARSE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char scan_local_name[32];
    uint8_t name_len;
} ble_scan_local_name_t;

// Finds the complete/short local name in the AD structures of one advertisement.
// data comes from a stranger's radio: every length in it is checked against data_len.
// On true, scan_local_name holds name_len bytes (not NUL-terminated).
bool ble_parse_local_name(const uint8_t *data, uint8_t data_len, ble_scan_local_name_t *out);

#endif
