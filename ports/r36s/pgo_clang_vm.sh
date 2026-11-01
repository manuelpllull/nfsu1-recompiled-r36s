#!/bin/bash
# Clang (LLVM 17) profile-guided build of the lifted game code in the Cowork VM, as pgo_vm.sh does with GCC.
# Clang: ~/clang17 (clang-17, llvm-profdata, headers and the aarch64 profile runtime from the LLVM 17.0.6
# aarch64 release, build/toolchains/clang17.tar.xz).
#   bash ports/r36s/pgo_clang_vm.sh gen     # instrumented objects -> ~/clang-gen (resumable; run until complete)
#   link:  OUT=$HOME/nfsvm-v31 GUEST_DIR=$HOME/clang-gen LDEXTRA="$(bash ports/r36s/pgo_clang_vm.sh ldextra)" \
#          bash ports/r36s/build_vm.sh   (then copy the binary aside, it is overwritten by the next link)
#   run:   a smoke run with NFSU2_GCOV_DUMP=<frame>; the counters go to ~/clang-pgo-data/*.profraw
#   bash ports/r36s/pgo_clang_vm.sh merge   # -> ~/clang-pgo-data/nfsu2.profdata
#   bash ports/r36s/pgo_clang_vm.sh use     # optimized objects -> ~/clang-pgo (resumable)
#   bash ports/r36s/pgo_clang_vm.sh plain   # clang without profile -> ~/clang-o2 (for A/B)
# -mllvm -pgso=false: code the training run never reached is optimized for speed as usual, not for size (the
# device plays other tracks and cars; GCC's -fprofile-partial-training does the same).
# -ffp-contract=off (build_guest_vm.sh) keeps the floating-point results identical to the GCC build.
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LLVM="${LLVM:-$HOME/clang17}"
BASE="-O2 -mcpu=cortex-a35 -fno-pie -DNFS_FIXED_GUEST_BASE=0x1000000000ULL -DRECOMP_TLS=__thread -DRECOMP_TLS_STRUCT -ftls-model=local-exec -fno-math-errno -fno-stack-protector -DNFS_X87_LOCAL -DNFS_X87_LW"
DATA="${DATA:-$HOME/clang-pgo-data}"
mkdir -p "$DATA"
case "$1" in
  gen)   OUTD="$HOME/clang-gen"; FL="$BASE -fprofile-generate=$DATA";;
  use)   OUTD="$HOME/clang-pgo"; FL="$BASE -fprofile-use=$DATA/nfsu2.profdata -mllvm -pgso=false -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date";;
  plain) OUTD="$HOME/clang-o2";  FL="$BASE";;
  merge) "$LLVM/bin/llvm-profdata" merge -o "$DATA/nfsu2.profdata" "$DATA"/*.profraw && ls -la "$DATA/nfsu2.profdata"; exit 0;;
  ldextra) echo "-u__llvm_profile_runtime $LLVM/lib/clang/17/lib/aarch64-unknown-linux-gnu/libclang_rt.profile.a"; exit 0;;
  *) echo "usage: $0 gen|merge|use|plain|ldextra"; exit 1;;
esac
GUEST_CC="$LLVM/bin/clang" GUEST_OUT="$OUTD" GUEST_CFLAGS="$FL" JOBS="${JOBS:-4}" BUDGET="${BUDGET:-120}" \
  timeout 172 bash "$ROOT/ports/r36s/build_guest_vm.sh" 2>&1 | grep -E " error|Killed|complete|left" | tail -3
