#!/bin/bash
# Recovery flash for a badge that boot-loops or drops the USB link during a normal flash.
# Writes the app image in 64 KB chunks and retries each chunk until the chip answers,
# so re-enter download mode (hold left dial center, plug USB, tap RST) whenever it stalls.
# Usage: tools/flash_chunked.sh <build_dir> [start_chunk]
set -u
B=${1:?build dir}; START=${2:-0}; PORT=${PORT:-/dev/ttyACM0}
E=${ESPTOOL:-esptool.py}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
split -b 65536 -d -a 2 "$B/badge-firmware.bin" "$T/c_"
N=$(ls "$T" | wc -l); i=$START
for f in $(ls "$T" | tail -n +$((START+1))); do
  last=no_reset; [ $((i+1)) -eq "$N" ] && last=hard_reset
  until $E --chip esp32c3 -p "$PORT" -b 115200 --before no_reset --after $last write_flash \
        --flash_mode dio --flash_freq 80m --flash_size 4MB "$(printf '0x%x' $((0x10000 + i*65536)))" "$T/$f" >/dev/null 2>&1; do
    echo "chunk $((i+1))/$N failed, enter download mode"; sleep 3
  done
  i=$((i+1)); echo "ok $i/$N"
done
