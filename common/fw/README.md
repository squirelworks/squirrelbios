# common/fw: shared C for the Phase E ports

Arch-neutral, freestanding C used by the firmware ports written in C
(`ppc64/` first) and by their test kernels. Nothing here touches hardware:
the port supplies `fw_putc()` / `fw_getc()`.

| File | |
|---|---|
| `fwlib.h` | the interface |
| `string.c` | `mem*`, `str*`, number parsing, unaligned big-endian access |
| `printf.c` | `fw_printf` / `fw_snprintf` (`%c %s %d %u %x %p`, widths, `l`) |
| `fdt.c` | flattened device tree: lookup by path, compatible, phandle; `reg` translated through `ranges` (ISA/LPC space cells included); a writer that copies a tree with nodes and properties added; text dump |
| `elf.c` | ELF64 executables in either byte order: probe, place `PT_LOAD` segments, or run in place |
| `prompt.c` | the `sq>` command prompt |

Written from the Devicetree Specification and the ELF-64 format; original
code.
