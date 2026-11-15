#!/usr/bin/env bash
# Short corpus smoke for the host USB descriptor fuzzer (Phase G).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${FUZZ_USB_BIN:-$ROOT/build/fuzz/usb-fuzz}"
CORPUS="${FUZZ_USB_CORPUS:-$ROOT/fuzz/corpus-usb}"

if [[ ! -x "$BIN" ]]; then
  echo "fuzz-usb: missing $BIN (run make fuzz-usb)" >&2
  exit 1
fi

if [[ ! -d "$CORPUS" ]] || [[ -z "$(find "$CORPUS" -type f 2>/dev/null | head -1)" ]]; then
  echo "fuzz-usb: building seed corpus in $CORPUS" >&2
  python3 "$ROOT/fuzz/mkcorpus_usb.py" -o "$CORPUS"
fi

echo "fuzz-usb: corpus smoke with $BIN"
fail=0
count=0
while IFS= read -r -d '' f; do
  count=$((count + 1))
  if ! "$BIN" "$f"; then
    echo "fuzz-usb: FAIL $f" >&2
    fail=1
  fi
done < <(find "$CORPUS" -type f -print0 | sort -z)

if [[ "$count" -eq 0 ]]; then
  echo "fuzz-usb: no corpus files in $CORPUS" >&2
  exit 1
fi

if [[ "$fail" -ne 0 ]]; then
  echo "fuzz-usb: $count inputs, failures" >&2
  exit 1
fi
echo "PASS  fuzz-usb ($count corpus inputs)"
