#!/bin/bash
# Fails when a file derived from the game, or a game binary, is tracked by git.
# The lifted code (src/recomp/gen*), Speed.exe, compiled binaries (*.aarch64*), DLLs, the local work/ and
# build/ folders must stay local: they come from, or contain, the game's code. Run it before pushing:
#   bash scripts/check_no_game_files.sh
cd "$(dirname "${BASH_SOURCE[0]}")/.." || exit 2
bad="$(git ls-files | grep -Ei '(^|/)speed2?\.exe$|\.(exe|dll|aarch64|so)(\.[a-z0-9]+)?$|^src/recomp/gen(_[a-z0-9]+)?/|^work/|^build/|(^|/)(android_registry|gamedata)/' || true)"
if [ -n "$bad" ]; then
  echo "ERROR: these tracked files are game data or derived from the game and must not be committed:" >&2
  echo "$bad" | head -50 >&2
  exit 1
fi
echo "OK: no game files or game-derived code are tracked."
