#!/usr/bin/env bash
# Short corpus smoke for the host Authenticode/PKCS#7 fuzzer (Phase G).
# Original code. Not a long nightly: each seed runs once through
# build/fuzz/authenticode-fuzz and must exit 0 (no crash / ASan abort).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${FUZZ_AUTH_BIN:-$ROOT/build/fuzz/authenticode-fuzz}"
CORPUS="${FUZZ_AUTH_CORPUS:-$ROOT/fuzz/corpus-authenticode}"

if [[ ! -x "$BIN" ]]; then
  echo "fuzz-authenticode: missing $BIN (run make fuzz-authenticode)" >&2
  exit 1
fi

if [[ ! -d "$CORPUS" ]] || [[ -z "$(find "$CORPUS" -type f 2>/dev/null | head -1)" ]]; then
  echo "fuzz-authenticode: building seed corpus in $CORPUS" >&2
  python3 "$ROOT/fuzz/mkcorpus_authenticode.py" -o "$CORPUS"
fi

echo "fuzz-authenticode: corpus smoke with $BIN"
fail=0
count=0
while IFS= read -r -d '' f; do
  count=$((count + 1))
  if ! "$BIN" "$f"; then
    echo "fuzz-authenticode: FAIL $f" >&2
    fail=1
  fi
done < <(find "$CORPUS" -type f -print0 | sort -z)

if [[ "$count" -eq 0 ]]; then
  echo "fuzz-authenticode: no corpus files in $CORPUS" >&2
  exit 1
fi

if [[ "$fail" -ne 0 ]]; then
  echo "fuzz-authenticode: $count inputs, failures" >&2
  exit 1
fi
echo "PASS  fuzz-authenticode ($count corpus inputs)"
