#!/usr/bin/env bash
set -euo pipefail

# Run on the Ubuntu host after starting the SRS service.
board="${BOARD_HOST:-root@10.34.39.99}"
rtmp_port=1935
mqtt_port=18830

exec ssh -o ExitOnForwardFailure=yes \
    -o ServerAliveInterval=15 -o ServerAliveCountMax=3 \
    -R "127.0.0.1:$rtmp_port:127.0.0.1:$rtmp_port" \
    -R "127.0.0.1:$mqtt_port:127.0.0.1:$mqtt_port" \
    -- "$board"
