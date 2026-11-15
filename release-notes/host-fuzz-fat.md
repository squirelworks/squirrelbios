# Host FAT fuzz harness (Phase G, first step)

A sizeable first step toward CONTINUANCE Phase G fuzzing on SquirrelBIOS:
`common/efi/fat.c` builds and runs on the host under a libFuzzer-style
harness, without QEMU.

- **Harness:** `fuzz/fat_harness.c` (`LLVMFuzzerTestOneInput`) plus
  `fuzz/host_stubs.c` (pool, protocol install, quiet `printf`, minimal
  `efi_st` / `get_time`). Still the original FAT parser; no vendor code.
- **Smoke:** `make fuzz-fat` builds `build/fuzz/fat-fuzz` and runs
  `fuzz/smoke.sh` once over `fuzz/corpus/` (seeds from `fuzz/mkcorpus.py`).
  Optional — not part of `make test`.
- **libFuzzer binary:** `make fuzz-fat-libfuzzer` when clang with
  `-fsanitize=fuzzer,address` is available (desk / nightly later).
- **CI:** optional workflow `.github/workflows/fuzz-fat.yml`
  (`workflow_dispatch` and path-filtered push/PR). Short corpus only.
- **Docs:** `fuzz/README.md`, HANDOFF / ROADMAP Phase G card.

Next parsers to host-build the same way: PE/COFF (`image.c`), then the
network and USB descriptor readers.
