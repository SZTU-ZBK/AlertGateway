#!/usr/bin/env bash
# Upload only; does not start the application or change system libraries.
set -euo pipefail

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT_DIR/build-atk-arm64}"
BOARD_USER="${BOARD_USER:-root}"
REMOTE_DIR="/root/AlertGateway-test"

read -r -p '开发板 IPv4 地址: ' BOARD_IP
if [[ ! "$BOARD_IP" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]]; then
    echo '错误：请输入 IPv4 地址。' >&2
    exit 1
fi
IFS=. read -r -a OCTETS <<< "$BOARD_IP"
for OCTET in "${OCTETS[@]}"; do
    if (( 10#$OCTET > 255 )); then
        echo '错误：IP 地址每段必须在 0～255 之间。' >&2
        exit 1
    fi
done

# Also support builds produced before the deploy collection step was added.
APP="$BUILD_DIR/AlertGateway"
PAHO_DIR="$PROJECT_DIR/third_party/paho/install-arm64/lib"
if [[ -f "$BUILD_DIR/deploy/AlertGateway" &&
      -f "$BUILD_DIR/deploy/lib/libpaho-mqtt3a.so.1" &&
      -f "$BUILD_DIR/deploy/lib/libpaho-mqttpp3.so.1" &&
      ! "$APP" -nt "$BUILD_DIR/deploy/AlertGateway" ]]; then
    APP="$BUILD_DIR/deploy/AlertGateway"
    PAHO_DIR="$BUILD_DIR/deploy/lib"
fi
for FILE in "$APP" "$PAHO_DIR/libpaho-mqtt3a.so.1" "$PAHO_DIR/libpaho-mqttpp3.so.1"; do
    if [[ ! -f "$FILE" ]]; then
        echo "错误：缺少文件 $FILE" >&2
        exit 1
    fi
done

STAGE_DIR="$(mktemp -d)"
trap 'rm -rf -- "$STAGE_DIR"' EXIT
mkdir -p "$STAGE_DIR/lib"
cp -L -- "$APP" "$STAGE_DIR/AlertGateway"
cp -L -- "$PAHO_DIR/libpaho-mqtt3a.so.1" "$STAGE_DIR/lib/"
cp -L -- "$PAHO_DIR/libpaho-mqttpp3.so.1" "$STAGE_DIR/lib/"

cat > "$STAGE_DIR/run.sh" <<'RUN'
#!/bin/sh
set -eu
cd "$(dirname "$0")"
export LD_LIBRARY_PATH="$PWD/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec ./AlertGateway "${1:-config/config.json}"
RUN
chmod +x "$STAGE_DIR/AlertGateway" "$STAGE_DIR/run.sh"

echo "上传至 $BOARD_USER@$BOARD_IP:$REMOTE_DIR（保留板端已有配置和模型）"
tar -C "$STAGE_DIR" -cf - . | ssh "$BOARD_USER@$BOARD_IP" \
    "mkdir -p '$REMOTE_DIR' && tar -xf - -C '$REMOTE_DIR'"

echo '上传完成。配置和模型需单独准备；未启动程序。'
echo "板端运行：cd $REMOTE_DIR && ./run.sh"
