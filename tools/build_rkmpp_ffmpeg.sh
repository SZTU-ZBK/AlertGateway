#!/usr/bin/env bash
# Build the isolated FFmpeg variant for the verified ATK Buildroot board.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
sdk="${SDK_ROOT:-$root/../01、linux5.10_sdk/atk_dlrk3588_linux5.10_sdk_release_v2.2.0_20260820/atk_dlrk3588_linux5.10}"
board="${BOARD_HOST:-root@10.34.39.99}"
cross="${CROSS_PREFIX:-$root/third_party/atk-arm64/toolchain/bin/aarch64-none-linux-gnu-}"
build="${RKMPP_BUILD_DIR:-$root/build-rkmpp-deps}"
mkdir -p "$build"/{src,stage/lib,stage/include/rockchip,pc}
build="$(cd -- "$build" && pwd)"
tar -xf "$sdk/buildroot/dl/ffmpeg/ffmpeg-4.4.1.tar.xz" -C "$build/src"
tar -xf "$sdk/buildroot/dl/libdrm/libdrm-2.4.120.tar.xz" -C "$build/src"
patch -d "$build/src/ffmpeg-4.4.1" -p1 < "$root/tools/ffmpeg/4.4.1-rkmpp-backpressure.patch"
cp "$root"/third_party/mpp/include/*.h "$build/stage/include/rockchip/"
for lib in librknnrt librga librockchip_mpp; do
    cp -L "$root/third_party/atk-arm64/lib/$lib.so" "$build/stage/lib/"
done
ssh -o BatchMode=yes -o StrictHostKeyChecking=yes "$board" 'cat /usr/lib/libdrm.so.2' > "$build/stage/lib/libdrm.so.2"
ln -sfn libdrm.so.2 "$build/stage/lib/libdrm.so"

drm="$build/src/libdrm-2.4.120"
cat > "$build/pc/libdrm.pc" <<PC
Name: libdrm
Description: Target libdrm
Version: 2.4.120
Libs: -L"$build/stage/lib" -ldrm
Cflags: -I"$drm" -I"$drm/include/drm"
PC
# The SDK MPP pkg-config template for the board's 2af257e revision specifies 1.3.9.
cat > "$build/pc/rockchip_mpp.pc" <<PC
Name: rockchip_mpp
Description: Target Rockchip MPP
Version: 1.3.9
Libs: -L"$build/stage/lib" -lrockchip_mpp
Cflags: -I"$build/stage/include"
PC

cd "$build/src/ffmpeg-4.4.1"
# The board's libdrm resolves its libc imports on the target. Check them there with ldd -r.
PKG_CONFIG_LIBDIR="$build/pc" ./configure --pkg-config="$(command -v pkg-config)" \
    --prefix="$build/ffmpeg" --extra-ldflags=-Wl,--allow-shlib-undefined \
    --enable-cross-compile --cross-prefix="$cross" --arch=aarch64 --target-os=linux \
    --enable-shared --disable-static --enable-libdrm --enable-rkmpp --enable-version3 \
    --disable-autodetect --disable-doc --disable-ffplay --disable-avdevice \
    --disable-everything --enable-network --enable-demuxer=h264,hevc,flv,rtsp \
    --enable-muxer=flv,rawvideo,null --enable-protocol=file,tcp,udp,rtmp \
    --enable-decoder=h264_rkmpp,hevc_rkmpp --enable-parser=h264,hevc \
    --enable-encoder=wrapped_avframe,rawvideo --enable-filter=null,anull,buffer,buffersink \
    --enable-ffmpeg --enable-ffprobe
make -j"${JOBS:-8}"
make install
cp -a "$build"/ffmpeg/lib/*.so* "$build/stage/lib/"
cp -a "$build/ffmpeg/include/." "$build/stage/include/"
