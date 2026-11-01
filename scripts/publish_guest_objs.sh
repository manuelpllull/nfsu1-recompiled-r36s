#!/bin/bash
# One-time (and after every rebuild of the game code): export the compiled game code (the guest *.o) and upload
# it to the GitHub release "r36s-guest-objs", where the release workflow reads it (like NFSU2's
# "build.py --export-guest --publish"). Needs the GitHub CLI (https://cli.github.com/, then "gh auth login").
#   bash scripts/publish_guest_objs.sh [guest-dir]      default guest-dir: build/r36s-vm/guest
# The archive is code derived from the game; a release asset of a public repository is public.
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GUEST_DIR="${1:-$ROOT/build/r36s-vm/guest}"
TAG=r36s-guest-objs
command -v gh >/dev/null || { echo "ERROR: needs the GitHub CLI: https://cli.github.com/" >&2; exit 1; }
[ -n "$(find "$GUEST_DIR" -name '*.o' 2>/dev/null | head -1)" ] || { echo "ERROR: no *.o files in $GUEST_DIR" >&2; exit 1; }
REPO="${REPO:-$(cd "$ROOT" && gh repo view --json nameWithOwner -q .nameWithOwner)}"
mkdir -p "$ROOT/dist"
TAR="$ROOT/dist/guest-objs.tar.gz"
tar czf "$TAR" --exclude='.stamp' -C "$GUEST_DIR" .
echo "exported $(find "$GUEST_DIR" -name '*.o' | wc -l | tr -d ' ') objects -> $TAR ($(du -h "$TAR" | cut -f1))"
if gh release view "$TAG" --repo "$REPO" >/dev/null 2>&1; then
  gh release upload "$TAG" "$TAR" --clobber --repo "$REPO"
else
  gh release create "$TAG" "$TAR" --repo "$REPO" --prerelease --latest=false \
    --title "Guest objects (CI input)" \
    --notes "Compiled game code exported by scripts/publish_guest_objs.sh. The release workflow links the published binary from it. Not for players."
fi
echo "published to $REPO release $TAG"
