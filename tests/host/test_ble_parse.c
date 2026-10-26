// Host-side tests for the BLE advert parser: make -C tests/host
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ble_parse.h"

int main(void)
{
    ble_scan_local_name_t n;

    // flags, then complete local name "[3] Bob"
    const uint8_t ok[] = {2, 0x01, 0x06, 8, 0x09, '[', '3', ']', ' ', 'B', 'o', 'b'};
    memset(&n, 0, sizeof(n));
    assert(ble_parse_local_name(ok, sizeof(ok), &n));
    assert(n.name_len == 7 && !memcmp(n.scan_local_name, "[3] Bob", 7));

    // no name structure
    const uint8_t none[] = {2, 0x01, 0x06};
    assert(!ble_parse_local_name(none, sizeof(none), &n));

    // length byte 0xFF in a 31-byte packet: used to copy 254 bytes into a 32-byte buffer
    uint8_t evil[31];
    memset(evil, 'A', sizeof(evil));
    evil[0] = 0xFF;
    evil[1] = 0x09;
    assert(!ble_parse_local_name(evil, sizeof(evil), &n));

    // AD structure that claims to run past the end of the packet
    const uint8_t overrun[] = {10, 0x09, 'a', 'b'};
    assert(!ble_parse_local_name(overrun, sizeof(overrun), &n));

    // name that fits the packet but not the 32-byte buffer
    uint8_t big[40];
    memset(big, 'B', sizeof(big));
    big[0] = 39;
    big[1] = 0x08;
    assert(!ble_parse_local_name(big, sizeof(big), &n));

    // length byte is the last byte of the packet
    const uint8_t tail[] = {2, 0x01, 0x06, 5};
    assert(!ble_parse_local_name(tail, sizeof(tail), &n));

    // empty packet
    assert(!ble_parse_local_name(ok, 0, &n));

    puts("ble_parse: all tests passed");
    return 0;
}
