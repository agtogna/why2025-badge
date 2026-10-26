#!/bin/bash
# Serve repo root on localhost so the flasher can read build/ artifacts. Open http://localhost:8000/tools/flasher/
cd "$(dirname "$0")/../.." && exec python3 -m http.server 8000 --bind 127.0.0.1
