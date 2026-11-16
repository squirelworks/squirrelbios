#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Host smoke for Phase G M7 docs-lint-harden: negative + positive fixtures.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
exec python3 "$ROOT/tools/docs_lint.py" --self-test
