# Agent notes

- Target is ESP32-C3. Before building in a fresh copy or build dir, run `idf.py set-target esp32c3`. Otherwise `idf.py` defaults to `esp32` and the link fails (for example, undefined `usb_serial_jtag_is_connected`).
- A plain `idf.py flash` / esptool write can drop the USB link mid-image and leave the badge boot-looping. Recover with `tools/flash_chunked.sh <build_dir>` (64 KB chunks, retries each; re-enter download mode when it stalls). Use it for bricked badges only. Bootloader and partition table must already be written.
