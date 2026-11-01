# Need for Speed: Underground — R36S & PortMaster guide

A native static recompilation of **Need for Speed: Underground** (Speed.exe, patch 1.4) for the **R36S**
(Rockchip RK3326, Mali-G31, 1 GB RAM) and other ARM64 handhelds running ArkOS, AmberELEC or ROCKNIX.
Package version **v7**.

---

## Installation

Copy the package to the SD card under `ports/`:

```text
/roms/ports/
├── NFSU.sh                         <- launcher
└── nfsu/
    ├── nfsu.aarch64                <- the game (native ARM64 binary)
    ├── nfsu.cfg                    <- your settings (optional, see below)
    ├── nfsu.gptk, port.json        <- PortMaster metadata
    └── gamedata/                   <- YOUR PC GAME FILES GO HERE
        ├── Speed.exe               <- patch 1.4, exactly 3,178,496 bytes
        ├── GLOBAL/  CARS/  TRACKS/  SOUND/  MOVIES/  LANGUAGES/ ...
```

No game data is included: bring your own legitimate PC copy, patched to **1.4**. The launcher warns if
`Speed.exe` is another size (the recompiled code only matches 1.4). Run the game once from the front end;
the first start may take a little longer while the shader cache is filled.

---

## Controls (R36S button labels, PS2 layout: B = Cross, Y = Square, A = Circle, X = Triangle)

| R36S button | Driving | Menus |
|:---|:---|:---|
| **Left stick** | Steer (analog) | Navigate |
| **D-pad** | — (the game has no free roam) | Navigate |
| **B** (bottom) | Accelerate | Select / confirm |
| **Y** (left) | Brake / reverse | Space |
| **A** (right) | Handbrake | Back |
| **X** (top) | Change camera | Back |
| **R1** | Nitrous | — |
| **L1** | Look behind | — |
| **R2 / L2** | Shift up / down (manual gearbox) | — |
| **Start** | Pause | Select / confirm |
| **Select** | Reset car | — |
| **Select + Start** | Quit to the system menu | Quit to the system menu |
| **Select + R3** | Show / hide the frame counter | Show / hide the frame counter |

The bumper camera (X cycles through the views) has no car body in it; that is the game, not a bug.

---

## Settings (`nfsu/nfsu.cfg`, plain `NAME=value` lines)

| Setting | Default | Effect |
|---|---|---|
| `NFSU_GRAPHICS` | `performance` | The game's own detail presets: `performance` (Low), `balanced` (Medium), `quality` (High). Low is what reaches 30 fps. |
| `NFSU_FPS` | `30` | Frame cap: `30`, `60`, or `0` for uncapped on faster hardware. |
| `NFSU_VSYNC` | `1` | `1`: the display's vertical sync is on (no tearing; the frame rate cannot exceed the panel's refresh, normally 60). `0`: vsync off, so an uncapped run is not held to the refresh rate (`NFSU_FPS=0`; may tear). Only matters above 30 fps. |
| `NFSU_LANG` | `Spanish` | Spanish, English, French, German, Italian, Dutch or Swedish. |
| `NFSU_DRAW_DIST` | `150` | Scenery props more than this many metres away **and** only a few pixels across are not drawn (the fog hides the edge). `0` draws everything. |
| `NFSU_CULL_PX` | `7` | ...smaller than this projected radius in pixels at 640×480. `0` culls by distance alone. |
| `NFSU_REPLAY_SETUP` | `1` | `1`: replayed groups use the native v7 material setup (only the cull and z-write states; the texture, UV, alpha-test and blend states come from the recorded effect pass). `0`: the game's setup for every group (A/B). |
| `NFSU_COMMIT_RUN` | `1` | `1`: a plain item skips its effect BeginPass when a real one would queue no changed device state (checked once per effect and then periodically). `0`: always run the BeginPass. `2`: always run it and report the count (diagnostic). |
| `NFSU_WORLD_MATRIX` | `1` | `1`: native per-item world/view transform setup. `0`: lifted setup. `2`: compare both paths (diagnostic). |
| `NFSU_MATRIX_MULTIPLY` | `1` | `1`: native, bit-verified SSE-equivalent matrix multiplication. `0`: lifted multiplication. `2`: compare every result bitwise (diagnostic). |
| `NFSU_GROUP_BATCH` | `1` | `1`: inside an opaque replayed group, batch all non-consecutive items sharing the same mesh/strip into single instanced draws. `0`: consecutive runs only. |
| `NFSU_HUD_HALF` | `1` | The race HUD is redrawn every second frame. `0` redraws it every frame. |
| `NFSU_HUD_TARGET` | `1` | `1`: HUD renders to an offscreen target and composites with a single quad on replay frames (cuts ~150 draws). `0`: recorded D3D9 call replay. |
| `NFSU_ASPECT` | `auto` | `auto`: real widescreen on panels wider than 4:3 (wider field of view, nothing stretched, HUD at the screen edges), 4:3 with bars on narrower panels. `4:3` always forces 4:3 with bars. |
| `NFSU_RENDER_SCALE` | — | e.g. `0.75`: render below the panel resolution and scale up (for big panels). |
| `NFSU_RENDER`, `NFSU_SCREEN`, `NFSU_ROTATE` | — | Exact render size, panel size if detected wrongly, `90`/`270` for rotated panels. |
| `NFSU_SHOW_FPS` | — | `1`: frame counter on from the start. |

The R36S's own 640×480 panel needs none of the display options.

---

## Performance

What the port does beyond a plain recompilation, all on the same game logic and simulation rate:

- Direct3D 9 → OpenGL ES 2.0 layer with a separate render thread, cached shaders and batched dynamic draws.
- Instanced world draws: consecutive scenery items of the same mesh and shader (barriers, trees, lamp posts)
  are merged into hardware instanced draws (`glDrawElementsInstanced`), cutting draw calls by 30–40%.
- The world draw-list flush is native code with a learned "run" model: consecutive scenery items that only
  differ by their world matrix skip most of the per-item work (every 16th item is checked against the
  original's real calls; a mismatch drops the model).
- Group replay (new in v4): the flush captures the D3D calls one full group emits (the effect's state,
  textures and shader constants) and learns, per effect, which constants are a function of the world matrix,
  which are the same for every group in the frame and which are per material. Replay then draws the whole
  group from that record and only computes the per-item constants, so most groups no longer pay for the
  lifted effect setup and Pass per item. Every 8th/16th/32nd use is verified with a full draw; a material
  whose calls changed three times (texture LOD, blend mode) is retired and re-recorded after 120 presented
  frames (`NFSU_GROUP_RETRY=0` restores permanent retirement).
- Native material setup for replayed groups (v7): the game's `sub_0040F3D0` still runs for full groups,
  but a replayed group only sets the cull and z-write states; the texture, UV, alpha-test and blend states
  are already in the recorded effect pass. The per-group texture/UV/effect-parameter updates and the
  `sub_00410650`/`sub_00410550` helpers are skipped. On the host this cut the group-setup block of the
  flush from 0.38–0.40 ms to 0.10–0.20 ms per frame; the following full group always runs the real setup.
  `NFSU_REPLAY_SETUP=0` reverts to the game's setup for every group.
- Commit runs for plain items (new in v7): this game's D3DX effect object exposes Begin/BeginPass/End but
  no CommitChanges, so every per-item BeginPass re-runs the pass's whole state and constant list. For a
  material group, the first full item's BeginPass has already applied the material state, and the next
  items' world/lighting calls set the per-item constants directly. v7 runs a real BeginPass once per effect
  and periodically to count the device states it would still change; while the count is zero, the following
  items of the group draw without it. The host mode-2 check covered about 130,000 commit items with 0
  changed device states. `NFSU_COMMIT_RUN=0` disables the commit runs; `2` always runs the BeginPass and
  reports the count (diagnostic).
- Native world/view setup and matrix multiplication for plain and commit-run world items (v9/v11): v11 reproduces the active D3DX SSE helper's float rounding and addition order exactly. Its mode-2 gameplay check covered 53,772 multiplies with 0 bitwise mismatches. `NFSU_WORLD_MATRIX=0` restores the lifted setup; `NFSU_MATRIX_MULTIPLY=0` restores only the lifted multiply; mode `2` enables verification.
- `_ftol2`, called ~7000 times per frame, is native; the 100 Hz sound-buffer thread runs on its own core.
- Presenter thread (new in v2): the game draws into two off-screen back buffers and a second thread scales them onto the
  screen and swaps. On the Mali the swap call waits for the GPU to finish the whole frame, so without this the CPU sat
  idle there and a frame cost CPU time + GPU time instead of the larger of the two. It needs OpenGL ES 3 fences and
  `EGL_KHR_surfaceless_context`; if either is missing it is not used and `log.txt` says
  `presenter thread not used: <reason>` (the old path then runs unchanged).
- Draw-distance cull of small far props, and the half-rate HUD (the two visual trade-offs above).

**Expected speed.** There is no R36S in the build environment, so the figure is a model, not a measurement: the
headless test measures the CPU time of every thread, scales it by a factor calibrated on the NFSU2 port's real
R36S logs and adds a per-draw GPU-driver cost. It does not include the presenter thread, whose real gain on the Mali has not been measured. It is deliberately pessimistic; the pre-v7 model gave about **24 to 30 fps in
the Low preset** (about 30 on open streets, about 24 in the busiest scenes with 700+ draws); with the three native speed-ups
below switched off the same test gave 12 to 20 fps. The model has not been re-run for v7. On the device, v6 measured
a worst sustained window of **24.0 fps** (frame 2760: 629 draws, 41.7 ms) and a 78 ms / 836-draw hitch; v5 was 19.7 fps.
v7 removes most of the replayed group setup and the per-item BeginPass for plain items, so the device test targets
25 fps or better in that window. Lowering `NFSU_DRAW_DIST` (e.g. 120) and raising
`NFSU_CULL_PX` (e.g. 10) trades distant prop detail for draw calls; keep `NFSU_GRAPHICS=performance`. `log.txt` has the `[NFSU1 CULL]`, `[NFSU2 PRESENT]` and
`[NFSU1 FLUSH]` lines with the per-frame numbers; please send it with any report.

### Troubleshooting switches (put in `nfsu.cfg`; each undoes one optimisation)

`NFSU_NATIVE_FLUSH=0` (original world flush), `NFSU_FLUSH_MERGE=0` (no run model; `2` checks every item),
`NFSU_INSTANCING=0` (no instanced draws; draws items singly), `NFSU_GROUP_CACHE=0` (no group replay; every group runs the full path),
`NFSU_GROUP_BATCH=0` (no non-consecutive instanced batching in opaque groups),
`NFSU_GROUP_RETRY=0` (a material whose record changed is retired for good, instead of being re-recorded after 120 frames),
`NFSU_REPLAY_SETUP=0` (the game's material setup for every group), `NFSU_COMMIT_RUN=0` (every plain item runs the game's effect BeginPass), `NFSU_COMMIT_RUN=2` (always run it and count changed device states; diagnostic), `NFSU_WORLD_MATRIX=0` (original world/view setup), `NFSU_MATRIX_MULTIPLY=0` (original matrix multiply; `2` compares results bitwise), `NFSU_NATIVE_MATH=0` (original `_ftol2`; `2` compares both), `NFSU_PARALLEL_GUEST=0` (all guest threads take
turns on one core, as in a plain port), `NFSU_RENDER_THREAD=0`, `NFSU_UP_BATCH=0`, `NFSU_HUD_HALF=0`, `NFSU_HUD_TARGET=0` (record/replay D3D9 calls instead of offscreen target composite),
`NFSU_DRAW_DIST=0`, `NFSU_PRESENT_THREAD=0` (the render thread swaps the window itself, as in v1), `NFSU_PRESENT_SLOTS=3`
(three back buffers instead of two: one more frame of latency, more slack when frame times vary). The parallel sound thread could not be listened to in the build environment (no audio
device there): if you hear crackling or stutter, try `NFSU_PARALLEL_GUEST=0` first.
`NFSU_FLUSH_TIME=1` writes the per-frame flush split (group setup / full+capture / plain / commit / fast) to `log.txt`;
it is diagnostic only and does not change the game.

---

## Troubleshooting

- **Black screen / exits at once**: check that `gamedata/Speed.exe` exists and is 3,178,496 bytes; read `nfsu/log.txt`
  (the previous run's log is `log.prev.txt`).
- **Controls not responding**: the binary reads the pad through SDL itself; check the pad in the front end.
- **Text in the wrong language / missing letters**: set `NFSU_LANG`; a repacked language file with a different
  glyph atlas is handled by a font overlay, so please report which file you used.
