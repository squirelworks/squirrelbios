# Host fuzzing of UEFI parsers (Phase G)

CONTINUANCE Phase G asks for host builds of the parsers that read
untrusted input (FAT, PE/COFF, Authenticode, DHCP/TFTP/HTTP, USB
descriptors, the device tree) under libFuzzer or AFL, with a nightly
job. This directory hosts those harnesses. Still all original
code: the harnesses link the existing parsers against small host stubs
and do not vendor AFL, libFuzzer, or any other project's firmware.

## What runs

| Target | What it is |
|---|---|
| `make fuzz-fat` | Build `build/fuzz/fat-fuzz` with the host gcc and run `fuzz/smoke.sh` over `fuzz/corpus/` (one pass each; seconds). Optional — not part of `make test`. |
| `make fuzz-fat-libfuzzer` | Same sources with `clang -fsanitize=fuzzer,address` into `build/fuzz/fat-fuzz-libfuzzer`. Not run in the PR gate. |
| `make fuzz-pe` | Build `build/fuzz/pe-fuzz` and run `fuzz/smoke-pe.sh` over `fuzz/corpus-pe/`. Optional — not part of `make test`. |
| `make fuzz-pe-libfuzzer` | PE harness under libFuzzer + ASan. |
| `make fuzz-authenticode` | Build `build/fuzz/authenticode-fuzz` and run `fuzz/smoke-authenticode.sh` over `fuzz/corpus-authenticode/`. Optional — not part of `make test`. |
| `make fuzz-authenticode-libfuzzer` | Authenticode/PKCS#7 harness under libFuzzer + ASan. |
| `make fuzz-net` | Build `build/fuzz/net-fuzz` and run `fuzz/smoke-net.sh` over `fuzz/corpus-net/` (DHCP/TFTP/HTTP). |
| `make fuzz-net-libfuzzer` | Network parse harness under libFuzzer + ASan. |
| `make fuzz-usb` | Build `build/fuzz/usb-fuzz` and run `fuzz/smoke-usb.sh` over `fuzz/corpus-usb/`. |
| `make fuzz-usb-libfuzzer` | USB descriptor harness under libFuzzer + ASan. |
| `make fuzz-fdt` | Build `build/fuzz/fdt-fuzz` and run `fuzz/smoke-fdt.sh` over `fuzz/corpus-fdt/`. |
| `make fuzz-fdt-libfuzzer` | FDT harness under libFuzzer + ASan. |

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

### Authenticode / PKCS#7 (`authenticode_harness.c`)

1. Feed the bytes to `pkcs7_authenticode`, `pkcs7_detached`, and
   `x509_rsa_key` (`x86_64/secure/der.c`).
2. Call `pe_authenticode_hash` and walk a PE security directory's
   `WIN_CERTIFICATE` PKCS#7 bodies the same way `secure_check_image`
   does (no db/dbx enforcement; never StartImage).
3. Call `sb_db_walk` / `sb_entry_key` over EFI_SIGNATURE_LIST shaped
   input.
4. Host stubs cover pool/printf (`host_stubs.c`) and vars/fwcfg
   (`secure_host_stubs.c`) so the secure modules link without QEMU.
   Seeds are synthetic only (public DER / Authenticode / UEFI layouts).

### DHCP / TFTP / HTTP (`net_harness.c`)

1. `net_parse_dhcp` on the bytes (BOOTREPLY + magic cookie + options).
2. `net_parse_tftp` (DATA / ERROR / OACK).
3. Treat the bytes as an `http://` URL string via `net_parse_http_url`
   / `net_parse_ipv4`.
4. Stream the bytes through `net_http_rx_feed` (headers, Content-Length,
   chunked), including a mid-buffer split feed.
5. Production `net_ip.c` / `net_http.c` call the same `net_parse.c`.

### USB descriptors (`usb_harness.c`)

1. `usb_parse_device_desc` on the bytes (and on an 8-byte prefix).
2. `usb_config_find_bot` for a BBB (8/6/0x50) interface's bulk endpoints.
3. Production `xhci.c` uses the same `usb_desc.c`.

### FDT (`fdt_harness.c`)

1. `fdt_init` on the bytes (failed init clears the tree pointer).
2. Walk nodes with `fdt_next_node` / props / `fdt_reg` / path and
   compatible lookups.
3. Links `common/fw/fdt.c` and `common/efi/fdt.c`.

## Layout

```
fuzz/arch.h                 ARCH_* constants for the host build of sq.h
fuzz/host_stubs.c           pool + pages, protocol install/handle/uninstall,
                            guid_eq, quiet printf, arch_sync_icache,
                            image_call/image_exit stubs, minimal efi_st
fuzz/secure_host_stubs.c    vars_get/set + fwcfg stubs for secure/*.c
fuzz/fat_harness.c          FAT LLVMFuzzerTestOneInput + standalone main
fuzz/pe_harness.c           PE/COFF LLVMFuzzerTestOneInput + standalone main
fuzz/authenticode_harness.c Authenticode/PKCS#7 LLVMFuzzerTestOneInput + main
fuzz/net_harness.c          DHCP/TFTP/HTTP LLVMFuzzerTestOneInput + main
fuzz/usb_harness.c          USB descriptor LLVMFuzzerTestOneInput + main
fuzz/fdt_harness.c          FDT LLVMFuzzerTestOneInput + main
fuzz/mkcorpus.py            synthetic FAT seed images
fuzz/mkcorpus_pe.py         synthetic PE32+ seed images
fuzz/mkcorpus_authenticode.py  synthetic PKCS#7 / X.509 / siglist / PE+cert
fuzz/mkcorpus_net.py        synthetic DHCP / TFTP / HTTP seeds
fuzz/mkcorpus_usb.py        synthetic USB device/config descriptors
fuzz/mkcorpus_fdt.py        synthetic flattened device trees
fuzz/corpus/                FAT seeds (regenerable with mkcorpus.py)
fuzz/corpus-pe/             PE seeds (regenerable with mkcorpus_pe.py)
fuzz/corpus-authenticode/   Authenticode seeds (regenerable)
fuzz/corpus-net/            Network seeds (regenerable)
fuzz/corpus-usb/            USB seeds (regenerable)
fuzz/corpus-fdt/            FDT seeds (regenerable)
fuzz/smoke.sh               FAT corpus smoke used by make fuzz-fat
fuzz/smoke-pe.sh            PE corpus smoke used by make fuzz-pe
fuzz/smoke-authenticode.sh  Authenticode corpus smoke
fuzz/smoke-net.sh           Network corpus smoke
fuzz/smoke-usb.sh           USB corpus smoke
fuzz/smoke-fdt.sh           FDT corpus smoke
```

## CI

`.github/workflows/fuzz-fat.yml`, `fuzz-pe.yml`, `fuzz-authenticode.yml`,
`fuzz-net.yml`, `fuzz-usb.yml`, and `fuzz-fdt.yml` are optional:
`workflow_dispatch`, and pull requests that touch the matching parser,
`fuzz/**`, or the fuzz Makefile rules. They only run the short corpus
smoke, not a long fuzz campaign. The required gate remains `make test`.

`.github/workflows/fuzz-nightly.yml` is a scheduled (and
`workflow_dispatch`) job that builds each `fuzz-*-libfuzzer` binary and
runs libFuzzer for 60 seconds per harness when clang's fuzzer runtime
is present; otherwise it skips. Crash artifacts upload on failure.

## Next (Phase G)

- Optional Setup UI polish and other CONTINUANCE product work.
- Grow corpora from any overnight crashes; keep the `make fuzz-*`
  corpus smokes as the cheap PR checks.
