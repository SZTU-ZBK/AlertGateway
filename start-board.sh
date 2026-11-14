#!/bin/sh
set -eu

# 此脚本在 RK3588 上运行，默认使用脚本所在目录的部署文件。
app_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
program='./AlertGateway'
config='config_srs_gui.json'
lib_dir='./lib'

cd "$app_dir"
if [ ! -x "$program" ] || [ ! -r "$config" ]; then
    printf '请检查 %s 下的 %s 和 %s。\n' "$app_dir" "$program" "$config" >&2
    exit 1
fi
export LD_LIBRARY_PATH="$app_dir/${lib_dir#./}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$program" "$config"
