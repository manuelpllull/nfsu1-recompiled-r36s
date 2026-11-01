# CMake toolchain file for cross-compiling to Linux aarch64 (ArkOS / R36S / RK3326)
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Default to aarch64-linux-gnu cross compiler
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

# Target sysroot if specified via environment or CMake cache
if(DEFINED ENV{AARCH64_SYSROOT})
    set(CMAKE_SYSROOT "$ENV{AARCH64_SYSROOT}")
    set(CMAKE_FIND_ROOT_PATH "$ENV{AARCH64_SYSROOT}")
endif()

# Search settings
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Optimize specifically for Cortex-A35 (RK3326)
add_compile_options(-mcpu=cortex-a35 -mtune=cortex-a35)
