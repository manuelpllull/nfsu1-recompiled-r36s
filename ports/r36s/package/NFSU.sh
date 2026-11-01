#!/bin/bash
# PortMaster Launch Script for Need for Speed: Underground (Recompiled, Speed.exe 1.4)
# Target Devices: R36S, RG351MP, RGB20S, RK2020 (Rockchip RK3326 ARM64)
# Supported OS: ArkOS, AmberELEC, ROCKNIX, TheRA

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

# PortMaster is optional: a fresh dArkOS has it only after Tools > Install.PortMaster. Without it the
# game still starts (the binary reads the pad through SDL itself; nothing here needs gptokeyb).
if [ -f "$controlfolder/control.txt" ]; then
  source "$controlfolder/control.txt"
  [ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
  get_controls
fi
ESUDO="${ESUDO:-sudo}"

CURR_TTY="/dev/tty1"
exec < "$CURR_TTY"

SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
if [ -n "$directory" ] && [ -d "/$directory/ports/nfsu" ]; then
  GAMEDIR="/$directory/ports/nfsu"
elif [ -d "$SCRIPT_DIR/nfsu" ]; then
  GAMEDIR="$SCRIPT_DIR/nfsu"
else
  GAMEDIR="$SCRIPT_DIR"
fi
cd "$GAMEDIR"

# Binary update delivered in parts (the build host can only write files of up to 30 MB to the card): join
# nfsu.aarch64.partNN, check the result against nfsu.aarch64.update.sha1, then replace the binary. On a
# checksum failure the parts are kept and the old binary runs. The result goes to update.log.
if [ -f nfsu.aarch64.update.sha1 ] && ls nfsu.aarch64.part* >/dev/null 2>&1; then
  cat nfsu.aarch64.part* > nfsu.aarch64.new
  if [ "$(sha1sum nfsu.aarch64.new | cut -d' ' -f1)" = "$(tr -d ' \n\r' < nfsu.aarch64.update.sha1)" ]; then
    chmod 755 nfsu.aarch64.new; mv -f nfsu.aarch64.new nfsu.aarch64
    rm -f nfsu.aarch64.part* nfsu.aarch64.update.sha1 ._nfsu.aarch64.part* ._nfsu.aarch64.update.sha1
    echo "updated nfsu.aarch64 ($(date)): $(sha1sum nfsu.aarch64)" > update.log
  else
    rm -f nfsu.aarch64.new
    echo "update parts failed the checksum ($(date)); kept the old binary" > update.log
  fi
  sync
fi

export DEVICE_ARCH="aarch64"
export LD_LIBRARY_PATH="$GAMEDIR/libs:$LD_LIBRARY_PATH"
[ -n "$sdl_controllerconfig" ] && export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

# R36S Mali-G31 ArkOS driver bindings
export SDL_VIDEODRIVER=kmsdrm
export SDL_AUDIODRIVER=alsa
export ALSOFT_DRIVERS=alsa
export SDL_VIDEO_GL_DRIVER=libGLESv2.so
export SDL_VIDEO_EGL_DRIVER=libEGL.so
export MESA_GL_VERSION_OVERRIDE=2.1
export MESA_GLES_VERSION_OVERRIDE=2.0
export MALLOC_CHECK_=0

# Clocks: CPU, GPU (Mali) and memory controller (DMC) at their top step while the game runs. The RK3326
# devfreq governors otherwise keep the GPU and DRAM clocks low, and this game is bound by CPU and memory.
# The previous governors are restored on exit.
SUDO="${ESUDO:-sudo}"
CLOCK_STATE=""
for gov in /sys/devices/system/cpu/cpufreq/policy*/scaling_governor /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor \
           /sys/class/devfreq/*/governor; do
  [ -f "$gov" ] || continue
  old=$(cat "$gov" 2>/dev/null)
  echo performance | $SUDO tee "$gov" > /dev/null 2>&1 && CLOCK_STATE="$CLOCK_STATE $gov=$old"
done
restore_clocks() {
  for entry in $CLOCK_STATE; do
    echo "${entry#*=}" | $SUDO tee "${entry%%=*}" > /dev/null 2>&1 || true
  done
}
trap restore_clocks EXIT

# R36S 1GB RAM Protection:
# Free reclaimable page caches to maximize contiguous memory before launching
sync
echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null 2>&1 || true

# Ensure safety swapfile to prevent OOM Killer from terminating the game during high texture loads
SWAP_TOTAL=$(free -m | awk '/Swap:/ {print $2}')
if [ -z "$SWAP_TOTAL" ] || [ "$SWAP_TOTAL" -lt 256 ]; then
  echo "[PortMaster] Ensuring 512MB safety swap on SD card..."
  if [ ! -f "$GAMEDIR/.swapfile" ]; then
    dd if=/dev/zero of="$GAMEDIR/.swapfile" bs=1M count=512 status=none
    mkswap "$GAMEDIR/.swapfile" >/dev/null 2>&1
  fi
  sudo swapon "$GAMEDIR/.swapfile" >/dev/null 2>&1 && SWAP_ADDED=1
fi

# Verify game assets presence (the PC install spells it Speed.exe)
EXE=""
for cand in "$GAMEDIR/gamedata/Speed.exe" "$GAMEDIR/gamedata/SPEED.EXE" "$GAMEDIR/gamedata/speed.exe"; do
  if [ -f "$cand" ]; then EXE="$cand"; break; fi
done

if [ -z "$EXE" ]; then
  printf "\033c" > "$CURR_TTY"
  printf "======================================================================\n" > "$CURR_TTY"
  printf " ERROR: Game assets not found!\n" > "$CURR_TTY"
  printf " Please copy your PC Need for Speed Underground (patch 1.4) files into:\n" > "$CURR_TTY"
  printf "   %s/gamedata/\n" "$GAMEDIR" > "$CURR_TTY"
  printf " Required files: Speed.exe, GLOBAL/, CARS/, TRACKS/, SOUND/, etc.\n" > "$CURR_TTY"
  printf "======================================================================\n" > "$CURR_TTY"
  sleep 5
  exit 1
fi

# The port was recompiled from Speed.exe patch 1.4 (3,178,496 bytes); another build will not run.
EXESIZE=$(stat -c%s "$EXE" 2>/dev/null || wc -c < "$EXE" | tr -d ' ')
if [ "$EXESIZE" != "3178496" ]; then
  echo "[NFSU1 WARN] Speed.exe size is $EXESIZE bytes (expected 3,178,496 for patch 1.4)" >> "$GAMEDIR/launcher.log" 2>&1
  printf "\033c" > "$CURR_TTY" 2>/dev/null || true
  printf " NOTICE: Speed.exe is %s bytes; this port needs patch 1.4 (3,178,496 bytes).\n" "$EXESIZE" > "$CURR_TTY" 2>/dev/null || true
  printf " Install the official 1.4 patch. Continuing in 4 seconds...\n" > "$CURR_TTY" 2>/dev/null || true
  sleep 4
fi

# Unbind vtconsole to allow KMSDRM full access to DRM primary plane
printf "\033c" > "$CURR_TTY" 2>/dev/null || true
printf "\e[?25l" > "$CURR_TTY" 2>/dev/null || true
echo 0 | sudo tee /sys/class/vtconsole/vtcon0/bind > /dev/null 2>&1 || true
echo 0 | sudo tee /sys/class/vtconsole/vtcon1/bind > /dev/null 2>&1 || true

# ---------------------------------------------------------------------------
# Graphics profile and frame-rate cap. Override in "$GAMEDIR/nfsu.cfg", e.g.:
#   NFSU_GRAPHICS=performance   # performance | balanced | quality: the game's own Low / Medium / High
#                               # detail presets (Speed.exe sub_00410ED0 levels 1 / 2 / 3)
#   NFSU_GFX=0                  # leave the options to the game (its benchmark picks a low level here)
#   NFSU_FPS=30                 # 30, 60, or 0 (uncapped on faster hardware)
#   NFSU_VSYNC=0                # 0: vsync off (with NFSU_FPS=0 the frame rate is not held to the panel's refresh; may tear)
#   NFSU_DRAW_DIST=150          # metres: scenery props beyond it that are a few pixels across are not drawn
#   NFSU_CULL_PX=7              # ... smaller than this projected radius in pixels (0 = cull by distance alone)
#   NFSU_HUD_HALF=1             # race HUD at half rate (0 = every frame)
#   NFSU_LANG=Spanish           # Spanish | English | French | German | Italian | Dutch | Swedish
# Display (the game adapts to the handheld's own screen; nothing to set on a 640x480 R36S):
#   NFSU_ASPECT=auto            # auto: widescreen on 16:9 etc. panels, 4:3 with bars on narrower ones
#                               # 4:3: always 4:3 (black bars on the sides of a wide screen)
#   NFSU_RENDER_SCALE=0.75      # render below the panel resolution and scale up (faster on big panels)
#   NFSU_RENDER=854x480         # or an exact render size
#   NFSU_SCREEN=854x480         # only if the panel size is detected wrongly
#   NFSU_ROTATE=90              # panel mounted rotated (90 or 270; portrait panels default to 90)
#   NFSU_SHOW_FPS=1             # frame counter on from the start (Select + R3 toggles it in game)
# Any NFSU_X (or NFSU2_X) troubleshooting switch in README.md is passed through to the game.
# ---------------------------------------------------------------------------
NFSU_GRAPHICS=performance
NFSU_FPS=30
NFSU_LANG=Spanish
NFSU_DRAW_DIST=150
NFSU_CULL_PX=7
[ -f "$GAMEDIR/nfsu.cfg" ] && . "$GAMEDIR/nfsu.cfg"
case "$NFSU_GRAPHICS" in
  performance)   # level 1 (Low)
    GFX="736334=1;E70=1;E74=1;E78=0;E7C=1;E80=0;E84=0;E88=0;E8C=0;E90=1;E94=0;E98=0;E9C=0;EA0=1;EA4=1;EA8=1;EAC=0;EB0=1;EB4=1" ;;
  quality)       # level 3 (High)
    GFX="736334=3;E70=1;E74=3;E78=1;E7C=2;E80=1;E84=1;E88=1;E8C=1;E90=2;E94=1;E98=1;E9C=1;EA0=1;EA4=1;EA8=1;EAC=1;EB0=3;EB4=1" ;;
  *)             # level 2 (Medium)
    GFX="736334=2;E70=1;E74=2;E78=1;E7C=2;E80=1;E84=1;E88=1;E8C=1;E90=2;E94=1;E98=0;E9C=0;EA0=1;EA4=1;EA8=1;EAC=1;EB0=3;EB4=1" ;;
esac
[ -n "${NFSU_GFX+x}" ] || NFSU_GFX="$GFX"
export NFSU_GFX NFSU_LANG
# Pass every NFSU_* setting of nfsu.cfg (display options, troubleshooting switches) to the game.
for v in $(set | sed -n 's/^\(NFSU_[A-Z0-9_]*\)=.*/\1/p'); do export "$v"; done
CAP_ARG="--cap=$NFSU_FPS"

# Record the clocks actually in effect (launcher.log next to log.txt)
{
  echo "[launcher] graphics=$NFSU_GRAPHICS fps=$NFSU_FPS gfx=$NFSU_GFX"
  # What this kernel offers for GPU load (read by the binary for its [NFSU2 GPU] log line)
  for d in /sys/class/devfreq/*; do [ -d "$d" ] && echo "[launcher] devfreq $d: $(ls "$d" 2>/dev/null | tr '\n' ' ')"; done
  for f in /sys/devices/platform/*gpu*/utilisation /sys/class/misc/mali0/device/utilisation; do
    [ -f "$f" ] && echo "[launcher] $f = $(cat "$f" 2>/dev/null)"
  done
  ls -d /sys/devices/platform/*gpu* 2>/dev/null | sed 's/^/[launcher] gpu device /'
  for f in /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq /sys/class/devfreq/*/cur_freq; do
    [ -f "$f" ] && echo "[launcher] $f = $(cat "$f" 2>/dev/null) ($(cat "$(dirname "$f")/governor" 2>/dev/null || cat "$(dirname "$f")/scaling_governor" 2>/dev/null))"
  done
  free -m
  uname -a
  for f in /sys/kernel/mm/transparent_hugepage/enabled /sys/kernel/mm/transparent_hugepage/defrag /sys/kernel/mm/transparent_hugepage/hpage_pmd_size; do
    [ -f "$f" ] && echo "[launcher] $f = $(cat "$f" 2>/dev/null)"
  done
  grep -i huge /proc/meminfo
} > "$GAMEDIR/launcher.log" 2>&1

# Keep the previous run's log (a crash on one run is otherwise overwritten by the next launch)
[ -f "$GAMEDIR/log.txt" ] && mv -f "$GAMEDIR/log.txt" "$GAMEDIR/log.prev.txt"

# Launch game: GLES 2.0 at the screen's native resolution (see Display above)
LANG_ARG="--lang=$NFSU_LANG"
./nfsu.aarch64 "$GAMEDIR/gamedata" $CAP_ARG $LANG_ARG > "$GAMEDIR/log.txt" 2>&1
EXIT_CODE=$?

# Get everything onto the card before the console returns to the menu (people often switch off right
# after quitting): pending writes first, then the swap file this launcher enabled on the card.
sync
if [ -n "$SWAP_ADDED" ]; then
  sudo swapoff "$GAMEDIR/.swapfile" >/dev/null 2>&1
  sync
fi

# Restore console and services on exit
echo 1 | sudo tee /sys/class/vtconsole/vtcon0/bind > /dev/null 2>&1 || true
echo 1 | sudo tee /sys/class/vtconsole/vtcon1/bind > /dev/null 2>&1 || true
$ESUDO systemctl restart oga_events >/dev/null 2>&1 &
printf "\033c" > "$CURR_TTY" 2>/dev/null || true
printf "\e[?25h" > "$CURR_TTY" 2>/dev/null || true
exit $EXIT_CODE
