#!/usr/bin/env bash
# Short corpus smoke for the host PE/COFF fuzzer (CONTINUANCE Phase G).
# Original code. Not a long nightly: each seed runs once through
# build/fuzz/pe-fuzz and must exit 0 (no crash / ASan abort).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${FUZZ_PE_BIN:-$ROOT/build/fuzz/pe-fuzz}"
CORPUS="${FUZZ_PE_CORPUS:-$ROOT/fuzz/corpus-pe}"

if [[ ! -x "$BIN" ]]; then
  echo "fuzz-pe: missing $BIN (run make fuzz-pe)" >&2
  exit 1
fi

if [[ ! -d "$CORPUS" ]] || [[ -z "$(find "$CORPUS" -type f 2>/dev/null | head -1)" ]]; then
  echo "fuzz-pe: building seed corpus in $CORPUS" >&2
  python3 "$ROOT/fuzz/mkcorpus_pe.py" -o "$CORPUS"
fi

echo "fuzz-pe: corpus smoke with $BIN"
fail=0
count=0
while IFS= read -r -d '' f; do
  count=$((count + 1))
  if ! "$BIN" "$f"; then
    echo "fuzz-pe: FAIL $f" >&2
    fail=1
  fi
done < <(find "$CORPUS" -type f -print0 | sort -z)

if [[ "$count" -eq 0 ]]; then
  echo "fuzz-pe: no corpus files in $CORPUS" >&2
  exit 1
fi

if [[ "$fail" -ne 0 ]]; then
  echo "fuzz-pe: $count inputs, failures" >&2
  exit 1
fi
echo "PASS  fuzz-pe ($count corpus inputs)"
