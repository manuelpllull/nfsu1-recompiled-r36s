cmake_minimum_required(VERSION 3.20)
project(NFSU2_R36S C CXX)

get_filename_component(NFS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(PCRECOMP "${NFS_ROOT}/../pcrecomp" CACHE PATH "pcrecomp directory")
set(DXVK_SOURCE "${NFS_ROOT}/build/dxvk-source" CACHE PATH "Prepared DXVK native source (for D3D9 headers)")
set(GEN_DIR "${NFS_ROOT}/src/recomp/gen" CACHE PATH "Generated lifted C directory")

set(NFS_R36S ON CACHE BOOL "Enable R36S handheld platform target" FORCE)

# Find dependencies
find_package(SDL2 REQUIRED)
find_package(Threads REQUIRED)
find_library(GLESV2_LIBRARY NAMES GLESv2 libGLESv2.so.2 libGLESv2.so REQUIRED)
find_library(EGL_LIBRARY NAMES EGL libEGL.so.1 libEGL.so REQUIRED)

# -----------------------------------------------------------------------------
# Direct3D 9 to OpenGL ES 2.0 / 3.0 Backend (MojoShader + GLES Native)
# -----------------------------------------------------------------------------
set(MOJOSHADER_DIR "${NFS_ROOT}/third_party/mojoshader")

add_library(d3d9_gles STATIC
    d3d9_gles/d3d9_gles.cpp
    "${MOJOSHADER_DIR}/mojoshader.c"
    "${MOJOSHADER_DIR}/mojoshader_common.c"
    "${MOJOSHADER_DIR}/profiles/mojoshader_profile_common.c"
    "${MOJOSHADER_DIR}/profiles/mojoshader_profile_glsl.c"
)

target_compile_features(d3d9_gles PRIVATE cxx_std_17)
target_compile_definitions(d3d9_gles PUBLIC
    SUPPORT_PROFILE_ARB1=0
    SUPPORT_PROFILE_ARB1_NV=0
    SUPPORT_PROFILE_HLSL=0
    SUPPORT_PROFILE_METAL=0
    SUPPORT_PROFILE_D3D=0
    SUPPORT_PROFILE_BYTECODE=0
    SUPPORT_PROFILE_SPIRV=0
    SUPPORT_PROFILE_GLSPIRV=0
    NFS_R36S
    NFS_GLES
    MOJOSHADER_DEPTH_CLIPPING
    MOJOSHADER_FLIP_RENDERTARGET
)

target_include_directories(d3d9_gles PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/d3d9_gles"
    "${MOJOSHADER_DIR}"
    "${DXVK_SOURCE}/include/native"
    "${DXVK_SOURCE}/include/native/windows"
    "${DXVK_SOURCE}/include/native/directx"
    ${SDL2_INCLUDE_DIRS}
)

target_link_libraries(d3d9_gles PUBLIC
    ${GLESV2_LIBRARY}
    ${EGL_LIBRARY}
    ${SDL2_LIBRARIES}
)

# -----------------------------------------------------------------------------
# Lifted Game Sources
# -----------------------------------------------------------------------------
file(GLOB GEN_SOURCES "${GEN_DIR}/recomp_*.c")
if(NOT GEN_SOURCES)
    message(WARNING "No lifted game C sources found in ${GEN_DIR}. Ensure you generate code first.")
    set(GEN_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/dummy_guest.c")
    file(WRITE "${GEN_SOURCES}" "void nfs_dummy_stub(void) {}\n")
endif()

# Split extra large lifted sources if needed
set(R36S_GEN_SOURCES)
find_package(Python3 COMPONENTS Interpreter)
foreach(source IN LISTS GEN_SOURCES)
    file(SIZE "${source}" source_bytes)
    if(source_bytes GREATER 67108864 AND Python3_FOUND)
        execute_process(
            COMMAND "${Python3_EXECUTABLE}" "${NFS_ROOT}/scripts/split_android_sources.py"
            "${source}" "${CMAKE_CURRENT_BINARY_DIR}/split_guest"
            RESULT_VARIABLE split_status OUTPUT_VARIABLE split_sources OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(split_status EQUAL 0)
            string(REPLACE "\n" ";" split_sources "${split_sources}")
            list(APPEND R36S_GEN_SOURCES ${split_sources})
        else()
            list(APPEND R36S_GEN_SOURCES "${source}")
        endif()
    else()
        list(APPEND R36S_GEN_SOURCES "${source}")
    endif()
endforeach()

# Compile lifted game code into an OBJECT library
add_library(nfs_guest OBJECT ${R36S_GEN_SOURCES})
target_include_directories(nfs_guest PRIVATE
    "${PCRECOMP}/runtime/recomp32"
    "${GEN_DIR}"
)
target_compile_definitions(nfs_guest PRIVATE
    RECOMP_STRICT_ICALL
    RECOMP_LOCAL_REGS
    RECOMP_X87_WINDOW
    "RECOMP_MEM_QUAL="
)
target_compile_options(nfs_guest PRIVATE
    -O2 -w -fwrapv -fno-strict-aliasing -fexceptions -ffp-contract=off
    -include "${NFS_ROOT}/android/app/src/main/cpp/arm64_guest.h"
    -g0 -fvisibility=hidden
)

# Optional ThinLTO for Cortex-A35 performance
option(NFS_R36S_LTO "Enable ThinLTO optimization for Cortex-A35" OFF)
if(NFS_R36S_LTO)
    target_compile_options(nfs_guest PRIVATE -flto=thin)
endif()

# -----------------------------------------------------------------------------
# Main Executable for R36S (Linux / Mali-G31 / ArkOS)
# -----------------------------------------------------------------------------
add_executable(nfsu2.aarch64
    main.cpp
    "${NFS_ROOT}/android/app/src/main/cpp/guest_runtime.cpp"
    "${NFS_ROOT}/android/app/src/main/cpp/d3d9_bridge.cpp"
    "${NFS_ROOT}/src/runtime/overrides.c"
    "${NFS_ROOT}/src/runtime/widescreen.c"
    $<TARGET_OBJECTS:nfs_guest>
)

target_compile_features(nfsu2.aarch64 PRIVATE cxx_std_17)

target_compile_definitions(nfsu2.aarch64 PRIVATE
    NFS_R36S
    NFS_D3D9_BACKEND
    NFS_SDL_AUDIO
    RECOMP_STRICT_ICALL
    RECOMP_X87_WINDOW
)

target_include_directories(nfsu2.aarch64 PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/d3d9_gles"
    "${NFS_ROOT}/android/app/src/main/cpp"
    "${NFS_ROOT}/src/runtime"
    "${NFS_ROOT}/src/platform"
    "${PCRECOMP}/runtime/recomp32"
    "${PCRECOMP}/runtime/win32hle"
    "${SDL2_INCLUDE_DIRS}"
)

target_include_directories(nfsu2.aarch64 SYSTEM PRIVATE
    "${DXVK_SOURCE}/include/native"
    "${DXVK_SOURCE}/include/native/windows"
    "${DXVK_SOURCE}/include/native/directx"
)

target_compile_options(nfsu2.aarch64 PRIVATE
    -O2 -fno-strict-aliasing -ffp-contract=off
    -Wall -Wextra -Wno-missing-field-initializers
)

target_link_libraries(nfsu2.aarch64 PRIVATE
    d3d9_gles
    ${SDL2_LIBRARIES}
    ${GLESV2_LIBRARY}
    ${EGL_LIBRARY}
    Threads::Threads
    dl
    m
)

if(NFS_R36S_LTO)
    target_link_options(nfsu2.aarch64 PRIVATE -flto=thin)
endif()

# Staging is explicit so an unverified development build cannot replace the
# last known PortMaster package binary.
option(NFS_R36S_STAGE_PACKAGE "Copy the built binary into the PortMaster package" OFF)
if(NFS_R36S_STAGE_PACKAGE)
    add_custom_command(TARGET nfsu2.aarch64 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:nfsu2.aarch64>
        "${CMAKE_CURRENT_SOURCE_DIR}/package/nfsu2.aarch64"
        COMMENT "Copying binary to PortMaster package directory"
    )
endif()
