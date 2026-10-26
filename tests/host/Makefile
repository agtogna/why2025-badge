# Plain gcc, no ESP-IDF needed: make -C tests/host
# SAN="-fsanitize=address,undefined" if libasan is installed
SAN ?=
CFLAGS += -Wall -Wextra $(SAN) -g -I../../main/badge/common

test: test_ble_parse
	./test_ble_parse

test_ble_parse: test_ble_parse.c ../../main/badge/common/ble_parse.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f test_ble_parse

.PHONY: test clean
