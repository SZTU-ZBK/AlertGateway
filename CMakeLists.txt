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

# Board-specific dependencies can come from a complete SDK sysroot or a local bundle.
set(ALERTGATEWAY_BOARD_DEPS "${TP}/atk-arm64" CACHE PATH "ARM64 board headers and link libraries")
if(ALERTGATEWAY_SYSROOT)
    set(BOARD_DEPS "${ALERTGATEWAY_SYSROOT}/usr")
else()
    set(BOARD_DEPS "${ALERTGATEWAY_BOARD_DEPS}")
endif()
include_directories(BEFORE
    ${BOARD_DEPS}/include/rknn
    ${BOARD_DEPS}/include/rockchip
    ${BOARD_DEPS}/include/rga
    ${BOARD_DEPS}/include
)

set(FFMPEG_LIB "${BOARD_DEPS}/lib")
set(MPP_LIB "${BOARD_DEPS}/lib/librockchip_mpp.so")
set(RGA_LIB "${BOARD_DEPS}/lib/librga.so")
set(RKNN_LIB_PATH "${BOARD_DEPS}/lib/librknnrt.so" CACHE FILEPATH "RKNN runtime library")

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
set_target_properties(AlertGateway PROPERTIES SKIP_BUILD_RPATH TRUE)

target_include_directories(AlertGateway PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(NOT EXISTS "${RKNN_LIB_PATH}")
    message(FATAL_ERROR "RKNN link library not found: ${RKNN_LIB_PATH}")
endif()

target_link_libraries(AlertGateway PRIVATE
    ${RKNN_LIB_PATH}
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

# Collect the application and locally built Paho runtime libraries for upload.
# Rockchip/FFmpeg libraries are supplied by the matching board image.
add_custom_command(TARGET AlertGateway POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/deploy/lib"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:AlertGateway>" "${CMAKE_BINARY_DIR}/deploy/AlertGateway"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PAHO_C_LIB}" "${CMAKE_BINARY_DIR}/deploy/lib/libpaho-mqtt3a.so.1"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PAHO_CPP_LIB}" "${CMAKE_BINARY_DIR}/deploy/lib/libpaho-mqttpp3.so.1"
    VERBATIM
)

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
    target_link_libraries(rknn_benchmark PRIVATE ${RKNN_LIB_PATH} pthread)
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
        ${RKNN_LIB_PATH}
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
