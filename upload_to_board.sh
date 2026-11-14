#!/usr/bin/env bash
# Upload the camera deployment package. Start it manually on the board.
set -euo pipefail

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT_DIR/build-atk-arm64}"
BOARD_USER="${BOARD_USER:-root}"
BOARD_IP="${BOARD_IP:-10.34.39.99}"
REMOTE_DIR="${REMOTE_DIR:-/tmp/alertgateway-1080p}"

# This path is embedded in a remote POSIX shell command.
if [[ ! "$REMOTE_DIR" =~ ^/tmp/[a-zA-Z0-9][a-zA-Z0-9._-]*$ ]]; then
    echo '错误：REMOTE_DIR 必须是 /tmp 下的单级目录，例如 /tmp/alertgateway-1080p。' >&2
    exit 1
fi

APP="$BUILD_DIR/AlertGateway"
PAHO_DIR="$PROJECT_DIR/third_party/paho/install-arm64/lib"
if [[ -f "$BUILD_DIR/deploy/AlertGateway" && ! "$APP" -nt "$BUILD_DIR/deploy/AlertGateway" ]]; then
    APP="$BUILD_DIR/deploy/AlertGateway"
    PAHO_DIR="$BUILD_DIR/deploy/lib"
elif [[ -f "$BUILD_DIR/lib/libpaho-mqtt3a.so.1" && -f "$BUILD_DIR/lib/libpaho-mqttpp3.so.1" ]]; then
    PAHO_DIR="$BUILD_DIR/lib"
fi
CONFIG="$PROJECT_DIR/config/config_srs_gui.json"
MODEL="$PROJECT_DIR/model/yolov8s_rockchip_dfl.rknn"
LAUNCHER="$PROJECT_DIR/start-board.sh"
LIBRARIES=(libpaho-mqtt3a.so.1 libpaho-mqttpp3.so.1)

for file in "$APP" "$CONFIG" "$MODEL" "$LAUNCHER" "${LIBRARIES[@]/#/$PAHO_DIR/}"; do
    if [[ ! -s "$file" ]]; then
        printf '错误：缺少部署文件 %s\n' "$file" >&2
        exit 1
    fi
done

stage="$(mktemp -d)"
trap 'rm -rf -- "$stage"' EXIT
mkdir -p "$stage/lib" "$stage/model"
install -m 755 -- "$APP" "$stage/AlertGateway"
install -m 755 -- "$LAUNCHER" "$stage/start.sh"
install -m 644 -- "$CONFIG" "$stage/config_srs_gui.json"
install -m 644 -- "$MODEL" "$stage/model/"
install -m 644 -- "${LIBRARIES[@]/#/$PAHO_DIR/}" "$stage/lib/"

# Fully receive the archive before replacing any deployed file.
printf '上传至 %s@%s:%s\n' "$BOARD_USER" "$BOARD_IP" "$REMOTE_DIR"
tar -C "$stage" -cf - . |
    ssh -T -- "$BOARD_USER@$BOARD_IP" "set -eu
        dest='$REMOTE_DIR'
        staging=\$(mktemp -d /tmp/alertgateway-upload.XXXXXX)
        trap 'rm -rf -- \"\$staging\"' EXIT
        tar -xf - -C \"\$staging\"
        mkdir -p \"\$dest/lib\" \"\$dest/model\"
        for file in lib/libpaho-mqtt3a.so.1 lib/libpaho-mqttpp3.so.1 model/yolov8s_rockchip_dfl.rknn config_srs_gui.json start.sh AlertGateway; do
            mv -f -- \"\$staging/\$file\" \"\$dest/\$file\"
        done"

printf '上传完成。SSH 登录板端后运行：cd %s && ./start.sh\n' "$REMOTE_DIR"
