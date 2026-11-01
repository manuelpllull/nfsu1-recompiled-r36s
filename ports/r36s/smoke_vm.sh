#!/bin/bash
# Headless smoke test of nfsu.aarch64 (Speed.exe 1.4) on an aarch64 Linux box without Docker (the Cowork VM).
# Runs the game under Xvfb + Mesa llvmpipe with --cap30 --auto-advance, captures the diagnostic frames
# (intro/menus 0..800, Quick Race menus ~1000-1400, race grid ~1600-1700, driving 1800+) and converts them
# to PNG in $RUN (default build/r36s-vm/run). The auto-advance path needs a saved profile in the game folder.
#   SECS=110 bash ports/r36s/smoke_vm.sh            # full run to the race
#   NFSU2_TRACE_FRAME=2300 bash ports/r36s/smoke_vm.sh   # per-draw state trace of one frame
#   SCREEN=854x480 bash ports/r36s/smoke_vm.sh           # another panel size (the game adapts to it)
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="${BIN:-$HOME/n1vm/nfsu.aarch64}"
RUN="${RUN:-$ROOT/build/r36s-vm/run}"
SECS="${SECS:-110}"
GAME="${GAME:-$ROOT/work/gamedata}"
GLSHIM="${GLSHIM:-$HOME/glshim}"
WORK="${WORK:-$HOME/n1run}"
mkdir -p "$RUN" "$WORK"; RUN="$(cd "$RUN" && pwd)"
rm -f "$WORK"/diag_frame_*.ppm "$WORK"/diag_screen_*.ppm "$WORK"/run_output.log
cd "$WORK"
# Same graphics profile as the device launcher (NFSU_GRAPHICS=performance|balanced|quality, default
# performance = the game's Low preset, like the card's nfsu.cfg): NFSU.sh turns it into NFSU2_GFX for nfsu1_runtime.c.
if [ -z "$NFSU2_GFX" ]; then
  NFSU_GRAPHICS="${NFSU_GRAPHICS:-performance}"
  eval "$(sed -n '/^case "\$NFSU_GRAPHICS"/,/^esac/p' "$ROOT/ports/r36s/package/NFSU.sh")"
  export NFSU2_GFX="$GFX"
fi
export NFSU2_DRAW_DIST="${NFSU2_DRAW_DIST:-150}" NFSU2_CULL_PX="${NFSU2_CULL_PX:-7}"
export NFSU2_R36S_EMULATE="${NFSU2_R36S_EMULATE:-1}" LP_NUM_THREADS="${LP_NUM_THREADS:-4}" SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 NFSU2_DIAG_FRAMES=1 NFSU2_GL_CHECK=1
export LD_LIBRARY_PATH="$GLSHIM${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
xvfb-run -a -s "-screen 0 ${SCREEN:-640x480}x24 -nolisten tcp" timeout -s KILL "$SECS" stdbuf -oL -eL "$BIN" "$GAME" ${FPS_ARG:---cap30} --auto-advance > run_output.log 2>&1 || true
cp run_output.log "$RUN/run_output.log"
python3 - "$WORK" "$RUN" <<'PY' || echo "PNG conversion skipped (Pillow missing?); PPM frames are in $WORK"
import sys, glob, os
from PIL import Image
src, dst = sys.argv[1], sys.argv[2]
for f in sorted(glob.glob(os.path.join(src, 'diag_frame_*.ppm')) + glob.glob(os.path.join(src, 'diag_screen_*.ppm'))):
    Image.open(f).save(os.path.join(dst, os.path.basename(f)[:-4] + '.png'))
PY
grep "R36S model" run_output.log | tail -4
echo "frames: $(ls "$RUN"/diag_frame_*.png 2>/dev/null | wc -l)  GL errors: $(grep -c 'GL ERROR' run_output.log || true)"
grep -E "NOTE:|compile failed|link failed|Framebuffer incomplete|MojoShader error|CreateCubeTexture|\[D3D9 RT\]|caps:" run_output.log | sort | uniq -c | sort -rn | head -40
