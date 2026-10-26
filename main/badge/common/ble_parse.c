#include "ble_parse.h"

bool ble_parse_local_name(const uint8_t *data, uint8_t data_len, ble_scan_local_name_t *out)
{
    uint8_t curr_ptr = 0, curr_len, curr_type;
    while (curr_ptr < data_len) {
        curr_len = data[curr_ptr++];
        if (curr_len == 0 || curr_ptr >= data_len) {
            return false;
        }
        curr_type = data[curr_ptr++];
        /* The AD length comes from a stranger's advert: keep it inside the packet. */
        if (curr_ptr + curr_len - 1 > data_len) {
            return false;
        }

        /* Search for current data type and see if it contains name as data (0x08 or 0x09). */
        if (curr_type == 0x08 || curr_type == 0x09) {
            if (curr_len - 1 >= (int)sizeof(out->scan_local_name)) {
                return false;
            }
            for (uint8_t i = 0; i < curr_len - 1; i += 1) {
                out->scan_local_name[i] = data[curr_ptr + i];
            }
            out->name_len = curr_len - 1;
            return true;
        } else {
            /* Search for next data. Current length includes 1 octate for AD Type (2nd octate). */
            curr_ptr += curr_len - 1;
        }
    }
    return false;
}
