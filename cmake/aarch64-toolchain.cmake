set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# SDK root may be overridden with -DALERTGATEWAY_SDK_ROOT=/path/to/sdk.
# Resolve from this file so invocation from another directory also works.
get_filename_component(_rk3588_parent "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(ALERTGATEWAY_SDK_ROOT
    "${_rk3588_parent}/01、linux5.10_sdk/atk_dlrk3588_linux5.10_sdk_release_v2.2.0_20260820/atk_dlrk3588_linux5.10"
    CACHE PATH "Extracted Rockchip Linux 5.10 SDK root")

# Verified against this SDK's manifest and the toolchain repository's bin tree.
set(_default_toolchain "${ALERTGATEWAY_SDK_ROOT}/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu")
get_filename_component(_local_toolchain "${CMAKE_CURRENT_LIST_DIR}/../third_party/atk-arm64/toolchain" ABSOLUTE)
if(NOT EXISTS "${_default_toolchain}/bin/aarch64-none-linux-gnu-g++" AND IS_DIRECTORY "${_local_toolchain}")
    set(_default_toolchain "${_local_toolchain}")
endif()
set(ALERTGATEWAY_TOOLCHAIN_ROOT
    "${_default_toolchain}"
    CACHE PATH "ARM64 cross-compiler installation")
set(ALERTGATEWAY_CROSS_PREFIX "aarch64-none-linux-gnu"
    CACHE STRING "Cross-compiler executable prefix")

set(CMAKE_C_COMPILER
    "${ALERTGATEWAY_TOOLCHAIN_ROOT}/bin/${ALERTGATEWAY_CROSS_PREFIX}-gcc")
set(CMAKE_CXX_COMPILER
    "${ALERTGATEWAY_TOOLCHAIN_ROOT}/bin/${ALERTGATEWAY_CROSS_PREFIX}-g++")

foreach(_compiler IN ITEMS "${CMAKE_C_COMPILER}" "${CMAKE_CXX_COMPILER}")
    if(NOT EXISTS "${_compiler}")
        message(FATAL_ERROR
            "Cross-compiler not found: ${_compiler}\n"
            "The SDK may not have been checked out. See README.md/repo.sh in "
            "${ALERTGATEWAY_SDK_ROOT}, or override ALERTGATEWAY_TOOLCHAIN_ROOT.")
    endif()
endforeach()

# With no board sysroot override GCC keeps its bundled libc sysroot.
# Board-specific headers and link libraries are selected in CMakeLists.txt.
set(ALERTGATEWAY_SYSROOT "${CMAKE_SYSROOT}" CACHE PATH "Optional complete board sysroot")
if(ALERTGATEWAY_SYSROOT)
    if(NOT IS_DIRECTORY "${ALERTGATEWAY_SYSROOT}/usr/include"
       OR NOT IS_DIRECTORY "${ALERTGATEWAY_SYSROOT}/usr/lib")
        message(FATAL_ERROR "Invalid target sysroot: ${ALERTGATEWAY_SYSROOT}")
    endif()
    set(CMAKE_SYSROOT "${ALERTGATEWAY_SYSROOT}")
    set(CMAKE_FIND_ROOT_PATH "${ALERTGATEWAY_SYSROOT}")
endif()

# Preserve overrides when CMake loads the toolchain in compiler checks.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
    ALERTGATEWAY_SDK_ROOT ALERTGATEWAY_TOOLCHAIN_ROOT
    ALERTGATEWAY_CROSS_PREFIX ALERTGATEWAY_SYSROOT)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
