#!/bin/bash
# ==============================================================================
# Fast Build Script for NFSU2 R36S Port (Linux ARM64 / PortMaster)
# Recompiles host-side translation runtime & links with precompiled guest objects.
# ==============================================================================
set -e

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build/ci-r36s}"
OUT="${OUT:-$ROOT/ports/r36s/package}"
GUEST_DIR="${GUEST_DIR:-$BUILD_DIR/guest-objs}"
COMPILE_ONLY="${COMPILE_ONLY:-0}"

CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
CC="${CC:-${CROSS_COMPILE}gcc}"
CXX="${CXX:-${CROSS_COMPILE}g++}"
STRIP="${STRIP:-${CROSS_COMPILE}strip}"
OBJDUMP="${OBJDUMP:-${CROSS_COMPILE}objdump}"

echo "============================================================"
echo " NFSU2 R36S Fast CI/CD Build (ARM64)                        "
echo " Compiler     : $CC / $CXX"
echo " Build Dir    : $BUILD_DIR"
echo " Output       : $OUT/nfsu2.aarch64"
echo " Guest Dir    : $GUEST_DIR"
echo " Compile Only : $COMPILE_ONLY"
echo "============================================================"

mkdir -p "$BUILD_DIR/obj" "$OUT"

# 1. SDK Include paths (self-contained within repository)
SDK="$ROOT/ports/r36s/sdk/include"
SDK_INC="-I$SDK -I$SDK/SDL2"
DXVK_INC="-I$SDK/dxvk -I$SDK/dxvk/native -I$SDK/dxvk/native/windows -I$SDK/dxvk/native/directx"
RECOMP_INC="-I$SDK/recomp32 -I$SDK/win32hle"

# 2. Verify guest objects (only if linking)
if [ "$COMPILE_ONLY" != "1" ]; then
    if [ ! -d "$GUEST_DIR" ] || [ -z "$(find "$GUEST_DIR" -name '*.o' 2>/dev/null | head -1)" ]; then
        echo "[ERROR] No compiled guest objects (*.o) found in $GUEST_DIR!"
        echo "Provide precompiled guest objects or extract r36s-guest-objs.tar.gz into $GUEST_DIR."
        exit 1
    fi
    GUEST_OBJ_COUNT="$(find "$GUEST_DIR" -name '*.o' | wc -l | tr -d ' ')"
    echo "[INFO] Found $GUEST_OBJ_COUNT precompiled guest objects in $GUEST_DIR."
fi

# 3. Compile flags
MOJO="$ROOT/third_party/mojoshader"
MOJODEF="-DSUPPORT_PROFILE_ARB1=0 -DSUPPORT_PROFILE_ARB1_NV=0 -DSUPPORT_PROFILE_HLSL=0 -DSUPPORT_PROFILE_METAL=0 -DSUPPORT_PROFILE_D3D=0 -DSUPPORT_PROFILE_BYTECODE=0 -DSUPPORT_PROFILE_SPIRV=0 -DSUPPORT_PROFILE_GLSPIRV=0 -DMOJOSHADER_DEPTH_CLIPPING -DMOJOSHADER_FLIP_RENDERTARGET"
GLESINC="-I$ROOT/ports/r36s/d3d9_gles -I$MOJO $DXVK_INC $SDK_INC"
APPINC="-I$ROOT/ports/r36s -I$ROOT/ports/r36s/d3d9_gles -I$ROOT/android/app/src/main/cpp -I$ROOT/src/runtime -I$ROOT/src/platform $RECOMP_INC $SDK_INC -isystem $SDK/dxvk -isystem $SDK/dxvk/native -isystem $SDK/dxvk/native/windows -isystem $SDK/dxvk/native/directx"
APPDEF="-DRECOMP_TLS=__thread -DRECOMP_TLS_STRUCT -DNFS_PARALLEL_GUEST -DNFS_FIXED_GUEST_BASE=0x1000000000ULL -DNFS_R36S -DNFS_GLES -DNFS_D3D9_BACKEND -DNFS_SDL_AUDIO -DRECOMP_STRICT_ICALL -DRECOMP_X87_WINDOW $MOJODEF"
OPT="-O2 -g0 -DNDEBUG -fno-strict-aliasing -ffp-contract=off"

# Target RK3326 Cortex-A35 if aarch64 cross-compiler supports it
if "$CC" -mcpu=cortex-a35 -E - < /dev/null >/dev/null 2>&1; then
    OPT="$OPT -mcpu=cortex-a35 -mtune=cortex-a35"
fi

compile() {
    local src="$1" obj="$2"; shift 2
    local name="$(basename "$src")"
    echo "  CC $name"
    local err_file="$BUILD_DIR/obj/${name}.err"
    if ! "$@" -c "$src" -o "$obj" 2> "$err_file"; then
        echo "[ERROR] Compilation failed for $name:" >&2
        cat "$err_file" >&2
        if [ -n "$GITHUB_STEP_SUMMARY" ]; then
            echo "### ❌ Compilation Failure: \`$name\`" >> "$GITHUB_STEP_SUMMARY"
            echo '```' >> "$GITHUB_STEP_SUMMARY"
            cat "$err_file" >> "$GITHUB_STEP_SUMMARY"
            echo '```' >> "$GITHUB_STEP_SUMMARY"
        fi
        return 1
    fi
}

# 4. Compilation of runtime and translation layer
compile "$ROOT/ports/r36s/d3d9_gles/d3d9_gles.cpp" "$BUILD_DIR/obj/d3d9_gles.o" "$CXX" -std=c++17 $OPT $MOJODEF -DNFS_R36S -DNFS_GLES $GLESINC
for c in mojoshader.c mojoshader_common.c profiles/mojoshader_profile_common.c profiles/mojoshader_profile_glsl.c; do
    compile "$MOJO/$c" "$BUILD_DIR/obj/$(basename "$c" .c).o" "$CC" $OPT $MOJODEF -DNFS_R36S -DNFS_GLES -I"$MOJO"
done
compile "$ROOT/ports/r36s/main.cpp" "$BUILD_DIR/obj/main.o" "$CXX" -std=c++17 $OPT $APPDEF $APPINC
compile "$ROOT/android/app/src/main/cpp/guest_runtime.cpp" "$BUILD_DIR/obj/guest_runtime.o" "$CXX" -std=c++17 $OPT $APPDEF $APPINC
compile "$ROOT/android/app/src/main/cpp/d3d9_bridge.cpp" "$BUILD_DIR/obj/d3d9_bridge.o" "$CXX" -std=c++17 $OPT $APPDEF $APPINC
compile "$ROOT/src/runtime/overrides.c" "$BUILD_DIR/obj/overrides.o" "$CC" $OPT $APPDEF $APPINC
compile "$ROOT/src/runtime/fx_cache.c" "$BUILD_DIR/obj/fx_cache.o" "$CC" $OPT $APPDEF $APPINC
compile "$ROOT/src/runtime/hud_half.c" "$BUILD_DIR/obj/hud_half.o" "$CC" $OPT $APPDEF $APPINC
compile "$ROOT/src/runtime/world_cull.c" "$BUILD_DIR/obj/world_cull.o" "$CC" $OPT $APPDEF $APPINC
compile "$ROOT/src/runtime/native_math.c" "$BUILD_DIR/obj/native_math.o" "$CC" $OPT $APPDEF $APPINC
compile "$ROOT/src/runtime/widescreen.c" "$BUILD_DIR/obj/widescreen.o" "$CC" $OPT $APPDEF $APPINC

if [ "$COMPILE_ONLY" = "1" ]; then
    echo "============================================================"
    echo " COMPILE_ONLY: All runtime objects compiled successfully!   "
    echo "============================================================"
    exit 0
fi

# 5. Generate GLESv2 and SDL2 stubs for clean dynamic linking on RK3326 ArkOS
STUB="$BUILD_DIR/stubs"
mkdir -p "$STUB"

# GLES2 stub with soname libGLESv2.so.2
nm -u "$BUILD_DIR"/obj/*.o | awk '{print $2}' | grep -E '^gl[A-Z]' | sort -u | sed 's/.*/void &(void){}/' > "$STUB/stub_gles.c"
"$CC" -shared -fPIC -o "$STUB/libGLESv2.so" -Wl,-soname,libGLESv2.so.2 "$STUB/stub_gles.c"

# SDL2 stub with soname libSDL2-2.0.so.0
nm -u "$BUILD_DIR"/obj/*.o | awk '{print $2}' | grep -E '^SDL_' | sort -u | sed 's/.*/void &(void){}/' > "$STUB/stub_sdl.c"
"$CC" -shared -fPIC -o "$STUB/libSDL2.so" -Wl,-soname,libSDL2-2.0.so.0 "$STUB/stub_sdl.c"

# 6. Link final binary
echo "  LD $OUT/nfsu2.aarch64"
"$CXX" -o "$OUT/nfsu2.aarch64" \
    "$BUILD_DIR"/obj/*.o \
    "$GUEST_DIR"/*.o \
    -L"$STUB" \
    -lSDL2 -lGLESv2 -lpthread -ldl -lm -rdynamic -no-pie

# 7. Strip and verify
"$STRIP" --strip-debug "$OUT/nfsu2.aarch64"
chmod 755 "$OUT/nfsu2.aarch64"

if "$OBJDUMP" -p "$OUT/nfsu2.aarch64" | grep -q 'NEEDED.*libGL\.so'; then
    echo "[ERROR] Binary erroneously links desktop libGL.so instead of libGLESv2.so.2"
    exit 1
fi

echo "============================================================"
echo " Build Succeeded: $OUT/nfsu2.aarch64"
echo " File Size: $(ls -lh "$OUT/nfsu2.aarch64" | awk '{print $5}')"
echo " Dynamic Dependencies:"
"$OBJDUMP" -p "$OUT/nfsu2.aarch64" | grep NEEDED || true
echo "============================================================"
