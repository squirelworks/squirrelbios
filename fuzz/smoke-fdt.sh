#!/usr/bin/env bash
# Short corpus smoke for the host FDT fuzzer (Phase G).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${FUZZ_FDT_BIN:-$ROOT/build/fuzz/fdt-fuzz}"
CORPUS="${FUZZ_FDT_CORPUS:-$ROOT/fuzz/corpus-fdt}"

if [[ ! -x "$BIN" ]]; then
  echo "fuzz-fdt: missing $BIN (run make fuzz-fdt)" >&2
  exit 1
fi

if [[ ! -d "$CORPUS" ]] || [[ -z "$(find "$CORPUS" -type f 2>/dev/null | head -1)" ]]; then
  echo "fuzz-fdt: building seed corpus in $CORPUS" >&2
  python3 "$ROOT/fuzz/mkcorpus_fdt.py" -o "$CORPUS"
fi

echo "fuzz-fdt: corpus smoke with $BIN"
fail=0
count=0
while IFS= read -r -d '' f; do
  count=$((count + 1))
  if ! "$BIN" "$f"; then
    echo "fuzz-fdt: FAIL $f" >&2
    fail=1
  fi
done < <(find "$CORPUS" -type f -print0 | sort -z)

if [[ "$count" -eq 0 ]]; then
  echo "fuzz-fdt: no corpus files in $CORPUS" >&2
  exit 1
fi

if [[ "$fail" -ne 0 ]]; then
  echo "fuzz-fdt: $count inputs, failures" >&2
  exit 1
fi
echo "PASS  fuzz-fdt ($count corpus inputs)"
