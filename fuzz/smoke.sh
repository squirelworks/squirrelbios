#!/usr/bin/env bash
# Short corpus smoke for the host FAT fuzzer (CONTINUANCE Phase G).
# Original code. Not a long nightly: each seed runs once through
# build/fuzz/fat-fuzz and must exit 0 (no crash / ASan abort).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${FUZZ_FAT_BIN:-$ROOT/build/fuzz/fat-fuzz}"
CORPUS="${FUZZ_CORPUS:-$ROOT/fuzz/corpus}"

if [[ ! -x "$BIN" ]]; then
  echo "fuzz-fat: missing $BIN (run make fuzz-fat)" >&2
  exit 1
fi

if [[ ! -d "$CORPUS" ]] || [[ -z "$(find "$CORPUS" -type f 2>/dev/null | head -1)" ]]; then
  echo "fuzz-fat: building seed corpus in $CORPUS" >&2
  python3 "$ROOT/fuzz/mkcorpus.py" -o "$CORPUS"
fi

echo "fuzz-fat: corpus smoke with $BIN"
fail=0
count=0
while IFS= read -r -d '' f; do
  count=$((count + 1))
  if ! "$BIN" "$f"; then
    echo "fuzz-fat: FAIL $f" >&2
    fail=1
  fi
done < <(find "$CORPUS" -type f -print0 | sort -z)

if [[ "$count" -eq 0 ]]; then
  echo "fuzz-fat: no corpus files in $CORPUS" >&2
  exit 1
fi

if [[ "$fail" -ne 0 ]]; then
  echo "fuzz-fat: $count inputs, failures" >&2
  exit 1
fi
echo "PASS  fuzz-fat ($count corpus inputs)"
