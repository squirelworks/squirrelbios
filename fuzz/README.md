# Host fuzzing of UEFI parsers (Phase G)

CONTINUANCE Phase G asks for host builds of the parsers that read
untrusted input (FAT, PE/COFF, Authenticode, DHCP/TFTP/HTTP, USB
descriptors, the device tree) under libFuzzer or AFL, with a nightly
job later. This directory hosts those harnesses. Still all original
code: the harnesses link the existing parsers against small host stubs
and do not vendor AFL, libFuzzer, or any other project's firmware.

## What runs

| Target | What it is |
|---|---|
| `make fuzz-fat` | Build `build/fuzz/fat-fuzz` with the host gcc and run `fuzz/smoke.sh` over `fuzz/corpus/` (one pass each; seconds). Optional — not part of `make test`. |
| `make fuzz-fat-libfuzzer` | Same sources with `clang -fsanitize=fuzzer,address` into `build/fuzz/fat-fuzz-libfuzzer`. Not run in CI. |
| `make fuzz-pe` | Build `build/fuzz/pe-fuzz` and run `fuzz/smoke-pe.sh` over `fuzz/corpus-pe/`. Optional — not part of `make test`. |
| `make fuzz-pe-libfuzzer` | PE harness under libFuzzer + ASan. Not run in CI. |

Each harness entry point is `LLVMFuzzerTestOneInput`, so the same object
is libFuzzer- and AFL-ready. Standalone builds supply a small `main`
that feeds files or stdin; libFuzzer builds define `FUZZ_NO_MAIN`.

### FAT (`fat_harness.c`)

1. Treat the bytes as a raw disk image (Block I/O over a mutable copy,
   sector size taken from the BPB when it is 512/1024/2048/4096).
2. Call `fat_install` (the same probe the firmware uses on a partition).
3. On success, `OpenVolume`, list a few directory entries, open a short
   list of paths, `GetInfo` / `Read`, and one create/write of `FUZZ.TMP`.
4. Free every host pool allocation and drop protocol state.

### PE/COFF (`pe_harness.c`)

1. Treat the bytes as a PE32+ image buffer.
2. `efi_image_init`, then `efi_load_image` (headers, sections, base
   relocations — the same path as firmware LoadImage).
3. On success, `efi_unload_image`. **Never** `StartImage`: that would
   execute a fuzzed entry point. Host `image_call` is a no-op stub.
4. Free every host pool/page allocation and drop protocol state.

## Layout

```
fuzz/arch.h          ARCH_* constants for the host build of sq.h
fuzz/host_stubs.c    pool + pages, protocol install/handle/uninstall,
                     guid_eq, quiet printf, arch_sync_icache,
                     image_call/image_exit stubs, minimal efi_st
fuzz/fat_harness.c   FAT LLVMFuzzerTestOneInput + standalone main
fuzz/pe_harness.c    PE/COFF LLVMFuzzerTestOneInput + standalone main
fuzz/mkcorpus.py     synthetic FAT seed images
fuzz/mkcorpus_pe.py  synthetic PE32+ seed images
fuzz/corpus/         FAT seeds (regenerable with mkcorpus.py)
fuzz/corpus-pe/      PE seeds (regenerable with mkcorpus_pe.py)
fuzz/smoke.sh        FAT corpus smoke used by make fuzz-fat
fuzz/smoke-pe.sh     PE corpus smoke used by make fuzz-pe
```

## CI

`.github/workflows/fuzz-fat.yml` and `.github/workflows/fuzz-pe.yml` are
optional: `workflow_dispatch`, and pull requests that touch the matching
parser, `fuzz/**`, or the fuzz Makefile rules. They only run the short
corpus smoke, not a long fuzz campaign. The required gate remains
`make test`.

## Next (still Phase G)

- Authenticode / PKCS#7, DHCP/TFTP/HTTP parsers, USB descriptors, FDT.
- A scheduled nightly that runs libFuzzer for a fixed budget and uploads
  crashes; keep `make fuzz-fat` / `make fuzz-pe` as the cheap PR smokes.
