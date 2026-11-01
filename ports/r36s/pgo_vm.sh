#!/bin/bash
# Profile-guided build of the lifted game code (guest objects) in the Cowork VM.
#   bash ports/r36s/pgo_vm.sh gen   # instrumented objects (-fprofile-generate), resumable like build_guest_vm.sh
#   bash ports/r36s/pgo_vm.sh use   # optimized objects from the collected profile (-fprofile-use)
# Both build into $GUEST_OUT (default ~/n1vm/guest-pgo; the profile files are named after the object paths,
# so the two builds must use the same directory); the profile goes to $PGO (default ~/n1vm/pgo-data). Collect it with an
# instrumented binary (LDEXTRA=-fprofile-generate build_vm.sh with GUEST_DIR pointing at these objects) and a
# smoke run with NFSU2_GCOV_DUMP=<frame> (main.cpp calls __gcov_dump at that frame).
# -fprofile-partial-training keeps code the training run never reached optimized normally (the device plays
# other tracks and cars than the smoke run).
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BASE="-O2 -mcpu=cortex-a35 -fno-pie -DNFS_FIXED_GUEST_BASE=0x1000000000ULL -DRECOMP_TLS=__thread -DRECOMP_TLS_STRUCT -ftls-model=local-exec -fno-math-errno -fno-stack-clash-protection -fno-stack-protector -DNFS_X87_LOCAL -DNFS_X87_LW"
PGO="${PGO:-$HOME/n1vm/pgo-data}"
mkdir -p "$PGO"
case "$1" in
  gen) FL="$BASE -fprofile-generate -fprofile-dir=$PGO";;
  use) FL="$BASE -fprofile-use -fprofile-dir=$PGO -fprofile-partial-training -Wno-missing-profile -Wno-coverage-mismatch -fprofile-correction";;
  *) echo "usage: $0 gen|use"; exit 1;;
esac
GUEST_OUT="${GUEST_OUT:-$HOME/n1vm/guest-pgo}" GUEST_CFLAGS="$FL" JOBS="${JOBS:-3}" BUDGET="${BUDGET:-20}" bash "$ROOT/ports/r36s/build_guest_vm.sh"
