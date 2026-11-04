# Build configurations

`make PLATFORM=<platform> CONFIG=<name>` builds firmware from
`configs/<platform>/<name>.cfg` into `build/<platform>/<name>/`.

| Platform | Output | Machine |
|---|---|---|
| `x86-pc` | `squirrel-bios.rom` | QEMU PC (`-bios`) |
| `aarch64-virt` | `squirrel-bios.bin` | QEMU AArch64 `virt` (`-bios`) |
| `riscv64-virt` | `squirrel-bios.bin` | QEMU RISC-V `virt` (`-bios`) |

A `.cfg` file is `KEY=VALUE` lines; `#` starts a comment line. Keys are
`CONFIG_*`; values are decimal or `0x` hex numbers, or `y` / `n` (1 / 0).
`tools/mkconfig.sh` turns it into `config.inc` for the platform's
assembler (and `config.h` for C sources to come) and rejects anything
malformed.

Every key has a built-in default in the source, equal to today's
behaviour, and a key left out of a `.cfg` keeps that default. Each
platform's `default.cfg` writes the defaults out, and `make test`
(`config-default`) checks that building it gives the same bytes as plain
`make`. Change a default in the source and in `default.cfg` together.

## Keys

### x86-pc

| Key | Default | Meaning |
|---|---|---|
| `CONFIG_COM1_BASE` | `0x3F8` | I/O port of the serial console |
| `CONFIG_ATA_ATTEMPTS` | `3` | Tries per ATA read before reporting the failure |
| `CONFIG_ACPI` | `y` | ACPI RSDP/RSDT/FADT/DSDT in the ROM (`x86/tables/acpi/`) |
| `CONFIG_SMBIOS` | `y` | SMBIOS 2.4 entry point and tables (`x86/tables/smbios/`) |
| `CONFIG_BZIMAGE` | `y` | Linux bzImage loader (`x86/boot/bzimage.inc`); without it a `HdrS` disk boots as a plain boot sector |
| `CONFIG_OPTROM` | `y` | Option ROM scan at `0xD0000`–`0xEFFFF` (`x86/boot/optrom.inc`) |
| `CONFIG_CDBOOT` | `y` | El Torito no-emulation boot from an ATAPI CD and `INT 13h` `DL = E0h` (`x86/drivers/atapi/`, `x86/boot/eltorito.inc`) |
| `CONFIG_AHCI` | `y` | A SATA disk on an AHCI controller as `DL = 80h` when no IDE disk answered: boot and `INT 13h` (`x86/drivers/ahci/`). Needs `CONFIG_PCI` |
| `CONFIG_PCI` | `y` | PCI bus 0 enumeration, BAR placement, PIIX3 IRQ routing, PCI BIOS `INT 1Ah` `AX=B1xx` and `$PIR` (`x86/drivers/pci/`) |
| `CONFIG_FLOPPY` | `y` | 1.44 MB diskettes: FDC driver, `INT 13h` `DL = 0x00`/`0x01`, `INT 1Eh`, boot from A: (`x86/drivers/fdc/`, `x86/boot/floppy.inc`) |

`x86-pc/minimal.cfg` turns the four modules off. `make test` boots it
(`config-x86-minimal`) and checks that it really has no ACPI tables
(`config-x86-minimal-noacpi`). The ROM stays 64 KB; the space is padding.

### aarch64-virt

| Key | Default | Meaning |
|---|---|---|
| `CONFIG_UART_BASE` | `0x09000000` | PL011 console |
| `CONFIG_FWCFG_BASE` | `0x09020000` | QEMU fw_cfg MMIO |

### riscv64-virt

| Key | Default | Meaning |
|---|---|---|
| `CONFIG_UART_BASE` | `0x10000000` | NS16550A console |
| `CONFIG_MAX_HARTS` | `512` | Hart IDs the SBI tracks; higher IDs park |

`riscv64-virt/small.cfg` (8 harts) shows a non-default build; `make test`
boots it as `config-riscv-small`.
