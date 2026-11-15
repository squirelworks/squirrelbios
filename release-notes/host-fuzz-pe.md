# Host PE/COFF fuzz harness (Phase G, G2)

Second CONTINUANCE Phase G step on SquirrelBIOS: `common/efi/image.c`
builds and runs on the host under a libFuzzer-style harness, without
QEMU. Mirrors the FAT harness (`make fuzz-fat`).

- **Harness:** `fuzz/pe_harness.c` (`LLVMFuzzerTestOneInput`) plus
  extended `fuzz/host_stubs.c` (pool, pages, protocol DB, quiet
  `printf`, `arch_sync_icache`, non-executing `image_call`). LoadImage /
  UnloadImage only — never StartImage.
- **Smoke:** `make fuzz-pe` builds `build/fuzz/pe-fuzz` and runs
  `fuzz/smoke-pe.sh` once over `fuzz/corpus-pe/` (seeds from
  `fuzz/mkcorpus_pe.py`). Optional — not part of `make test`.
- **libFuzzer binary:** `make fuzz-pe-libfuzzer` when clang with
  `-fsanitize=fuzzer,address` is available (desk / nightly later).
- **CI:** optional workflow `.github/workflows/fuzz-pe.yml`
  (`workflow_dispatch` and path-filtered push/PR). Short corpus only.
- **Docs:** `fuzz/README.md`, HANDOFF / ROADMAP Phase G card.

Next parsers after Authenticode (`make fuzz-authenticode`):
DHCP/TFTP/HTTP, USB descriptors, FDT; then a scheduled nightly with a
fixed libFuzzer budget.
