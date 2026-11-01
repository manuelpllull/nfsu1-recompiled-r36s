#!/bin/bash
# Builds the PortMaster bundle for the R36S: dist/NFSU-R36S.zip (+ .sha256).
#   BINARY=path/to/nfsu.aarch64   put this binary in the bundle (default: none, the bundle is the launcher,
#                                 settings, metadata and docs only; the binary contains the lifted game code)
#   DIST=dir                      output folder (default <repo>/dist)
# The bundle never contains game files; gamedata/ holds a note saying what to copy there.
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PKG="$ROOT/ports/r36s/package"
DIST="${DIST:-$ROOT/dist}"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

mkdir -p "$STAGE/nfsu/gamedata" "$DIST"
cp "$PKG/NFSU.sh" "$STAGE/"
cp "$PKG/port.json" "$PKG/nfsu.gptk" "$PKG/nfsu.cfg" "$PKG/README.md" "$STAGE/nfsu/"
chmod 755 "$STAGE/NFSU.sh"
printf '%s\n' \
  "Need for Speed: Underground - PortMaster" \
  "========================================" \
  "" \
  "Copy your own retail PC game files into this folder (patch 1.4):" \
  "- Speed.exe (exactly 3,178,496 bytes)" \
  "- GLOBAL/ CARS/ TRACKS/ SOUND/ MOVIES/ LANGUAGES/ ..." \
  > "$STAGE/nfsu/gamedata/COPY_RETAIL_PC_FILES_HERE.txt"

if [ -n "$BINARY" ]; then
  [ -f "$BINARY" ] || { echo "BINARY=$BINARY not found" >&2; exit 1; }
  cp "$BINARY" "$STAGE/nfsu/nfsu.aarch64"; chmod 755 "$STAGE/nfsu/nfsu.aarch64"
  echo "bundle includes nfsu.aarch64 ($(wc -c < "$BINARY") bytes)"
else
  echo "bundle has no binary (set BINARY=... to include one)"
fi

rm -f "$DIST/NFSU-R36S.zip" "$DIST/NFSU-R36S.zip.sha256"
(cd "$STAGE" && zip -qr "$DIST/NFSU-R36S.zip" NFSU.sh nfsu)
(cd "$DIST" && sha256sum NFSU-R36S.zip > NFSU-R36S.zip.sha256)
echo "wrote $DIST/NFSU-R36S.zip"
