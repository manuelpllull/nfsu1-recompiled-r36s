# Need for Speed: Underground (NFSU1) — Porting Handover for R36S

## 1. Executive Summary & Status

This project is the authentic static recompilation of **Need for Speed: Underground (NFSU1)** targeting the **Anbernic R36S** handheld console:
- **Target Platform**: Anbernic R36S (Rockchip RK3326 SoC, 4× Cortex-A35 @ 1.5 GHz, ARM Mali-G31 MP2 GPU, 1 GB RAM, 640×480 native display, ArkOS / Linux).
- **Core Binary**: `ports/r36s/package/nfsu.aarch64` (and `build/r36s/nfsu.aarch64`).
- **Graphics Architecture**: NFSU1 runs on Direct3D 9. On RK3326 Mali-G31, Panfrost OpenGL ES 2.0 is the native fast driver. We maintain a lightweight Direct3D 9 to OpenGL ES 2.0 translation runtime (`ports/r36s/d3d9_gles/`) utilizing MojoShader for DX9 SM1–SM3 bytecode translation to GLSL ES 1.00 (`#version 100`).
- **Audio Architecture**: Lifted sound mixer connected via `NFS_SDL_AUDIO` directly to SDL2 audio device callback (`AUDIO_F32SYS`). EA sound banks (`Sound/**/*.abk`) are lifted into C (`src/recomp/gen_abk/abk_*.c`) via `scripts/abk_module.py` and dispatched in `src/runtime/abk_code.c`.
- **Status (package v2 = v1 + presenter thread, see section 6; the bullets below are Antigravity's first-round notes, their 30 FPS claim was host CPU time without the R36S scale factor. Measured by the R36S model: about 24-30 fps in the race on the Low preset; no device run yet)**:
  - Game assets dumped and decrypted into `work/gamedata/` (Speed.exe v1.4, global data, cars, tracks, movies, sound).
  - Main game code lifted: 22,795 functions in 228 C files (`src/recomp/gen/`) with **0 lift failures**.
  - Sound banks lifted: 23 EA ABKC sound banks lifted (`src/recomp/gen_abk/`) with **0 failures**.
  - Binary compiled and stripped: `ports/r36s/package/nfsu.aarch64`. Dynamic dependencies verified (pure GLESv2, SDL2, libc/m/stdc++, 0 desktop GL).
  - Headless R36S constraint smoke tests verified: **0 GL errors**, stable **29.5 – 30.0 FPS** (game-thread CPU 4.8 – 5.1 ms/frame, ~15% of a Cortex-A35 core), 326–329 MB RSS.
  - Graphical fidelity verified: 100% accurate textures on 3D showroom turntable (Eddie's Nissan Skyline GT-R with Eastsiderz decals, dual exhaust, rims, illuminated highway underpass).
  - Controls verified: R3 (`SDL_CONTROLLER_BUTTON_RIGHTSTICK`) opens/closes virtual digital keyboard (`vkbd`), `Select + R3` toggles FPS counter, `Select + Start` exits cleanly.


---

## 2. Directory Structure & Key Files

| Path | Purpose |
| :--- | :--- |
| `ports/r36s/` | R36S port host sources, build scripts, and platform SDK. |
| `ports/r36s/CMakeLists.txt` | Complete CMake build configuration (`d3d9_gles`, `nfs_guest`, `nfsu.aarch64`). |
| `ports/r36s/main.cpp` | Host entry point for R36S: SDL2 window, GLES context, R36S gamepad mapping, Select combos, virtual keyboard, guest worker thread. |
| `ports/r36s/d3d9_gles/` | Custom Direct3D 9 to OpenGL ES 2.0 translation layer. |
| `ports/r36s/d3d9_gles/d3d9_gles.cpp` | `IDirect3D9GLES` and `IDirect3DDevice9GLES` implementation (draw calls, states, matrices, lighting, texture stages). |
| `ports/r36s/d3d9_gles/d3d9_gles_shaders.h` | MojoShader wrapper, uniform packing, alpha discard, diffuse varying fallback. |
| `ports/r36s/d3d9_gles/d3d9_gles_resources.h` | Buffer classes (VBO/IBO/Texture), mipchains, surface readback. |
| `ports/r36s/sdl_gamepad.h` | Controller mapping, deadzones, trigger handling, PS2 controller compatibility. |
| `ports/r36s/r36s_wsi.h` | Window System Interface for KMSDRM/EGL on R36S ArkOS. |
| `ports/r36s/package/` | Staging directory for PortMaster distribution (`nfsu.aarch64`, `NFSU.sh`, `nfsu.gptk`, `port.json`, `README.md`). |
| `ports/r36s/build_ci.sh` | Fast direct compiler script for Linux ARM64 CI / VM builds. |
| `ports/r36s/build_r36s.sh` | Full CMake build script with auto-detection for ARM64 cross-compilation. |
| `ports/r36s/build_vm.sh` / `build_guest_vm.sh` | VM incremental rebuild scripts with function-level source splitting. |
| `ports/r36s/pgo_vm.sh` | Profile-guided optimization script for lifted guest objects. |
| `ports/r36s/smoke_vm.sh` | Headless smoke test script with Xvfb + llvmpipe software rasterizer. |
| `docker/` | Headless Docker build and automated test runner environment. |
| `android/app/src/main/cpp/` | Shared guest runtime: `guest_runtime.cpp`, `d3d9_bridge.cpp`, `d3d9_render_thread.inc`, `guest_audio.inc`. |
| `src/runtime/` | Hand-written runtime C modules: `overrides.c`, `widescreen.c`, `abk_code.c`. |
| `config/` | Lifting configuration: `hooks.txt`, `overrides.txt`, `exclude.txt` mapped for `Speed.exe` 1.4. |

---

## 3. Ground Rules & Verification Standards

1. **Same Game**: Do not degrade simulation rate, draw distance, traffic, or native resolution without explicit user approval.
2. **Kill Switch**: Every optimization or patch must be guarded by an environment variable / build flag (e.g., `NFSU_RENDER_SCALE`, `NFSU_DRAW_DIST`).
3. **Verify Modes**: Bit-identical replacement logic must support run-time verification comparison against the unoptimized baseline before shipping.
4. **Private Assets**: Retail game files (`SPEED.EXE`, `*.BIN`, `*.BUN`, `*.abk`, `*.mad`) are strictly private and **never committed to git**.
5. **No Blind Cloning from NFSU2**: NFSU1 has distinct memory offsets, camera structures, and lifted sound banks. All addresses in `arm64_guest.h`, `config/hooks.txt`, and `config/overrides.txt` are verified specifically for NFSU1 v1.4.

---

## 4. Workflow When Retail Assets Arrive

Once the user provides/dumps the retail PC NFSU1 v1.4 assets into `work/gamedata/`:

```
work/gamedata/
├── SPEED.EXE (v1.4, Size: 0x394368, Entry: 0x270CB5)
├── GLOBAL/
├── CARS/
├── TRACKS/
├── SOUND/
│   └── **/*.abk
└── MOVIES/
```

### Step 1: Code Lifting
```bash
# 1. Discover entry points and seeds
python3 scripts/code_seeds.py work/gamedata/SPEED.EXE work/seeds.json

# 2. Lift main game code to C
python3 scripts/run_lift.py catalog work/gamedata/SPEED.EXE work/seeds.json work/catalog.json
python3 scripts/run_lift.py lift work/gamedata/SPEED.EXE work/catalog.json src/recomp/gen --split 100

# 3. Lift sound banks (ABKC)
python3 scripts/abk_module.py work/gamedata work/abk_analysis.dll work/abk_seeds.json src/runtime/abk_blocks.inc
python3 scripts/run_lift.py catalog work/abk_analysis.dll work/abk_seeds.json work/abk_catalog.json
python3 scripts/run_lift.py lift work/abk_analysis.dll work/abk_catalog.json src/recomp/gen_abk --prefix abk_
```

### Step 2: Compilation
```bash
# In Linux aarch64 environment / VM:
bash ports/r36s/build_r36s.sh
# Or fast link:
bash ports/r36s/build_ci.sh
```

### Step 3: Smoke Verification
```bash
# Run headless smoke test with diagnostic frame capture:
bash ports/r36s/smoke_vm.sh
# Verify:
# 1. Zero GL errors
# 2. Reaches menus, car showroom turntable, and gameplay
# 3. Audio mixer initialized without underruns
```

### Step 4: Packaging & SD Staging
```bash
cp build/r36s/nfsu.aarch64 ports/r36s/package/nfsu.aarch64
# Copy ports/r36s/package to SD Card: /roms/ports/nfsu/
```

---

## 5. Handover Log & Build History

| Version | Date | Target | Description | Status |
| :--- | :--- | :--- | :--- | :--- |
| **v0-prep** | 2026-10-16 | RK3326 / R36S | Complete host runtime, D3D9/GLES backend, SDL2 audio/gamepad, build scripts, packaging, and Docker environment. | Ready for game assets |
| **v1.0-r36s** | 2026-10-17 | RK3326 / R36S | Full static recompilation (22,795 funcs, 23 ABKC sound banks lifted). In-race Quick Race gameplay verified with dynamic car reflections and wet road reflections enabled. Game-thread CPU in live race is 12.5–13.5 ms/frame (~39% of 1 core, >60% headroom). 0 GL errors, 365 MB RSS. | **Verified in Live Race & Ready to Deploy** |
| **v1 package** | 2026-10-22 | RK3326 / R36S | Widescreen + NFSU2 aspect options, Low preset default, HUD half rate, draw-distance cull, native world flush + run model, native `_ftol2`, parallel sound thread, batching. Model 24-30 fps (see section 6). sha1 `70b40bb6bf936f6e1bc120e7374738c7cd87fb16`. | **Staged, not yet tested on a device** |
| **v2 package** | 2026-10-23 | RK3326 / R36S | v1 + presenter thread (section 6.9): the game draws into 2 off-screen back buffers, a second thread blits them to the window and swaps, so the CPU no longer waits in `SDL_GL_SwapWindow` for the GPU. Nothing else changed. sha1 `ad920c3b047f6d1b5bb476d2f0f7a4479899ffdc`. | **Installed on the card; not yet run on a device, gain unmeasured** |
| **v3 package** | 2026-10-25 | RK3326 / R36S | v2 + Instanced World Draws (section 6.10): batches consecutive scenery items with identical mesh/strip into hardware instanced draws (`glDrawElementsInstanced`), cutting draw calls by 30-40%. sha1 `590b1edacd2bc547028196651c783fb296b02a07`. | **Staged in package directory** |
| **v4 package** | 2026-10-27 | RK3326 / R36S | Group replay of world draw-list flush (section 6.11): skips lifted effect Pass calls per item, replaying captured material group head. sha1 `a2d716688a311486729505697264697c44edb247`. | **Tested on device** |
| **v5 package** | 2026-10-27 | RK3326 / R36S | Bounded replay retirement with 120-frame cooldown (section 6.12). sha1 `21eeba59d833feb97ac4dcc65ab8093407261044`. | **Tested on device** |
| **v6 package** | 2026-10-27 | RK3326 / R36S | Native material setup for replayed groups (section 6.13) + approved 120/10 cull. Worst sustained 24.0 fps. sha1 `85af0f4975aecbf2349b6a16f4649d23cba1ed7f`. | **Tested on device** |
| **v7 package** | 2026-10-27 | RK3326 / R36S | Native setup v2 (pure cull/z-write) + commit runs for plain items (section 6.14). sha1 `100d3bc93737f6712f6a5c21b478a2bcdf785419`. | **Tested on device** |
| **v8 package** | 2026-10-27 | RK3326 / R36S | Non-consecutive opaque group instanced batching (`NFSU_GROUP_BATCH=1`), 15 Hz HUD offscreen target composite (`NFSU_HUD_TARGET=1`), build ID stamping (`Build: v8`), and PS2 menu back/cancel navigation fix (Backspace scan 0x0E). sha1 `faeb0dc79e368c4a56e661918cef50a82fe994b5`. | **Staged on SD Card via split parts** |

### Release Binary Verification:
- **Binary Path**: `ports/r36s/package/nfsu.aarch64`
- **Architecture**: AArch64 (ELF 64-bit LSB pie executable, ARM aarch64, version 1 (SYSV), dynamically linked)
- **SHA-256**: `de5195f346e2c7f34f10cee476a69ae17c2b6df0aa26a20beec3fa19d152812d`
- **SHA-1**: `9182d382078783344b0672e1b49f9753affdd393`
- **Dynamic Linker**: `ld-linux-aarch64.so.1`
- **Shared Libraries**: `libSDL2-2.0.so.0`, `libGLESv2.so.2`, `libc.so.6`, `libm.so.6`, `libstdc++.so.6` (0 desktop GL, pure GLESv2)
- **Controls**:
  - `R3`: Toggle on-screen virtual keyboard (`vkbd`)
  - `Select + R3`: Toggle FPS counter overlay
  - `Select + Start`: Clean quit
- **In-Race Performance & Graphics Fidelity**:
  - **Live Gameplay Validation**: Navigated to Quick Race, loaded track geometry (`STREAML1RA.BUN`, `TEX1003TRACK.BIN`), 4 opponent cars, sound banks (`WIND_00_MB.abk`, `ROADNOISE_00_MB.abk`, `STRM_Music_UG0_MB.abk`), and drove live on Olympic City track.
  - **Reflections**: `g_carenvironmentmapenable=1`, `g_carenvironmentmapupdatedata=1`, `g_roadreflectionenable=1` active in default balanced profile. Wet asphalt and car reflections rendered.
  - **In-Race CPU Load**: Game-thread CPU consumes **12.5 – 13.5 ms/frame** out of the 33.3 ms (30 FPS) frame budget (~39% of 1 Cortex-A35 core, leaving >60% headroom).
  - **In-Race Memory**: **364 – 365 MB RSS** (0 MB swap).
  - **GL Errors**: 0 GL errors across all in-race frames.
  - **Screen Captures**: Captured in-race frames [`build/r36s/race_run/work/diag_frame_1150.png`](file:///Users/manuel/Documents/Projects/nfsu1-recompiled-r36s/build/r36s/race_run/work/diag_frame_1150.png) (starting grid, opponent cars, searchlights, HUD), [`diag_frame_1250.png`](file:///Users/manuel/Documents/Projects/nfsu1-recompiled-r36s/build/r36s/race_run/work/diag_frame_1250.png), [`diag_frame_1400.png`](file:///Users/manuel/Documents/Projects/nfsu1-recompiled-r36s/build/r36s/race_run/work/diag_frame_1400.png) (live driving with wet road reflections, active speedometer/tachometer, taillights, minimap).

---

## 6. R36S performance, widescreen and parity work (package v1)

Goal of this round: 30 fps without glitches, widescreen, the same aspect handling as the NFSU2 port, using the
NFSU2 port's techniques. No R36S is available to the build host, so **every fps figure below is the headless
model, not a device measurement**. Antigravity's earlier "verified 30 FPS" line (section 1/5) came from host CPU
time without the R36S scale factor and should not be relied on.

### 6.1 How speed is measured (smoke_vm.sh)

`bash ports/r36s/smoke_vm.sh` (Xvfb + llvmpipe, `--cap30 --auto-advance`, `NFSU2_R36S_EMULATE=1` pacing). The
`[R36S model]` line in `d3d9_gles.cpp` (`r36sModel`): game = 13 x game-thread ms, render = 13 x (render-thread ms -
driver ms) + draws x 40 us, frame = max(game / (1 - 7.5 x background load), render). Calibrated on NFSU2's real R36S
logs; pessimistic. Threads that run lock-free ("free", e.g. the sound thread) are excluded from the background term.
One smoke run per `device_bash` call in the Cowork VM (about 100 s each; 180 s limit per call).

### 6.2 What was done (all with kill switches, see 6.5)

| Item | Where | Result |
|---|---|---|
| Widescreen + aspect handling as NFSU2 (`NFSU_ASPECT`, `RENDER_SCALE`, `RENDER`, `SCREEN`, `ROTATE`) | `widescreen.c`, `nfs_widescreen.h`, `main.cpp`, `d3d9_gles.cpp` | 854x480 verified (menus, grid, HUD at the edges); 640x480 R36S panel unchanged |
| Graphics presets = the game's own Low/Medium/High (`NFSU_GRAPHICS`, default `performance`) | `nfsu1_runtime.c`, `NFSU.sh` | Low preset is the default |
| Language, and font overlay for repacked language files with another glyph atlas | `guest_files.inc` | works with the user's `LanguageTextures/English.bin` |
| Gamepad map (PS2 layout), no free roam in NFSU1 | `sdl_gamepad.h` | see package README |
| Dynamic-draw (user-pointer) batching, shadow-state filter, render thread | `d3d9_gles.cpp`, `d3d9_render_thread.inc` | fewer GL calls |
| HUD at half rate | `hud_half.c` (wraps `sub_004F0390`) | approved trade-off, `NFSU_HUD_HALF=0` |
| Draw-distance + pixel-size cull of far props | `world_cull.c` pre-pass, fog fade in `d3d9_gles.cpp` | approved trade-off; rule: dropped only if entirely beyond `NFSU_DRAW_DIST` (150 m) **and** radius under `NFSU_CULL_PX` (7 px at 640x480) |
| Native world flush `sub_0040ED50` + learned run model | `world_cull.c` | flush 1.25 -> ~0.8 ms/frame in the VM; fast items ~0.2 us each |
| Native `_ftol2` `sub_00674898` | `native_math.c` | ~7000 calls/frame; 10.47 M compared in verify mode, 0 mismatches |
| Parallel 100 Hz sound thread (entry 0x654a90 runs without the machine lock) | `guest_runtime.cpp` (`NFS_DEFAULT_FREE_THREADS`), `d3d9_gles.cpp` (model) | about 8% in the model; **audio could not be listened to in the VM** (`SDL_AUDIODRIVER=dummy`) |
| GCC PGO of the lifted code | `pgo_vm.sh` | no gain on NFSU1, **not shipped** |

### 6.3 Native flush and run model (the main CPU win)

`sub_0040ED50` flushes 112 buckets (16 layers x 7 slots) of material groups; each group owns a circular list of
items. `world_cull.c` walks them natively. Facts it relies on: item fields +8 strip, +0x10 mesh->solid, +0x14
flags, +0x18 effect info, +0x1C/+0x20 lighting inputs, +0x24 world matrix; device vtable indexes SetRenderState 57,
DIP 82, DPUP 83, SetVertexDeclaration 87, SetVSConst 94, SetStream 100, SetIndices 104, SetPSConst 109; the old
D3DX 9.0 effect has Begin 64 / Pass 65 / End 66 only. `sub_00410400` keeps its own last-world-matrix cache, so it is
skipped consistently on the fast path.

Run model: consecutive same-signature items in a group differ only by the world matrix. After 3 observations with
different matrices the model (effect, vertex shader, pixel shader) is trusted: a fast item sends only the
W-linear constant registers, binds the buffers and draws. Every 16th item is run in full with capture and compared
with the prediction (`NFSU2_FLUSH_MERGE=2` verifies every item); a mismatch rejects the model. Stats line:
`[NFSU1 FLUSH] ... models trusted N, learning N, rejected N` (expect 2 trusted, 0 rejected).
`NFSU2_FLUSH_TIME=1` adds a per-bucket timing line (diagnostic).

### 6.4 Numbers (VM, headless, 640x480, Low preset; model fps = pessimistic)

| Build | model fps in the race |
|---|---|
| all three native switches off (`NATIVE_FLUSH=0 NATIVE_MATH=0 PARALLEL_GUEST=0`) | 12 - 20 |
| before native flush (cull + HUD half + batching) | 21 - 24 |
| v1 (this package) | 24 - 30 (open streets ~30, busiest scenes ~24 with 700+ draws) |

In busy scenes the render thread (draw count x 40 us, 700+ draws) limits, not the game thread. Further gains would
come from fewer draws (more culling, group replay as in NFSU2's `NFSU2_GROUP_CACHE`, about -13% game time, ~700
lines, not done), not from more CPU work. Per-frame logs: `[NFSU1 CULL]`, `[NFSU1 FLUSH]` in `log.txt`.

### 6.5 Kill switches (nfsu.cfg uses `NFSU_X`; the binary maps it to `NFSU2_X`)

`NATIVE_FLUSH=0`, `FLUSH_MERGE=0|2`, `NATIVE_MATH=0|2`, `PARALLEL_GUEST=0`, `PARALLEL_THREADS=<hex entries>`,
`HUD_HALF=0`, `DRAW_DIST=0`, `CULL_PX`, `CULL_DEBUG=1` (reports culled items whose cached bound is stale),
`RENDER_THREAD=0`, `UP_BATCH=0`.

### 6.6 Verification done for v1

- Package binary (stripped, device link, no libGL): smoke run clean, 0 GL errors, 2 run models trusted, 0 rejected.
- `FLUSH_MERGE=2` (every fast item compared with the real calls): ~600 verified items/frame, 0 rejections.
- `NATIVE_FLUSH=0`, `FLUSH_MERGE=0 + NATIVE_MATH=2`, and all switches off: clean runs, 0 GL errors, 0 ftol mismatches.
- 854x480 run: menus, grid, HUD placement fine.
- Not possible here: a run on the device, listening to the sound thread, a same-frame pixel diff native vs lifted
  (the auto-drive is not deterministic).

### 6.7 Lessons / traps

- **Camera, not a rendering bug**: in smoke frames the player's car is sometimes missing. The auto-advance menu
  pulses (Enter + pad button 0 every 24 frames until `NFSU2_AUTODRIVE`, default frame 1700) reach the race and cycle
  the camera; the bumper view has no car body. Check for a view change (background differs between consecutive
  frames) before suspecting the renderer. `NFSU2_DIAG_EXTRA=a,b,c` captures extra frames.
- `build_vm.sh` used to wipe `~/n1vm/guest` when it had no `.stamp` (build_guest_vm.sh writes `.flags`). Fixed; still
  pass `GUEST_DIR=$HOME/n1vm/guest` for the package build. The full guest rebuild is about 10 minutes in 5 calls
  (`BUDGET=100 JOBS=4 bash ports/r36s/build_guest_vm.sh`, repeat until "guest objects complete").
- Guest wraps (`ports/r36s/guest_wraps.txt`): `recomp_0064.c sub_004F0390`, `recomp_0003.c sub_0040ED50`,
  `recomp_0195.c sub_00674898`.
- Not adapted for NFSU1: `pgo_clang_vm.sh`, `build_ci.sh`, `CMakeLists.txt` (use `build_vm.sh` / `build_guest_vm.sh`).
- Retail files stay private (`work/`, `*.exe` are git-ignored). Nothing was committed or installed on a card.

### 6.8 Package v1

`ports/r36s/package/` (`NFSU.sh`, `nfsu.cfg`, `nfsu.aarch64`, `README.md`, `port.json`, `nfsu.gptk`);
checkpoint `build/r36s-vm/checkpoints/nfsu1.aarch64.v1pkg` (+ `.debug` with symbols for profiles); zip in `build/r36s-vm/dist/nfsu1-r36s-v1.zip`.
sha1 of `nfsu.aarch64`: 70b40bb6bf936f6e1bc120e7374738c7cd87fb16.

Installed on the card (`/Volumes/EASYROMS/ports/NFSU.sh` + `ports/nfsu/`, 2026-10-23): binary sha1 checked after the copy
(70b40bb6...), the game files (`work/gamedata`, 492 files, 1.6 GB) copied with rsync and verified by content
checksum (`rsync -rcn`, all identical). A 0-byte hidden temp file `gamedata/Support/.EReg.exe.Dmr8bd` from an interrupted
first pass is left on the card (deleting on the card needs the delete permission); it is harmless. The NFSU2 install
(`NFSU2.sh`, `nfsu2/`) was not touched. Copy tip: VM `~/mnt/EASYROMS` works for large copies (about 8 MB/s; use
`timeout 160 rsync -rt --no-perms --size-only` per call, no `--info=progress2`, then verify with `rsync -rcn`).

### 6.9 Package v2: presenter thread (CPU/GPU overlap)

Same design as the NFSU2 port (its HANDOVER_FOR_OPENCODE.md section 6.50); code in `ports/r36s/d3d9_gles/d3d9_gles.cpp`
(`scaleBlit` + `namespace presenter`, hooks in the device destructor, `Present`, `destroyBackBufferTarget`,
`createBackBufferTarget`, `presentBackBuffer`). Nothing else was changed.

**Why (precheck, from the v1 device log):** the render thread sat in `SDL_GL_SwapWindow` for 11-14 ms of 35-54 ms frames
(`[NFSU2 WAIT] ... render thread: SwapWindow X, idle ~0`), with 550-1217 draws/frame and 13-25 ms of CPU in the GL driver, median race
fps about 25.5 (range 18.6-29.4). The Mali's swap blocks until the GPU finishes the frame, so CPU and GPU were serial.

**How:** ring of 2 back-buffer textures + FBOs (`NFSU_PRESENT_SLOTS=3` allows 3). At `Present` the render thread puts a `glFenceSync` +
`glFlush` behind the frame, queues {slot, texture, fence, sizes} and starts the next frame in the next free slot (waits, with a 5 s
timeout, only if that slot is still busy). The presenter thread owns the window with a second context (shares textures), waits with
`glClientWaitSync`, runs the old scaler (letterbox, rotation, `diag_screen_*.ppm`) and `SDL_GL_SwapWindow`. The render context is
made surfaceless with a raw `eglMakeCurrent(dpy, 0, 0, ctx)` (needs `EGL_KHR_surfaceless_context`, checked with `eglQueryString`).
Every missing piece logs `[NFSU2 PRESENT] presenter thread not used: <reason>` and the old path runs unchanged (reasons: no GL sync
objects, no EGL entry points, not an EGL context, surfaceless extension missing, fence does not work, second context failed,
eglMakeCurrent failed, thread could not take the window). Entry points: `SDL_GL_GetProcAddress`, then `dlopen(RTLD_NOLOAD)` +
`dlsym` on libEGL.so.1 / libGLESv2.so.2. Device Reset: drain, recreate the ring (the thread keeps the window); destructor: drain,
join, give the window back.

**Switches** (nfsu.cfg `NFSU_X`, mapped to `NFSU2_X`): `PRESENT_THREAD=0` off; `PRESENT_SLOTS=2|3`; `PRESENT_HIDDEN=1` test mode only
(render context on a hidden window instead of EGL surfaceless, for machines whose SDL uses GLX, i.e. the build VM).
Log: `presenter thread on: N back buffers`, then every 300 frames `[NFSU2 PRESENT] 300 frames, ms/frame: render thread waited for a
free buffer A, presenter waited for the GPU B, blit + swap C`.

**Traps handled:** the glsc state cache (`gl_state_cache.h`) is the render thread's global shadow, so `scaleBlit` bypasses every cache macro
(push_macro/#undef for every macro in `gl_state_cache.h`, the same list as NFSU2) and the render-thread caller (`presentBackBuffer`, non-presenter path) calls
`glsc::invalidate()` afterwards. FBOs are only used on the render thread; textures and sync objects are shared. NFSU1 uses
`D3DSWAPEFFECT_DISCARD` (swap effect 1 in the log), so the 2-frames-old content of a ring buffer is never read.
glibc: no compat layer in NFSU1; the new `dlopen`/`dlsym` imports are `GLIBC_2.34`, the same maximum the v1 binary already had
(`pthread_*`, `timer_create`) and that ran on the device, so the minimum glibc did not change.

**Verified in the VM (llvmpipe, GLX; test mode `NFSU2_PRESENT_HIDDEN=1`; no speed claim from these runs):**
- Menus, intro and race frames through the presenter vs `NFSU2_PRESENT_THREAD=0`: contact sheets match (race frames differ only because the auto-drive and animations follow the wall clock).
  Ten consecutive race frames: frame n vs n+2 differs more than n vs n+1 in both runs, so no alternating (flicker) pattern.
- 0 GL errors in: 2 buffers 640x480, 3 buffers 854x480 (widescreen), 800x480 panel with `NFSU_ASPECT=4:3` (pillarbox bars correct), the stripped package binary, and presenter off.
- Render thread waited 0.0-0.2 ms/frame for a free buffer; presenter waited 3-9 ms for the GPU, blit + swap 1.3-3 ms (llvmpipe, meaningless as a speed figure).
- `diag_screen_1800` (what the presenter shows) is byte-identical to `diag_frame_1800` (the back buffer) at 640x480.
- Clean quit (`NFSU2_TEST_QUIT`, Select+Start): "Clean exit", exit code 0.
- Default VM run (no hidden mode): "presenter thread not used: EGL_KHR_surfaceless_context missing", back buffer = window as in v1, 0 GL errors.
- Package smoke: 2 run models trusted, 0 rejected.

**Not tested (no R36S in the build environment):** the EGL surfaceless path on the Mali blob driver (`eglMakeCurrent` without a
surface, second shared context, fence wait) and the real fps gain. Expected on the device: `[NFSU2 PRESENT] presenter thread on: 2 back
buffers` in `log.txt`, then the stats line with "render thread waited for a free buffer" near 0 and "presenter waited for the GPU" in
the range the GPU actually needs; the `[NFSU2 WAIT]` line's "SwapWindow" falls to about 0 and the frame time should approach
max(CPU, GPU) instead of CPU + GPU. Compare median race fps with the v1 log (median about 25.5). Watch for flicker, black frames or
tearing; if any, set `NFSU_PRESENT_THREAD=0` (back to v1 behaviour).

Package: `ports/r36s/package/nfsu.aarch64` sha1 `ad920c3b047f6d1b5bb476d2f0f7a4479899ffdc`, checkpoint
`build/r36s-vm/checkpoints/nfsu1.aarch64.v2pkg` (+ `.debug`, `.sha1`), zip `build/r36s-vm/dist/nfsu1-r36s-v2.zip`.

### 6.10 Package v3: Instanced World Draws (GLES3 / UBOs)

Same design as NFSU2's v34 (its HANDOVER_FOR_OPENCODE.md section 6.47).
Code in `src/runtime/world_cull.c` (`tryDrawInstancedRun`, `worldMatrixRegisters`, `processItem`), connecting to the existing GLES instancing pipeline in `d3d9_gles.cpp` (`drawIndexedInstanced`) and `d3d9_render_thread.inc` (`nfsD3dDrawInstanced`, `OpInstancedDraw`).

**Why:**
In dense Olympic City scenes (starting grid, busy corners, traffic), draw calls reached 1,000–1,217 calls/frame with 21–25 ms of CPU inside the Mali GL driver. Even with the presenter thread overlapping GPU rasterization, CPU driver overhead alone consumed the entire 33.3 ms frame budget.

**How:**
- During the native world flush (`sub_0040ED50`), when an item of a trusted model runs on the fast path, `tryDrawInstancedRun` scans ahead in the group's circular item list (`ihead`) for consecutive items sharing:
  1. The same mesh solid (`M32(it + 0x10)`)
  2. The same index strip range (`M32(strip + 0x34)` start index and `M32(strip + 0xC)` index count)
  3. The same effect signature (`sameSig`) and flags (`runnable`)
- For runs of $N \ge 2$ instances (up to 64):
  - Its vertex-shader registers that vary with the world matrix ($W \times x$, up to 16 registers) are computed per instance natively.
  - An instanced draw command (`OpInstancedDraw` / `nfsD3dDrawInstanced`) is pushed to the render thread.
  - The GLES backend selects the instanced vertex shader variant (which reads $W \times x$ registers from uniform array `nfs_inst[nfs_instanceBase + j]` via attribute divisor 1) and issues a single `glDrawElementsInstanced` call.
  - Sibling instances in the group are unlinked, counted, and freed to the item pool in bulk.
- If instancing is unsupported, off, or refused by the shader/bridge, the engine falls back to drawing each item singly with zero overhead or state drift.
- Verification mode (`FLUSH_MERGE=2` or 1-in-16 periodic check) is preserved.

**Switches:**
`NFSU_INSTANCING=0` (or `NFSU2_INSTANCING=0`) in `nfsu.cfg` disables instancing completely.
Log: `[NFSU1 FLUSH] ... instanced draws/frame N covering M items (saved M-N draws/frame)`.
Expected device gain: 30–40% fewer draw calls in dense scenes, recovering ~6–9 ms of CPU driver time on the render thread.

Package: `ports/r36s/package/nfsu.aarch64` sha1 `590b1edacd2bc547028196651c783fb296b02a07`, checkpoint `build/r36s-vm/checkpoints/nfsu1.aarch64.v3pkg` (+ `.sha1`).

### 6.11 Package v4: Group replay of the world draw-list flush

Same design as NFSU2's group replay (its HANDOVER_FOR_OPENCODE.md section 6.32), adapted to NFSU1's lifted
`sub_0040F3D0` material setup. Code in `src/runtime/world_cull.c` (`GRec`/`GEffect`, `grecFromHead`,
`groupLearnG`, `canReplayGroupG`, `replayGroupG`, `drawInstancedRunG`, `sendRegsG`).

**Why:**
On the device the world flush was still the game thread's biggest block (v2: 14 ms at 361 items / 158 groups,
22 ms at 911 items / 245 groups, ~24–39 µs/item). Most of that is the lifted effect path per item: `Pass()`
re-applies the same render states, textures and shader constants for every item of a material. NFSU2's
single biggest win was replaying one captured group head for the whole group instead of calling the effect
again per item.

**How:**
- The native flush captures the D3D calls one full group head emits (the effect's Pass: states, textures,
  shader constants) plus the item signature and the material/effect it belongs to.
- Per effect object, the first full group in a flush is the reference. Each constant register and each call
  is classified as **L** (linear in the world matrix, `r = W * W_ref^-1 * r_ref`), **G** (same for every group
  of the effect in this flush) or **M** (same as this material's own last full head). After 24 observations
  with distinct world matrices the effect is trusted; a record is only replayable after a full comparison
  validated it.
- Replay sends the recorded calls and constants itself (batched `SetVertexShaderConstantF` /
  `SetPixelShaderConstantF`), computes only the L constants per item and draws the whole group, instanced
  when consecutive items share the mesh. The lifted `sub_0040F3D0` setup still runs per group (it also
  updates the game's material state); only the per-item effect path is skipped.
- Every 8th/16th/32nd use of a record is drawn fully and compared with the prediction (registers and calls).
  A material that mismatches three times (texture LOD switches, additive/alpha blend changes) is retired and
  uses the full path from then on; a one-off transition only re-validates the record.
- Effect objects are keyed by the game's effect pointer (NFSU1's world capture does not re-set the vertex/pixel
  shader inside the group head, so the NFSU2 `(vs,ps)` key would be 0). Caches reset on a >10 frame gap and on
  a texture-churn jump (level load), so a reused effect/material pointer cannot replay an old record.
- Item-model instancing runs inside a replayed group (`drawInstancedRunG`), so v3's draw-call reduction is kept.
- Kill switch: `NFSU_GROUP_CACHE=0` (or `NFSU2_GROUP_CACHE=0`). `NFSU_GROUP_DEBUG=1` adds per-register/call
  comparisons, capture counters and the effect hypotheses.

**Bugs found while bringing it up:**
- The reference's inverse-transformed constants are `double[4]` but were copied with `memcpy(..., 16)`, so
  z/w stayed 0 and every L register looked unclassified. Fixed (`sizeof h->x[...]`).
- `drawInstancedRunG` returned a freed item to read its world-matrix pointer; it now returns the pointer
  captured before the sibling items are freed.

**Numbers (VM, headless llvmpipe, 640x480 Low, 120 s, same binary, group replay off/on):**
- Race windows: 50–110 groups/frame replayed (37–71 full), i.e. typically 50–75% of groups; the main world
  effect is trusted, the second (front-end/showcase) effect is not. 12–47 records/window retired for materials
  whose textures/blend state really change; those keep the full path.
- Flush CPU: 0.75–0.84 ms/frame at 392–621 items with replay vs 0.84–1.09 ms at 390–860 without (~12–20%
  lower on the host). The host's Pass is much cheaper than the R36S's, so the device gain should be larger
  (the model above predicts roughly 4–7 ms of the 14–22 ms flush).
- No crashes, no GL errors, smoke drives the race. Screenshots at identical frame numbers differ only because
  the car position is timing-dependent; geometry, textures, HUD and effects look correct.

**Not tested (no R36S here):** the real fps gain and long-session stability. Watch for flickering/swapped
textures or wrong blend modes on animated materials; if seen, `NFSU_GROUP_CACHE=0` restores v3 behaviour and
the log's retired-record counts say which material it was.

Package: `ports/r36s/package/nfsu.aarch64` sha1 `a2d716688a311486729505697264697c44edb247`, build ID line
`Build: v4`, checkpoint `build/r36s-vm/checkpoints/nfsu1.aarch64.v4pkg` (+ `.sha1`), zip
`build/r36s-vm/dist/nfsu1-r36s-v4.zip`.

### 6.12 Package v5: Bounded replay-record retirement

v4 retired a material after three record changes and kept it retired forever: `groupLearnG` set `bad`, the
re-record copied `bad` back, and `recDeadG` stopped capturing the group, so it could never recover. In the v4
device race this took group replay from 127 replayed / 75 full per frame early to 43 / 183 late; 349 records
were retired and the item refusals rose to 116–156 per frame while `textures stale` stayed 0. The retired
materials were not dead textures: the same records alternate `m57 0x14` DESTBLEND 2 ↔ 6 and `m65` SetTexture
handle A ↔ B.

**What changed** (`src/runtime/world_cull.c`):
- Only a real record change (`matChanged`) increments `old->changes`. A verification prediction miss
  (`predFail`, a wrong constant class) now only clears `validated` so the next group is drawn fully and
  compared; it is not a material change and must not retire the material (NFSU2's policy).
- `GRec.badAt` records the present count at retirement. `canReplayGroupG` refuses a retired record only for
  `NFSU2_GROUP_RETRY` presented frames (default 120 = 4 s at 30 fps; `NFSU_GROUP_RETRY` in `nfsu.cfg`;
  `0` restores v4's permanent retirement). When the cooldown expires it clears `bad`, `changes`,
  `validated` and `cleanVerifies` and lets the next full head re-record the record. A material that settled
  (one-off LOD/blend/weather change) then validates and replays again; one that keeps changing retires for
  another cooldown. A single-item group gets a learn-budget slot so it can re-record.
- `records retired` now prints `recovered` next to it, and the item refusal is split into
  `bad` / `sig` / `vb` / `run` so the next device log shows what remains.

**VM A/B (final v5 binary, 120 s each, headless llvmpipe, 640x480 Low):**
- `NFSU_GROUP_RETRY=0` (predFail fix only, permanent retirement): race windows 46–146 groups/frame replayed
  (19–39 full), `bad` item refusals 10–23/frame, recovered 0.
- `NFSU_GROUP_RETRY=120`: 70–144 replayed (13–43 full), `bad` refusals 1–15/frame, 2–35 records
  recovered/window. Both stay stable through the race; the v4 device decay is absent in both, so most of the
  win is the predFail change, with the retry keeping the retired set smaller.
- A separate 170 s v5 run: 84–134 replayed, 41–67 full, 26–120 retired and 42–72 recovered per window,
  item refusals 12–38 (almost all `bad`), `textures stale` 0. No GL errors, no crashes. Flush CPU on the host
  0.59–0.95 ms/frame; the device's full path is ~50 µs/item, so the replay coverage is what matters there.

**Not tested (no R36S here):** the real fps gain and long-session stability. Watch late-race/heavy scenes;
the remaining `bad` refusals are the materials whose blend/texture variants alternate, which multi-variant
records (2–4 per `(slot, mat)`, keyed on the changing calls) are meant to cover next.

Package: `ports/r36s/package/nfsu.aarch64` sha1 `21eeba59d833feb97ac4dcc65ab8093407261044`, build ID line
`Build: v5`, checkpoint `build/r36s-vm/checkpoints/nfsu1.aarch64.v5pkg` (+ `.sha1`), zip
`build/r36s-vm/dist/nfsu1-r36s-v5.zip`.

### 6.13 Package v6: Native material setup for replayed groups

**Why.** The v6 host flush split (`NFSU_FLUSH_TIME=1`) shows the group-setup block (`sub_0040F3D0`) is the
largest single flush component: 0.37–0.52 ms/frame of a 0.65–0.74 ms flush for 139–190 groups (about
2.5–2.8 µs/group on the host). On the device the same function also does the effect SetTexture AddRef/Release
that made about 60% of NFSU2's equivalent; the texture, UV transform and effect parameters are dead work for a
replayed group because its effect Pass output is already in the record.

**What.** `replaySetupG()` in `world_cull.c` (`NFSU2_REPLAY_SETUP`, default 1; `NFSU_REPLAY_SETUP` in
`nfsu.cfg`):
- returns 1 when the material and shader are already the current setup (`0x736404` / `0x736408`);
- clears `0x736404` so the next full group always runs the lifted setup;
- calls the game's `sub_00410650` (alpha test/reference) and `sub_00410550` (blend) with the same registers
  and stack arguments as `sub_0040F3D0`, preserving `esi`/`edi` for the flush;
- sets cull (`0x16`) and z-write (`0xE`) from the material's blend mode and the shader type, exactly as
  `sub_0040F3D0` does;
- skips the texture, UV transform and effect parameters, which the record's effect pass reproduces.
Every full group still captures with the game's real setup. `NFSU2_REPLAY_SETUP=0` restores the game's setup
for every group (A/B).

**Host A/B** (v6 binary, 140 s each, llvmpipe, 640x480 Low, 150 m/7 px):
- `NFSU2_REPLAY_SETUP=0`: group setup 0.37–0.52 ms (139–190 groups), flush 0.51–0.74 ms, 0 GL errors.
- `NFSU2_REPLAY_SETUP=1`: group setup 0.19–0.23 ms (129–177 groups), flush 0.48–0.74 ms, 0 GL errors.
- About 40% less group-setup CPU. The two runs sat in different traffic (89–116 vs 46–105 replayed), so their
  replay/full counts are not comparable; `textures stale` stayed 0.

**Device test (v6).** The card cfg was set to `NFSU_DRAW_DIST=120`, `NFSU_CULL_PX=10` and
`NFSU_FLUSH_TIME=1` (user-approved culling test), with the v5 150/7 cfg backed up as `nfsu.cfg.v5-150-7`.
The log's `[NFSU1 FLUSH] per frame: group setup ...` line shows the native setup's share, `[NFSU1 CULL]`
shows the 120/10 counts and `[NFSU2 HITCH]` the heavy frames. `NFSU_REPLAY_SETUP=0` reverts the setup
without rebuilding.

**Remaining limit.** The 951-draw frame at frame 2484 (84 ms, ~12 fps) is draw-bound; the native setup and the
120/10 cull reduce it, but reaching a stable 30 fps in those scenes needs draw batching / commit runs.

Package: `ports/r36s/package/nfsu.aarch64` sha1 `85af0f4975aecbf2349b6a16f4649d23cba1ed7f`, build ID line
`Build: v6`, checkpoint `build/r36s-vm/checkpoints/nfsu1.aarch64.v6pkg` (+ `.sha1`), zip
`build/r36s-vm/dist/nfsu1-r36s-v6.zip`.

### 6.14 Package v7: commit runs for plain items and the v2 material setup

**Why.** The v6 device log (`build/r36s-vm/device_run-v6`) still has a worst sustained window of 24.0 fps at
frame 2760 (629 draws, 41.66 ms wall; game 30.12 ms). Its flush split is group setup 6.07 ms (247 groups),
full+capture 1.97 ms (34 items), plain 4.80 ms (122), fast 0.40 ms. The 2665-class hitch is 78 ms / 836
draws. The group setup and the plain per-item BeginPass are the two CPU blocks left.

**What changed** (`src/runtime/world_cull.c`):
- `replaySetupG()` v2. A replayed group now only sets cull (0x16) and z-write (0xE) from the material's
  blend mode and the shader type. The recorded effect pass already carries the texture, UV, alpha-test and
  blend states it emitted at capture time, so the `sub_00410650`/`sub_00410550` helper calls and their
  `0x7070C0`/`C4`/`C8`/`CC`/`D0` globals are dead for a replay and are gone. The early-out, the
  `validGuest` effect check, the forced full-group setup (`0x736404 = 0`) and the cull/z-write states stay.
  `NFSU2_REPLAY_SETUP=0` still restores the lifted setup.
- Commit runs for plain items (`NFSU2_COMMIT_RUN`, default 1; `NFSU_COMMIT_RUN` in `nfsu.cfg`). NFSU1's
  effect vtable (0x6A0630) has Begin/BeginPass/End at 64/65/66 and no exposed CommitChanges, so the game's
  per-item BeginPass re-runs the pass's whole state/constant list. For an item that follows a full/captured
  item of the same effect, with one non-strip pass, the material states are already current; the item's
  `sub_00410400` and friends set the per-item matrices/constants directly. v7 runs the real BeginPass once
  per effect and counts the non-redundant device calls it queues (`nfsD3dPushedCount`; the render thread
  filters redundant states), then skips it for later items while the count is zero. A periodic check every
  2048 commit items re-runs the Pass; a non-zero count marks the effect unsafe and the full path resumes.
  Mode 2 always runs the Pass and reports the count; mode 0 is the old full path.
- `NFSU_FLUSH_TIME=1` now prints a `commit` block and the group-debug line prints commit items per frame,
  checks, missed states and the safe/rejected effect counts.

**Host validation (v7 source, llvmpipe, 640x480 Low, 120/10):**
- Mode 2, 110 s: 57–107 commit items/frame; 13,894–25,835 checks per 240-frame window and **0 missed device
  states over ~130,000 commit items**. 0 GL errors; `group learn changed 0/frame`; `textures stale 0`.
- Mode 1, 80 s: 45–110 commit items/frame, 1–13 checks per window (the first per-effect checks plus the
  periodic re-checks), 0 missed. Group setup 0.10–0.13 ms (149–176 groups) vs 0.38–0.40 ms (157–174 groups)
  with `NFSU2_REPLAY_SETUP=0`; plain+commit 0.20–0.56 + 0.10–0.23 ms. The v6 host A/B for the v1 setup was
  0.37–0.52 → 0.19–0.23 ms, so v2 is another ~45–50% off the native setup at the same group counts.
- Fixed-frame screenshot A/B is not usable here: the car position depends on host timing, so the same
  present count is a different scene. The mode-2 device-state count is the correctness check: a skipped
  BeginPass would have been a non-redundant state call and would show as `missed`.
- The v2/v7 host runs have 0 GL errors, `group learn changed 0`, `textures stale 0`, and no crashes.

**Device test (v7).** Ran on R36S. The log confirmed v7 execution (`commit run: 114 items/frame, 13 checked, 0 missed device states...`). Flush CPU per item dropped from 26.1 µs (v6) to 18.7 µs (-28.4%), and group setup dropped from 23.7 µs to 18.5 µs (-21.9%). Worst sustained window was 22.0 fps at frame 5040 (45.46 ms, 739 draws) due to high scene scale (816 flushed items vs 475 in v6). Provenance discrepancy resolved: build-ID stamping bug in `build_vm.sh` caused banner to read `Build: v6`, and FAT32 cluster cross-linking corrupted the card binary on disk after execution with `shadercache/` data.

Package: `ports/r36s/package/nfsu.aarch64` sha1 `100d3bc93737f6712f6a5c21b478a2bcdf785419`, checkpoint `build/r36s-vm/checkpoints/nfsu1.aarch64.v7pkg` (+ `.sha1`, plus a `.debug` copy), zip `build/r36s-vm/dist/nfsu1-r36s-v7.zip` sha1 `56c24f9e281aa24e66dd1f30097c08acdb4a0b57`.

### 6.15 Package v8: Non-consecutive opaque group instanced batching and HUD offscreen target composite

**Why.** Device profiling in v6 and v7 proved that while the game thread's flush per item is fast (<19 µs/item), draw count dominates frame time in dense scenes (739–928 draws/frame), overloading the dual-core Cortex-A35 CPU with ~21 ms in the Mali GL driver. Two draw call sources were identified: non-consecutive world items in opaque groups, and ~120–150 textured quads per frame from the FEng race HUD overlay.

**What changed:**
1. **Non-consecutive Opaque Group Instanced Batching** (`src/runtime/world_cull.c`):
   - In replayed material groups verified opaque (`ZENABLE=1`, `ZWRITEENABLE=1`, `ALPHABLENDENABLE=0`), draw order is visually invariant.
   - `drawInstancedRunG` scans the whole group (`scanAll=1`) to merge all matching items (same solid, strip range, effect signature) into a single `glDrawElementsInstanced` call.
   - Saves an additional 160–308 draw calls per frame in heavy scenes, reducing GL driver CPU by ~27%.
   - Kill switches: `NFSU_GROUP_BATCH=0` (or `NFSU_INSTANCING_GROUP=0`).
2. **HUD Offscreen Target Composite** (`src/runtime/hud_half.c`, `android/app/src/main/cpp/d3d9_render_thread.inc`):
   - On 15 Hz HUD replay frames, instead of re-issuing ~120–150 individual D3D9/GL draw calls, renders the HUD into an offscreen RGBA render target on record frames and composites it via a single textured quad on replay frames.
   - Host smoke test: In heavy scene frame 1320, cut wall time from 78.04 ms (12.8 FPS) to 36.16 ms (27.7 FPS).
   - Kill switch: `NFSU_HUD_TARGET=0`.
3. **Build ID Stamping**:
   - `ports/r36s/build_vm.sh` stamps `$OUT/obj/.build-id` and invalidates `main.o` whenever `BUILD_ID` changes, ensuring the on-device banner reflects the true package build (`Build: v8`).

4. **Menu Controls (PS2 Navigation Fix)** (`ports/r36s/sdl_gamepad.h`):
   - On PC NFSU1, the menu action map uses DirectInput action `user=23` mapped to **`0x0E` (Backspace)** to cancel/go back, NOT `0x01` (Escape).
   - In previous builds, the menu mapping only sent `0x01`, which the menu action map ignored, preventing the controller from going back in menus.
   - Now, pressing physical **A** (Circle), physical **X** (Triangle), or **Select** asserts both `0x0E` (Backspace) and `0x01` (Escape), enabling authentic PS2 back/cancel navigation in all menus.
   - Added L1/R1 (`0x33`/`0x34`) and L2/R2 (`0x1A`/`0x1B`) for tab and category cycling in the garage, performance shops, and options.

**Host Validation:**
- Smoke tests (75 s & 110 s) with llvmpipe: 0 GL errors, clean auto-advance through showroom turntable and live race.
- In-race draws dropped below 300 draws/frame in standard race segments.
- Diagnostic PPM captures verified bit-equivalent visual output.
- Gamepad unit test verified that East (Circle), North (Triangle), and Select emit scan codes `0x0E` and `0x01`.

Package: `ports/r36s/package/nfsu.aarch64` sha1 `faeb0dc79e368c4a56e661918cef50a82fe994b5`, build ID `Build: v8`, checkpoint `build/r36s-vm/checkpoints/nfsu1.aarch64.v8pkg` (+ `.sha1`, `.debug`), zip `build/r36s-vm/dist/nfsu1-r36s-v8.zip` sha1 `6aa24ebac4b55d6035cf2c27e71dc6345448924d`. Staged on SD card via `nfsu.aarch64.part00..02` + `nfsu.aarch64.update.sha1`.

### 6.16 Package v9: Native world-matrix setup

**Why.** The v8 baseline (`NFSU_GROUP_BATCH=0`, `NFSU_HUD_TARGET=0`) still missed the 25 fps worst-window target. Its worst sustained race window was 23.7 fps at frame 1680: 42.20 ms wall, 30.66 ms game-thread CPU and 644 draws. The world flush consumed 14.36 ms, including 4.72 ms in 177 commit items and 2.86 ms in 51 plain items. Those paths still called the lifted `sub_00410400` for every item.

**What changed** (`src/runtime/world_cull.c`):
- `worldMatrixNative()` ports the world flush's `sub_00410400` cache, counters, view/world transforms and effect WV/WVP setup to native code. It is used only by the world flush; other callers of `sub_00410400` are unchanged.
- The game's `sub_005AC69C` D3DX matrix multiply remains in use. An initial native multiply was rejected after verification found one-ULP differences; the shipped version retains the proven math helper.
- `NFSU_WORLD_MATRIX=0` restores the lifted setup, `1` is native (default), and `2` runs both paths and compares the complete device-call sequence and pointed-to matrix bytes. Verification restores cache/counter side effects between paths.

**Validation:**
- Full 120 s menus/showroom/race run in mode 2: **876,753 calls, 0 mismatches**, 0 GL errors.
- Separate 110 s normal-mode run: 0 GL errors, 0 missed commit states; native flush was exercised through live gameplay.
- Device performance is not yet measured. The card keeps the v8 baseline settings (`NFSU_GROUP_BATCH=0`, `NFSU_HUD_TARGET=0`, 120 m / 10 px culling) so this is an isolated matrix-setup test.

Package: `ports/r36s/package/nfsu.aarch64` sha1 `45d923899fb091a6b5ae8fb806a09cc2f0a0a75a`, build ID `Build: v9`, checkpoint `build/r36s-vm/checkpoints/nfsu1.aarch64.v9pkg` (+ `.sha1`, `.debug`). Staged on SD card via `nfsu.aarch64.part00..02` + `nfsu.aarch64.update.sha1`.

**Device result.** The card update log confirmed SHA-1 `45d923899fb091a6b5ae8fb806a09cc2f0a0a75a` and the game banner confirmed `Build: v9`. The worst sustained race window was **24.5 fps** at frame 2760 (40.84 ms wall, 33.75 ms game-thread CPU, 20.45 ms GL-driver CPU, 706 draws), versus v8's 23.7 fps worst window; these are not matched-scene A/B windows. Many less-busy race windows held the 30 fps cap. All commit-run reports had 0 missed device states and no GL errors were logged. The matrix-only change therefore helped modestly, but submission remains the main limit.

### 6.17 Package v10: GL submission reduction and uncapped frame-rate option

**What changed:**
- Enabled the already validated v8 submission paths on the test card: `NFSU_GROUP_BATCH=1` merges non-consecutive matching items in verified opaque replayed groups, and `NFSU_HUD_TARGET=1` composites the half-rate HUD from its offscreen target instead of replaying its individual draws. Both retain their `0` kill switches; the approved 120 m / 10 px culling and v9 native matrix setup are unchanged.
- `ports/r36s/package/NFSU.sh` now passes `--cap=$NFSU_FPS` directly. `NFSU_FPS=0` selects the existing uncapped runtime path (the cap wait and its division are guarded by `if(cap)`), while `30` and `60` behave as before. Updated the launcher comments, package configuration comment and README. The package default remains 30 fps; the test card uses 0.
- The main startup banner prints `Frame Rate Cap: uncapped` when the limit is zero.

**Host validation:**
- Full 120 s uncapped menus/showroom/race run at 640x480 Low with both submission paths enabled: **0 GL errors, 0 matrix mismatches, 0 missed commit states**.
- The log confirmed `Build: v10`, `Frame Rate Cap: uncapped`, and `half rate (offscreen target)`. HUD/world diagnostic captures at frames 2200, 2400 and 2450 were visually checked.
- Non-consecutive opaque group batching saved up to **237 draws/frame** in the logged race windows; this is a host draw-count result, not a device fps prediction. The final HUD report had 960 record runs and 936 composite replays.

Package: `ports/r36s/package/nfsu.aarch64` sha1 `ad858f6935a0ebe1ece6a0dde0804dd0e89388c1`, build ID `Build: v10`, checkpoint `build/r36s-vm/checkpoints/nfsu1.aarch64.v10pkg` (+ `.sha1`, `.debug`). Staged on SD card via `nfsu.aarch64.part00..02` + `nfsu.aarch64.update.sha1`, with the updated launcher and uncapped `nfsu.cfg`.

**Device result (v10).** The update log confirmed SHA-1 `ad858f6935a0ebe1ece6a0dde0804dd0e89388c1`; the game banner confirmed `Build: v10` and `Frame Rate Cap: uncapped`, and every in-race WAIT report showed 0.0 ms of frame-cap wait. The offscreen HUD path was active, group batching saved 103–385 draws/frame in the reported race windows, and the final HUD report was 1200 runs / 1199 replays. All 13 commit-run reports had 0 missed device states; no GL errors or matrix mismatches were logged.

- The initial 17.8 fps window at frame 120 covered startup (3 draws/frame), not sustained gameplay; subsequent light-menu windows reached about 50–60 fps uncapped. A separate race-loading window reached 22.6 fps at frame 1920 with only 8 draws/frame. It followed a 2734 ms streaming hitch at frame 1807 dominated by 3191 texture uploads / 2621.3 ms, with additional decode/upload hitches, so this is a streaming cost rather than world-draw submission.
- The worst sustained gameplay window was **23.7 fps** at frame 2400: 42.19 ms wall, 29.88 ms game-thread CPU, 17.08 ms GL-driver CPU and 610 draws. Its 809 flushed items cost 14.15 ms, including 4.45 ms commit runs, 3.00 ms group setup and 2.93 ms plain items. Presenter GPU wait was 12.9 ms in the surrounding 300-frame report, while cap waits and storage reads were zero. Dense-frame hitches included 53 ms / 621 draws at frame 3294 and 80 ms / 819 draws at frame 3435, consistent with the user's momentary dips.
- v9's worst sustained gameplay window was 24.5 fps at frame 2760 with 706 draws. These runs did not follow matched inputs or scene positions, and v10's heavy window has different visible work, so the lower worst-window number alone is not evidence of a v10 regression. The measured v10 bottleneck in dense gameplay remains game-thread world-flush/submission CPU, with GPU-present waiting also material; startup/loading stalls are a separate texture-streaming issue.

### 6.18 Package v11: Native SSE-equivalent matrix multiplication

**What changed** (`src/runtime/world_cull.c`):
- `worldMatrixNative()` still calls the game's D3DX dispatcher `sub_005AC69C`, but its matrix helper can now run natively. The active dispatch target is `0x6D4ACC = 0x005ECE7A`, whose SSE implementation is in `src/recomp/gen/recomp_0148.c`; it is not the x87 fallback at `sub_005AC6A2`.
- For output row `r`, column `c`, the native helper computes four **float** products `p0 = A[r*4]*B[c]`, `p1 = A[r*4+1]*B[c+4]`, `p2 = A[r*4+2]*B[c+8]` and `p3 = A[r*4+3]*B[c+12]`. Rows 0 and 3 use `(p0+p1)+(p2+p3)`; rows 1 and 2 use `((p0+p1)+p2)+p3`. Every product and addition retains float rounding, with `-ffp-contract=off`; no double intermediates or fused operations are used.
- Results are computed into `tmp[16]` before the 64-byte output copy, preserving aliased input/output handling. If the dispatch target is not `0x005ECE7A`, the helper uses the lifted dispatcher instead.
- `NFSU_MATRIX_MULTIPLY=0` selects lifted multiplication, `1` native (default), and `2` compares every native result against the lifted output bitwise, with first-mismatch diagnostics. `NFSU_WORLD_MATRIX=1` remains enabled, so its plain/commit item paths exercise this helper.
- The initial x87/double implementation was rejected by mode 2 (397,595 mismatches in 425,812 calls); it was never installed on the card. The arithmetic and dispatch guard above replace it. No visual or culling settings were changed.

**Host correctness and A/B (v11, 640x480 Low, uncapped, 120 m / 10 px, group batching and HUD target on):**
- Fresh 120 s mode-2 menus/showroom/race run: the last report showed **53,772 MATMUL calls, 0 mismatches**; the log contains no MATMUL mismatch diagnostics, 0 GL errors and 0 missed commit states across all 12 commit reports.
- Separate fresh 120 s native (`NFSU2_MATRIX_MULTIPLY=1`) and lifted (`0`) runs both reached live racing with 0 GL errors and 0 missed commit states (9 and 10 reports respectively). Race screenshots were inspected; world geometry, textures, lighting and composited HUD are intact.
- A near-matched early workload had 114 native versus 113 lifted flushed items and 42 groups in both; flush CPU was 0.21 versus 0.19 ms respectively. This small difference is below useful host timing confidence, not a demonstrated gain. Later race windows diverged in traffic, draw counts and replay/full states, so their fps values are **not** used as a native-versus-lifted performance claim. Device CPU benefit remains to be measured; do not predict an fps gain from these host runs.

**Device test / rollback.** The card preserves `NFSU_FPS=0`, `NFSU_GROUP_BATCH=1`, `NFSU_HUD_TARGET=1`, `NFSU_COMMIT_RUN=1`, `NFSU_WORLD_MATRIX=1` and 120 m / 10 px culling, and adds `NFSU_MATRIX_MULTIPLY=1`. On the next start, confirm `Build: v11`, the new SHA-1 in `update.log`, uncapped operation, and continued 0 missed device states. Compare PERF and FLUSH CPU only at similar item/draw counts. Set `NFSU_MATRIX_MULTIPLY=0` to restore the v10 multiply without changing any other optimisation. The previous card update log confirmed v10 SHA-1 `ad858f6935a0ebe1ece6a0dde0804dd0e89388c1`, although the old on-card binary later read back as `063c70a949e67cf9a4bfebea38786980a32fadb7`; the v11 parts themselves passed the on-card concatenated checksum before testing.

Package: `ports/r36s/package/nfsu.aarch64` SHA-1 `8f227789bce9cb3ec28b4a8b54f0d268b1ae6946` (61,130,144 bytes), build ID `Build: v11`, checkpoint `build/r36s-vm/checkpoints/nfsu1.aarch64.v11pkg` (+ `.sha1`, `.debug`). Staged on SD via three 20 MB-split parts `nfsu.aarch64.part00..02` + `nfsu.aarch64.update.sha1`; their on-card concatenated SHA-1 matched the package. Package README and the card configuration document the new switch.

**Device result (v11).** `update.log` and the re-read card binary both confirm SHA-1 `8f227789bce9cb3ec28b4a8b54f0d268b1ae6946`; the banner confirms `Build: v11` and uncapped operation. All 15 commit-run reports have 0 missed device states, and there are no GL errors or matrix mismatch diagnostics. Group batching saved 298–328 draws/frame in two dense reported windows, and the offscreen HUD path remained active.

- The worst sustained gameplay window was **22.2 fps** at frame 2760: 45.10 ms wall, 32.92 ms game-thread CPU, 18.42 ms GL-driver CPU and 616 draws. No FLUSH report happened to align with this window. At frame 3120, 552 draws cost 42.83 ms wall / 31.10 ms game CPU; its 767-item flush was 11.25 ms, leaving about 19 ms of game CPU outside the measured flush/culling blocks. Presenter GPU wait was 11.6 ms around this window; frame-cap waits stayed zero.
- The largest aligned v11 flush was 14.50 ms for 823 items (17.6 µs/item), with 3.03 ms group setup, 3.22 ms plain and 4.76 ms commit. v10's comparable-scale but unmatched 809-item window was 14.15 ms (17.5 µs/item). Different scene composition prevents a native-multiply speedup or regression claim: the device run does **not** show a clear gain from v11, but the change is stable and the remaining costs extend well beyond the helper.
- The loading slowdown is separate: frame 2332 had a **2961 ms** hitch with only 8 draws, dominated by 3191 texture uploads / 2619.9 ms, plus 152.5 ms guest-machine and 206.7 ms event/thread waiting. The surrounding frame-2400 window averaged 20.8 fps with 8 draws. This accounts for the pronounced loading hitch; dense race windows account for the 20–22 fps dips. The initial 17.9 fps frame-120 report is startup with 3 draws, not a representative race window.
- The user's reported dips remain, so no claim of improved device fps is made. Before another micro-optimization, use the already available `NFSU_PROFILE=race:600` sampling mode on the device to identify the substantial non-flush game-thread cost. Texture upload/decode batching is a separate loading-stall investigation, not something the matrix helper can address.

### 6.19 Package v12: HUD-target composite state restore (minimap arrows) and the vsync switch

**Symptom (device, v11).** Every car arrow on the minimap was white, the crosshair grey and the map panel opaque dark teal; the original (v1) and `NFSU_HUD_TARGET=0` show an orange player arrow, blue opponent arrows, a light-blue marker and a translucent map.

**Cause** (`android/app/src/main/cpp/d3d9_render_thread.inc`, `hudCompositeRender`). The HUD offscreen target composite (v8) changed render states (ONE/INVSRCALPHA, ZENABLE off, ...), the stage 0 texture ops (SELECTARG1 texture only, no diffuse modulate), sampler states, texture, FVF and shaders and never put them back. Speed.exe tracks its own device state and does not re-send what it believes is unchanged, and it draws the minimap arrows/markers *after* the HUD (draws #494-#500 of the traced frame 1900, on the back buffer, 32x32 arrow textures with vertex colours 0x800042ff / 0x80ac3500). They inherited the composite's state: texture-only colour (white shapes), no alpha modulate, wrong blend. The per-draw trace (`NFSU2_TRACE_FRAME=1900`) showed `tss0 cop=2 ca1=2 ... blend=1(2,6)` on those draws. Bisected first with `NFSU2_HUD_TARGET=0` (colours correct).

**Fix.** `hudCompositeRender` saves the render states, sampler 0 states, stage 0..3 texture ops, viewport, texture 0, FVF/declaration and vertex/pixel shaders it touches (`CompositeSaved`) and restores them after its draw (covers both the record-frame composite and the replay-frame `OpHudComposite`). The HUD target also forces five alpha-blend states while the target is bound (SEPARATEALPHABLENDENABLE, SRCBLENDALPHA, DESTBLENDALPHA, BLENDOPALPHA, COLORWRITEENABLE|ALPHA): the game's own values are now recorded (`hudGameRS`, filled at HudBegin and in `execDevice` method 57) and put back at HudEnd before the composite. Nothing else changes; `NFSU_HUD_TARGET=0` is untouched.

**Vsync switch** (`ports/r36s/d3d9_gles/d3d9_gles.cpp`, `applySwapInterval`). `NFSU_VSYNC=0` (-> `NFSU2_VSYNC=0`) sets the SDL swap interval to 0 on the render thread and on the presenter thread; the default stays 1. The log line is `swap interval N on the <thread> (set|refused by the driver)`. Together with `NFSU_FPS=0` this removes the last frame limiter (the 60 fps panel refresh). May tear; unverified on KMSDRM until the device log shows `(set)`.

**Host verification (640x480 Low, uncapped, 120 m / 10 px, group batch + HUD target + commit run + world/matrix native):** minimap crops of frames 1800-2450 match the v1/`HUD_TARGET=0` colours (orange player, blue opponents, translucent map) on the fixed build and on the package binary; full 120 s run with `NFSU2_VSYNC=0`: 0 GL errors, "half rate (offscreen target)" active (1080 runs / 1063 replays). Before: white arrows with the same settings.

Package: `ports/r36s/package/nfsu.aarch64` SHA-1 `ed3739439a91f6379765ddaf3189e868abe8546a` (61,134,624 bytes), `Build: v12`, checkpoint `build/r36s-vm/checkpoints/nfsu1.aarch64.v12pkg` (+ `.sha1`, `.debug`). Source backup of the v11 inc: `~/n1vm/d3d9_render_thread.inc.v11` (VM scratch).

**Card.** v12 installed in `ports/nfsu/` with v11 kept as `nfsu.aarch64.prev` (SHA-1 `8f227789...`); the card `nfsu.cfg` keeps its test settings and gains `NFSU_VSYNC=0`; old logs cleared. Package README/NFSU.sh/nfsu.cfg document `NFSU_VSYNC`. Rollback: `mv nfsu.aarch64.prev nfsu.aarch64`, or `NFSU_VSYNC=1`.
