# Host fuzzing of UEFI parsers (Phase G, first step)

CONTINUANCE Phase G asks for host builds of the parsers that read
untrusted input (FAT, PE/COFF, Authenticode, DHCP/TFTP/HTTP, USB
descriptors, the device tree) under libFuzzer or AFL, with a nightly
job later. This directory is the first step: the shared FAT12/16/32
Simple File System in `common/efi/fat.c`, exercised on the host without
QEMU.

Still all original code. The harness does not vendor AFL, libFuzzer, or
any other project's firmware; it only links the existing `fat.c` /
`lib.c` against small host stubs.

## What runs

| Target | What it is |
|---|---|
| `make fuzz-fat` | Build `build/fuzz/fat-fuzz` with the host gcc and run `fuzz/smoke.sh` over the seed corpus (one pass each; seconds). Optional — not part of `make test`. |
| `make fuzz-fat-libfuzzer` | Same sources with `clang -fsanitize=fuzzer,address` into `build/fuzz/fat-fuzz-libfuzzer` when you want a real fuzzing binary. Not run in CI. |

The harness entry point is `LLVMFuzzerTestOneInput` in `fat_harness.c`,
so the same object is libFuzzer- and AFL-ready. The standalone build
supplies a small `main` that feeds files or stdin; libFuzzer builds
define `FUZZ_NO_MAIN`.

Flow for one input:

1. Treat the bytes as a raw disk image (Block I/O over a mutable copy,
   sector size taken from the BPB when it is 512/1024/2048/4096).
2. Call `fat_install` (the same probe the firmware uses on a partition).
3. On success, `OpenVolume`, list a few directory entries, open a short
   list of paths, `GetInfo` / `Read`, and one create/write of `FUZZ.TMP`.
4. Free every host pool allocation and drop the installed SFS pointer.

## Layout

```
fuzz/arch.h          ARCH_* constants for the host build of sq.h
fuzz/host_stubs.c    efi_allocate_pool / free_pool, install_protocol,
                     guid_eq, quiet printf, minimal efi_st + get_time
fuzz/fat_harness.c   LLVMFuzzerTestOneInput + standalone main
fuzz/mkcorpus.py     synthetic seed images (FAT12/16/32-ish and broken)
fuzz/corpus/         checked-in seeds (regenerable with mkcorpus.py)
fuzz/smoke.sh        short corpus smoke used by make fuzz-fat
```

## CI

`.github/workflows/fuzz-fat.yml` is optional: `workflow_dispatch`, and
pull requests that touch `common/efi/fat.c`, `fuzz/**`, or the fuzz
Makefile rules. It only runs the short corpus smoke (`make fuzz-fat`),
not a long fuzz campaign. The required gate remains `make test`.

## Next (still Phase G)

- PE/COFF (`common/efi/image.c`) with the same host-stub pattern.
- Authenticode / PKCS#7, DHCP/TFTP/HTTP parsers, USB descriptors, FDT.
- A scheduled nightly that runs libFuzzer for a fixed budget and uploads
  crashes; keep `make fuzz-fat` as the cheap PR smoke.
