# Host DHCP/TFTP/HTTP, USB descriptor and FDT fuzz harnesses (Phase G)

CONTINUANCE Phase G host-fuzz step after Authenticode (`make fuzz-authenticode`,
#187): the remaining parsers that read untrusted input now build and run on
the host under libFuzzer-style harnesses, without QEMU. Lane `-2eaf`. Epic #82.

Shared pure parsers land in the firmware so smokes exercise the same code
paths as boot:

- **Network:** `common/efi/net_parse.c` (DHCP options, TFTP opcodes/OACK,
  HTTP URL + response headers/chunked/Content-Length). Used by
  `net_ip.c` / `net_http.c`.
- **USB:** `common/efi/usb_desc.c` (device descriptor check; configuration
  walk for BBB 8/6/0x50 bulk endpoints). Used by `xhci.c`.
- **FDT:** existing `common/fw/fdt.c` via `common/efi/fdt.c` (failed
  `fdt_init` clears the tree pointer).

| Target | Smoke | Corpus |
|---|---|---|
| `make fuzz-net` | `fuzz/net_harness.c` / `smoke-net.sh` | `fuzz/corpus-net/` (`mkcorpus_net.py`) |
| `make fuzz-usb` | `fuzz/usb_harness.c` / `smoke-usb.sh` | `fuzz/corpus-usb/` (`mkcorpus_usb.py`) |
| `make fuzz-fdt` | `fuzz/fdt_harness.c` / `smoke-fdt.sh` | `fuzz/corpus-fdt/` (`mkcorpus_fdt.py`) |

Optional path-filtered workflows: `.github/workflows/fuzz-net.yml`,
`fuzz-usb.yml`, `fuzz-fdt.yml`. Desk / overnight:
`make fuzz-*-libfuzzer`, plus scheduled `.github/workflows/fuzz-nightly.yml`
(60 s libFuzzer budget per harness; skips if clang fuzzer runtime is missing).

Docs: `fuzz/README.md`, HANDOFF / ROADMAP / TODO Phase G cards.
