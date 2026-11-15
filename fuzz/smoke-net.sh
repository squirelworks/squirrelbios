#!/usr/bin/env bash
# Short corpus smoke for the host DHCP/TFTP/HTTP fuzzer (Phase G).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${FUZZ_NET_BIN:-$ROOT/build/fuzz/net-fuzz}"
CORPUS="${FUZZ_NET_CORPUS:-$ROOT/fuzz/corpus-net}"

if [[ ! -x "$BIN" ]]; then
  echo "fuzz-net: missing $BIN (run make fuzz-net)" >&2
  exit 1
fi

if [[ ! -d "$CORPUS" ]] || [[ -z "$(find "$CORPUS" -type f 2>/dev/null | head -1)" ]]; then
  echo "fuzz-net: building seed corpus in $CORPUS" >&2
  python3 "$ROOT/fuzz/mkcorpus_net.py" -o "$CORPUS"
fi

echo "fuzz-net: corpus smoke with $BIN"
fail=0
count=0
while IFS= read -r -d '' f; do
  count=$((count + 1))
  if ! "$BIN" "$f"; then
    echo "fuzz-net: FAIL $f" >&2
    fail=1
  fi
done < <(find "$CORPUS" -type f -print0 | sort -z)

if [[ "$count" -eq 0 ]]; then
  echo "fuzz-net: no corpus files in $CORPUS" >&2
  exit 1
fi

if [[ "$fail" -ne 0 ]]; then
  echo "fuzz-net: $count inputs, failures" >&2
  exit 1
fi
echo "PASS  fuzz-net ($count corpus inputs)"
