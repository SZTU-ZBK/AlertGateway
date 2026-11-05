cmake_minimum_required(VERSION 3.16)
project(AlertGateway CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 生成 compile_commands.json，供 VSCode C/C++ 插件读取真实的 include 路径/宏定义，
# 避免 IntelliSense 因为不知道 src/ 在 include 路径里而把跨文件类型误判为不完整类型。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Third-party headers ───────────────────────────────────────────────────────
set(TP ${CMAKE_SOURCE_DIR}/third_party)
include_directories(
    ${TP}/rknn/include
    ${TP}/mpp/include
    ${TP}/rga/include
    ${TP}            # nlohmann/json.hpp lives at third_party/nlohmann/json.hpp
    ${TP}/paho/install-arm64/include
)

# ── Sysroot ───────────────────────────────────────────────────────────────────
# ── Sysroot ──────────────────────────────────────────────────
set(ALERTGATEWAY_SYSROOT "${CMAKE_SYSROOT}" CACHE PATH
    "Target board sysroot")

if(NOT ALERTGATEWAY_SYSROOT)
    message(FATAL_ERROR "Please configure with an ARM64 toolchain file")
endif()

set(SYSROOT "${ALERTGATEWAY_SYSROOT}")

# 优先使用 SDK 中与其动态库配套的 Rockchip 头文件
include_directories(BEFORE
    ${SYSROOT}/usr/include/rknn
    ${SYSROOT}/usr/include/rockchip
    ${SYSROOT}/usr/include/rga
)

# ── Libraries ────────────────────────────────────────────────
set(FFMPEG_LIB "${SYSROOT}/usr/lib")

set(MPP_LIB "${SYSROOT}/usr/lib/librockchip_mpp.so")
set(RGA_LIB "${SYSROOT}/usr/lib/librga.so")

set(RKNN_LIB_PATH "${SYSROOT}/usr/lib/librknnrt.so"
    CACHE FILEPATH "RKNN runtime library")

# Paho 需要另外准备 ARM64 版本
set(PAHO_C_LIB
    "${TP}/paho/install-arm64/lib/libpaho-mqtt3a.so")

set(PAHO_CPP_LIB
    "${TP}/paho/install-arm64/lib/libpaho-mqttpp3.so")

# ── Sources ───────────────────────────────────────────────────────────────────
set(SOURCES
    src/main.cpp
    src/app/ChannelPipeline.cpp
    src/capture/CaptureThread.cpp
    src/capture/PullStreamThread.cpp
    src/infer/RoiFilter.cpp
    src/infer/TilingTask.cpp
    src/infer/ThumbnailTask.cpp
    src/infer/NpuInferenceScheduler.cpp
    src/infer/RknnNpuExecutor.cpp
    src/infer/InferThread.cpp
    src/infer/RockchipYoloPostprocess.cpp
    src/infer/YoloPostprocess.cpp
    src/encode/EncodeThread.cpp
    src/stream/StreamThread.cpp
    src/mqtt/MqttThread.cpp
)

add_executable(AlertGateway ${SOURCES})

target_include_directories(AlertGateway PRIVATE ${CMAKE_SOURCE_DIR}/src)

# RKNN: use stub .so for cross-link; real librknnrt.so is deployed on the board.
if(IS_DIRECTORY "${RKNN_LIB_PATH}")
    set(RKNN_STUB "${RKNN_LIB_PATH}/librknnrt.so")
else()
    set(RKNN_STUB "${RKNN_LIB_PATH}")
endif()

if(NOT EXISTS "${RKNN_STUB}")
    message(FATAL_ERROR "RKNN stub not found: ${RKNN_STUB}")
endif()

target_link_libraries(AlertGateway PRIVATE
    ${RKNN_STUB}
    ${MPP_LIB}
    ${RGA_LIB}
    ${PAHO_C_LIB}
    ${PAHO_CPP_LIB}
    ${FFMPEG_LIB}/libavformat.so
    ${FFMPEG_LIB}/libavcodec.so
    ${FFMPEG_LIB}/libavutil.so
    pthread
)

# --allow-shlib-undefined: FFmpeg/Paho transitive deps (libssl, libva, libx264, etc.)
# are present on the board but not in the cross-compile sysroot.
target_link_options(AlertGateway PRIVATE -Wl,--allow-shlib-undefined)

# Standalone RKNN microbenchmark used for controlled model comparisons.
# Disabled by default so the production build remains unchanged.
option(BUILD_RKNN_BENCHMARK "Build the standalone RKNN benchmark" OFF)
if(BUILD_RKNN_BENCHMARK)
    add_executable(rknn_benchmark
        tools/benchmark/rknn_benchmark.cpp
        src/infer/RockchipYoloPostprocess.cpp
        src/infer/YoloPostprocess.cpp
    )
    target_include_directories(rknn_benchmark PRIVATE
        ${TP}/rknn/include
        ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(rknn_benchmark PRIVATE ${RKNN_STUB} pthread)
    target_compile_options(rknn_benchmark PRIVATE -g -O2)
endif()

# Camera + production InferThread smoke test without RTMP/MQTT dependencies.
option(BUILD_INFER_CAMERA_SMOKE "Build the camera inference smoke test" OFF)
if(BUILD_INFER_CAMERA_SMOKE)
    add_executable(infer_camera_smoke
        tools/benchmark/infer_camera_smoke.cpp
        src/capture/CaptureThread.cpp
        src/infer/RoiFilter.cpp
        src/infer/TilingTask.cpp
        src/infer/ThumbnailTask.cpp
        src/infer/NpuInferenceScheduler.cpp
        src/infer/InferThread.cpp
        src/infer/RockchipYoloPostprocess.cpp
        src/infer/YoloPostprocess.cpp
    )
    target_include_directories(infer_camera_smoke PRIVATE
        ${TP}/rknn/include
        ${TP}/rga/include
        ${TP}
        ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(infer_camera_smoke PRIVATE
        ${RKNN_STUB}
        ${RGA_LIB}
        pthread
    )
endif()

option(BUILD_IMAGE_PROCESSING_SMOKE "Build image processing smoke test" OFF)
if(BUILD_IMAGE_PROCESSING_SMOKE)
    add_executable(image_processing_smoke
        tools/benchmark/image_processing_smoke.cpp
        src/infer/RoiFilter.cpp
        src/infer/TilingTask.cpp
        src/infer/ThumbnailTask.cpp)
    target_include_directories(image_processing_smoke PRIVATE ${CMAKE_SOURCE_DIR}/src)
endif()

option(BUILD_NPU_SCHEDULER_SMOKE "Build the host-safe NPU scheduler algorithm smoke test" OFF)
if(BUILD_NPU_SCHEDULER_SMOKE)
    add_executable(npu_scheduler_smoke
        tools/benchmark/npu_scheduler_smoke.cpp
        src/infer/NpuInferenceScheduler.cpp)
    target_include_directories(npu_scheduler_smoke PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(npu_scheduler_smoke PRIVATE pthread)
endif()
