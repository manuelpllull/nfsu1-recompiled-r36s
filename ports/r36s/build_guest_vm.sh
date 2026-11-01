#!/bin/bash
# Rebuild the lifted game code (guest objects) in the Cowork VM, resumably, with chosen flags.
# Each device_bash call is limited to ~3 minutes and background jobs do not outlive it, so this starts
# compiles until BUDGET seconds have passed and can be re-run until it prints "guest objects complete".
# Objects go to $GUEST_OUT (default ~/n1vm/guest); build_vm.sh links them with GUEST_DIR=$GUEST_OUT.
#   GUEST_CFLAGS   default: -O2 for the Cortex-A35 (in-order scheduling), direct (non-GOT) globals, a constant
#                  guest base, the CPU state as one thread-local struct, x87 stack position in locals and the
#                  local x87 window (needs src/recomp/gen/nfs_lw.h from ports/r36s/lw/lw.sh)
#   BUDGET         seconds to keep starting compiles (default 30)
#   JOBS           parallel compiles (default 3: the VM has ~3.9 GB and the big files need ~1 GB each)
#   GUEST_CC       compiler (default gcc; pgo_clang_vm.sh passes clang)
# Files over 8 MB are split at function boundaries (scripts/split_android_sources.py) so no single compile
# outlasts a call. The sound-bank routines (src/recomp/gen_abk) are built without the local x87 window
# (lw.sh analyses src/recomp/gen only).
# Speed.exe 1.4 functions replaced by native code are listed in ports/r36s/guest_wraps.txt
# ("<source file> <function> ..."): only that definition is renamed *_orig, in a copy of the source.
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PCRECOMP="${PCRECOMP:-$ROOT/../pcrecomp}"
GUEST_OUT="${GUEST_OUT:-$HOME/n1vm/guest}"
GUEST_CFLAGS="${GUEST_CFLAGS:--O2 -mcpu=cortex-a35 -fno-pie -DNFS_FIXED_GUEST_BASE=0x1000000000ULL -DRECOMP_TLS=__thread -DRECOMP_TLS_STRUCT -ftls-model=local-exec -fno-math-errno -fno-stack-clash-protection -fno-stack-protector -DNFS_X87_LOCAL -DNFS_X87_LW}"
BUDGET="${BUDGET:-30}"
JOBS="${JOBS:-3}"
GUEST_CC="${GUEST_CC:-gcc}"
SPLIT="${GUEST_OUT}-split"
mkdir -p "$GUEST_OUT" "$SPLIT"
rm -f "$GUEST_OUT"/*.tmp
FLAGS="$GUEST_CFLAGS -w -fwrapv -fno-strict-aliasing -fexceptions -ffp-contract=off -g0 -fvisibility=hidden \
 -include $ROOT/android/app/src/main/cpp/arm64_guest.h -DRECOMP_STRICT_ICALL -DRECOMP_LOCAL_REGS -DRECOMP_X87_WINDOW \
 -DRECOMP_MEM_QUAL= -I$PCRECOMP/runtime/recomp32 -I$ROOT/src/recomp/gen -I$ROOT/src/recomp/gen_abk -I$ROOT/src/runtime"
STAMP="$GUEST_OUT/.flags"
# The stamp ignores where the checkout is mounted, so a second mount of the same folder does not wipe every
# object. Run from ~/mnt/Documents/Projects/nfsu1-recompiled-r36s (pcrecomp must be its sibling).
STAMPED="${FLAGS//$ROOT/ROOT}"; [ "$GUEST_CC" = gcc ] || STAMPED="$GUEST_CC $STAMPED"; STAMPED="${STAMPED//$PCRECOMP/PCRECOMP}"
[ -d "$PCRECOMP/runtime/recomp32" ] || { echo "ERROR: $PCRECOMP not found (pcrecomp must sit next to this checkout)"; exit 1; }
case "$FLAGS" in *NFS_X87_LW*) [ -f "$ROOT/src/recomp/gen/nfs_lw.h" ] || { echo "ERROR: src/recomp/gen/nfs_lw.h missing: run ports/r36s/lw/lw.sh"; exit 1; };; esac
if [ "$(cat "$STAMP" 2>/dev/null)" != "$STAMPED" ]; then rm -f "$GUEST_OUT"/*.o; echo "$STAMPED" > "$STAMP"; fi
SRCS=()
shopt -s nullglob
for f in "$ROOT"/src/recomp/gen/recomp_*.c "$ROOT"/src/recomp/gen_abk/abk_*.c; do
  if [ "$(stat -c %s "$f")" -gt 8000000 ]; then
    stem=$(basename "$f" .c)
    if [ ! -f "$SPLIT/$stem.done" ] || [ "$f" -nt "$SPLIT/$stem.done" ]; then
      python3 "$ROOT/scripts/split_android_sources.py" "$f" "$SPLIT" --max-bytes 6000000 > /dev/null
      touch "$SPLIT/$stem.done"
    fi
    for p in "$SPLIT/${stem}"_part_*.c; do SRCS+=("$p"); done
  else
    SRCS+=("$f")
  fi
done
shopt -u nullglob
[ ${#SRCS[@]} -gt 0 ] || { echo "No lifted sources in src/recomp/gen"; exit 1; }
HDR="$ROOT/android/app/src/main/cpp/arm64_guest.h"
WRAPS="$ROOT/ports/r36s/guest_wraps.txt"
wraps_for() { [ -f "$WRAPS" ] || return 0; awk -v f="$1" '$1==f { $1=""; print }' "$WRAPS"; }
# An object is stale when its source or arm64_guest.h is newer, or (only for files with wraps) the wrap list.
fresh() { [ -f "$1" ] && [ "$1" -nt "$2" ] && [ "$1" -nt "$HDR" ] && { [ -z "$(wraps_for "$(basename "$2")" | tr -d ' ')" ] || [ "$1" -nt "$WRAPS" ]; }; }
start=$(date +%s)
for src in "${SRCS[@]}"; do
  obj="$GUEST_OUT/$(basename "$src" .c).o"
  fresh "$obj" "$src" && continue
  [ $(( $(date +%s) - start )) -ge "$BUDGET" ] && break
  while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do sleep 0.5; done
  F="$FLAGS"
  case "$(basename "$src")" in abk_*) F="${F//-DNFS_X87_LW/}";; esac
  cc_src="$src"; wrap="$(wraps_for "$(basename "$src")")"
  if [ -n "${wrap// /}" ]; then
    cc_src="$GUEST_OUT/$(basename "$src" .c).wrap.c"
    cp "$src" "$cc_src"
    for w in $wrap; do
      sed -i "s/^void $w(void) {\$/void ${w}_orig(void) {/" "$cc_src"
      grep -q "^void ${w}_orig(void) {\$" "$cc_src" || { echo "ERROR: $w definition not found"; exit 1; }
    done
  fi
  ( $GUEST_CC -c "$cc_src" -o "$obj.tmp" $F && mv "$obj.tmp" "$obj" && echo "  CC $(basename "$src")" ) &
done
wait
left=0
for src in "${SRCS[@]}"; do fresh "$GUEST_OUT/$(basename "$src" .c).o" "$src" || left=$((left+1)); done
# Objects from sources that no longer exist (e.g. a file now built from parts) must not be linked.
for o in "$GUEST_OUT"/*.o; do
  n=$(basename "$o" .o); found=0
  for src in "${SRCS[@]}"; do [ "$(basename "$src" .c)" = "$n" ] && { found=1; break; }; done
  [ $found = 1 ] || rm -f "$o"
done
if [ "$left" = 0 ]; then echo "guest objects complete ($GUEST_OUT)"; else echo "guest objects: $left left, run again"; fi
