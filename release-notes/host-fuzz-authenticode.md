# Host Authenticode/PKCS#7 fuzz harness (Phase G, G3)

Third CONTINUANCE Phase G host-fuzz step on SquirrelBIOS:
`x86_64/secure/der.c` (PKCS#7 Authenticode and detached SignedData, X.509
RSA keys), `pe_authenticode_hash` / certificate-table walk, and
`sb_db_walk` build and run on the host under a libFuzzer-style harness,
without QEMU. Mirrors `make fuzz-fat` / `make fuzz-pe`. Epic #82.

- **Harness:** `fuzz/authenticode_harness.c` (`LLVMFuzzerTestOneInput`)
  plus `fuzz/host_stubs.c` and `fuzz/secure_host_stubs.c` (vars / fwcfg
  stubs so `secure.c` / `authvar.c` link without QEMU). Synthetic seeds
  only; no vendor firmware blobs.
- **Smoke:** `make fuzz-authenticode` builds
  `build/fuzz/authenticode-fuzz` and runs `fuzz/smoke-authenticode.sh`
  once over `fuzz/corpus-authenticode/` (seeds from
  `fuzz/mkcorpus_authenticode.py`). Optional — not part of `make test`.
- **libFuzzer binary:** `make fuzz-authenticode-libfuzzer` when clang
  with `-fsanitize=fuzzer,address` is available (desk / nightly later).
- **CI:** optional workflow `.github/workflows/fuzz-authenticode.yml`
  (`workflow_dispatch` and path-filtered push/PR). Short corpus only.
- **Docs:** `fuzz/README.md`, HANDOFF / ROADMAP Phase G card.

Follow-on: DHCP/TFTP/HTTP, USB descriptors and FDT are
`make fuzz-net` / `fuzz-usb` / `fuzz-fdt`
(`release-notes/host-fuzz-net-usb-fdt.md`); nightly budget is
`.github/workflows/fuzz-nightly.yml`.
