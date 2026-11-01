#!/bin/bash
# Regenerate the local-x87-window data for src/recomp/gen (after the lifted sources change).
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"; GEN="$ROOT/src/recomp/gen"; D="$(dirname "${BASH_SOURCE[0]}")"
python3 "$D/annotate.py" strip "$GEN"
python3 "$D/analyze.py" "$GEN" /tmp/nfs_lw_info.json
python3 "$D/annotate.py" apply "$GEN" /tmp/nfs_lw_info.json
python3 "$D/annotate.py" header "$GEN" /tmp/nfs_lw_info.json "$GEN/nfs_lw.h"
