# Agent notes

- Target is ESP32-C3, forced by `set(IDF_TARGET esp32c3)` in `CMakeLists.txt`; no `idf.py set-target` needed in a fresh copy or build dir.
- A plain `idf.py flash` / esptool write can drop the USB link mid-image and leave the badge boot-looping. Recover with `tools/flash_chunked.sh <build_dir>` (64 KB chunks, retries each; re-enter download mode when it stalls). Use it for bricked badges only. Bootloader and partition table must already be written.
