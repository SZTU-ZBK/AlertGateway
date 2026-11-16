# ATK-DLRK3588 单路 H.264 硬解部署

已验收的 FFmpeg 4.4.1 库及配套头文件存放在项目的
`third_party/atk-arm64/ffmpeg-rkmpp`，沿用 `third_party` 的本地依赖管理方式，
二进制和头文件受 Git 忽略。默认的 `build-atk-arm64` 构建和
`./upload_to_board.sh` 会使用并打包这套库。完整 sysroot 或自定义板端依赖目录仍使用各自的 FFmpeg。
下文记录独立测试部署和 FFmpeg 的源码重建方式。

重新构建 FFmpeg 后，可从 `build-rkmpp-deps/ffmpeg` 更新项目内的依赖包。
同时复制 `libavcodec`、`libavformat`、`libavutil`、`libswresample`
的头文件目录和 `lib*.so*`，用 `cp -a` 保留 SONAME 符号链接。
这套库按项目验证过的 H.264/RTMP 用途裁剪。

候选包部署在板端 `/tmp/alertgateway-rkmpp`。启动前，在 Ubuntu 的 SRS 界面用局域网地址启动服务，再发布 **640×480@30** 的 H.264 输入到 `/live/testsrc2`。板端配置当前使用 `10.34.1.251`，主机地址变动时需要同步该配置的输入、输出和 MQTT 地址。

```sh
ssh root@10.34.39.99
cd /tmp/alertgateway-rkmpp
./start.sh
```

正常时拉流日志显示 `decoder=h264_rkmpp`。界面右侧订阅 `/live/alertgateway`。按 Ctrl+C 停止。旧部署位于 `/tmp/alertgateway-1080p`，两套部署共用同一个结果地址时应只启动一个。

## 重新构建

在 AlertGateway 根目录执行，使用与板端镜像匹配的 SDK、工具链和应用依赖。

```bash
./tools/build_rkmpp_ffmpeg.sh
cmake -S . -B build-atk-rkmpp \
  -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake \
  -DALERTGATEWAY_BOARD_DEPS="$PWD/build-rkmpp-deps/stage" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-atk-rkmpp -j6
```

FFmpeg 以 SDK 的 4.4.1 为基础。适配补丁在 MPP 队列满时保留待发送的包，先取输出再重试。正常取帧等待设为 1 ms。AlertGateway 按标准 `AVDRMFrameDescriptor` 读取 fd，RGA 将解码帧写入应用持有的 NV12 缓冲，叠框在应用缓冲上进行。

## 重新打包和上传

在板端程序停止后执行。将同一次构建的 FFmpeg SONAME 文件连同符号链接一起上传，并通过板端的 `ldd -r` 检查。

```bash
pkg=build-rkmpp-deps/package
mkdir -p "$pkg"/{lib,model}
cp build-atk-rkmpp/deploy/AlertGateway "$pkg/"
cp -a build-atk-rkmpp/deploy/lib/. "$pkg/lib/"
cp -a build-rkmpp-deps/ffmpeg/lib/{libavcodec,libavformat,libavutil,libswresample}.so.* "$pkg/lib/"
cp model/yolov8s_rockchip_dfl.rknn "$pkg/model/"
cp config/config_srs_gui.json "$pkg/"
install -m755 start-board.sh "$pkg/start.sh"
tar -C "$pkg" -cf - AlertGateway lib model config_srs_gui.json start.sh |
  ssh root@10.34.39.99 'mkdir -p /tmp/alertgateway-rkmpp && tar xf - -C /tmp/alertgateway-rkmpp'
```

板端系统继续提供 MPP、RGA、RKNN、libdrm 和基础运行库。此构建按项目运行所需组件裁剪，验收以 H.264/RTMP 为准。
