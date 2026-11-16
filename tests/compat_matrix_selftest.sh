#!/bin/sh
# Host smoke for Phase G M17 suite matrix hardening: negative + positive
# fixtures, then the real-tree check (no network).
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
python3 "$ROOT/tools/compat_matrix_lint.py" --self-test
exec python3 "$ROOT/tools/compat_matrix_lint.py" --root "$ROOT"
