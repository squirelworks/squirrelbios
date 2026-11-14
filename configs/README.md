# Build configurations

`make PLATFORM=<platform> CONFIG=<name>` builds firmware from
`configs/<platform>/<name>.cfg` into `build/<platform>/<name>/`.

| Platform | Output | Machine |
|---|---|---|
| `x86-pc` | `squirrel-bios.rom` | QEMU PC (`-bios`) |
| `aarch64-virt` | `squirrel-bios.bin` | QEMU AArch64 `virt` (`-bios`) |
| `riscv64-virt` | `squirrel-bios.bin` | QEMU RISC-V `virt` (`-bios`) |
| `x86_64-uefi` | `squirrel-bios.fd` | QEMU q35 or PC, 64-bit (`qemu-system-x86_64 -bios`) |
| `aarch64-uefi` | `squirrel-bios.fd` | QEMU AArch64 `virt`, UEFI-class (`-bios`); Raspberry Pi 3/4 as `kernel8.img` (`rpi3`, `rpi4`) |
| `riscv64-uefi` | `squirrel-bios.bin` | QEMU RISC-V `virt`, UEFI-class on the SBI (`-bios`) |
| `ppc64le-powernv` | `squirrel-bios.bin` | QEMU `powernv9` / `powernv10` (`qemu-system-ppc64 -bios`) |
| `sparc64-sun4u` | `squirrel-bios.bin` | QEMU `sun4u` (`qemu-system-sparc64 -bios`) |

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
| `CONFIG_NVME` | `y` | Namespace 1 of an NVMe controller as `DL = 80h` when no IDE or SATA disk answered: boot and `INT 13h` (`x86/drivers/nvme/`). Needs `CONFIG_PCI` |
| `CONFIG_VIRTIO` | `y` | A virtio-blk device (legacy interface) as `DL = 80h` when no IDE, SATA or NVMe disk answered: boot and `INT 13h` (`x86/drivers/virtio/`). Needs `CONFIG_PCI` |
| `CONFIG_NETBOOT` | `y` | When no disk or CD booted, DHCP and a TFTP download of the boot file to `0x7C00` over a virtio-net device (`x86/boot/netboot.inc`, `x86/drivers/virtio/net.inc`). Needs `CONFIG_VIRTIO` |
| `CONFIG_VBE` | `y` | VBE 3.0 (`INT 10h` `AH=4Fh`): 16- and 32-bit linear-framebuffer modes up to 1280x1024 on QEMU's standard VGA (`x86/drivers/vga/vbe.inc`). Needs `CONFIG_PCI` |
| `CONFIG_USB` | `y` | List the USB devices at POST: EHCI, UHCI, OHCI and xHCI controllers, enumeration and hubs (`x86/drivers/usb/`). Needs `CONFIG_PCI` |
| `CONFIG_PCI` | `y` | PCI bus 0 enumeration, BAR placement, PIIX3 IRQ routing, PCI BIOS `INT 1Ah` `AX=B1xx` and `$PIR` (`x86/drivers/pci/`) |
| `CONFIG_FLOPPY` | `y` | 1.44 MB diskettes: FDC driver, `INT 13h` `DL = 0x00`/`0x01`, `INT 1Eh`, boot from A: (`x86/drivers/fdc/`, `x86/boot/floppy.inc`) |

`x86-pc/minimal.cfg` turns the four modules off. `make test` boots it
(`config-x86-minimal`) and checks that it really has no ACPI tables
(`config-x86-minimal-noacpi`). The ROM stays 64 KB; the space is padding.

### x86_64-uefi

| Key | Default | Meaning |
|---|---|---|
| `CONFIG_COM1_BASE` | `0x3F8` | I/O port of the serial console |
| `CONFIG_X2APIC` | `n` | Put every local APIC in x2APIC mode when the CPU has it; with `n` only when more than 255 CPUs are present (`x86_64/smp/`) |
| `CONFIG_SECURE_BOOT` | `y` | Verify-only Secure Boot, on when a db comes through fw_cfg (`x86_64/secure/`) |
| `CONFIG_COREBOOT` | `n` | Prefer coreboot LBIO tables for the memory map and ACPI RSDP instead of fw_cfg (`x86_64/platform/coreboot.c`); for running as a coreboot payload |

`x86_64-uefi/x2apic.cfg` turns `CONFIG_X2APIC` on; `make test` boots it
with four CPUs (`x86_64-x2apic-cfg`). `x86_64-uefi/coreboot.cfg` turns
`CONFIG_COREBOOT` on (and Secure Boot off); `make test` feeds it synthetic
LBIO tables (`x86_64-coreboot`). Board notes:
[boards/ga-g41m-es2l](../boards/ga-g41m-es2l/README.md).

The C sources include `config.h` when built with `-DSQ_CONFIG`.

### aarch64-virt

| Key | Default | Meaning |
|---|---|---|
| `CONFIG_UART_BASE` | `0x09000000` | PL011 console |
| `CONFIG_FWCFG_BASE` | `0x09020000` | QEMU fw_cfg MMIO |

### aarch64-uefi

| Key | Default | Meaning |
|---|---|---|
| `CONFIG_UART_BASE` | `0x09000000` | PL011 console until the device tree is read (its `arm,pl011` node wins) |
| `CONFIG_FWCFG_BASE` | `0x09020000` | QEMU fw_cfg MMIO when the device tree has no `qemu,fw-cfg-mmio` node; `0` for none |
| `CONFIG_FW_BASE` | `0x40100000` | Link (and run) address; a boot loader that loads the image there (the Raspberry Pi's, at `0x80000`) runs it in place with the device tree in `x0` |

`rpi4.cfg` and `rpi3.cfg` build the Raspberry Pi images
([boards/rpi4](../boards/rpi4/README.md)); `make test` runs `rpi3` on
QEMU's `raspi3b`, and `rpi4` on `raspi4b` when QEMU ≥ 9.0 provides that
machine (otherwise the `rpi4-uefi-sd` case is skipped).

The C sources include `config.h` when built with `-DSQ_CONFIG`.

### riscv64-uefi

| Key | Default | Meaning |
|---|---|---|
| `CONFIG_UART_BASE` | `0x10000000` | NS16550A for the SBI, and for the UEFI core until the device tree is read |
| `CONFIG_MAX_HARTS` | `512` | Hart IDs the SBI tracks |
| `CONFIG_FWCFG_BASE` | `0x10100000` | QEMU fw_cfg MMIO when the device tree has no `qemu,fw-cfg-mmio` node |

### riscv64-virt

| Key | Default | Meaning |
|---|---|---|
| `CONFIG_UART_BASE` | `0x10000000` | NS16550A console when the device tree has no `ns16550a` node |
| `CONFIG_CLINT_BASE` | `0x02000000` | CLINT (msip, mtimecmp at +0x4000, mtime at +0xbff8) when the device tree has no `riscv,clint0`, `sifive,clint0` or ACLINT nodes |
| `CONFIG_MAX_HARTS` | `512` | Hart IDs the SBI tracks; higher IDs park |

`riscv64-virt/small.cfg` (8 harts) shows a non-default build; `make test`
boots it as `config-riscv-small`. `riscv64-virt/dt.cfg` points both
addresses at nothing, so the device tree has to supply them; `make test`
boots it as `config-riscv-dt-*`.

### ppc64le-powernv

| Key | Default | Meaning |
|---|---|---|
| `CONFIG_MAX_CPUS` | `64` | Hardware threads the firmware parks and can start; the rest stay in the reset loop |
| `CONFIG_KERNEL_BASE` | `0x20000000` | Where QEMU's `-kernel` puts the image |
| `CONFIG_KERNEL_MAX` | `0x08000000` | Largest kernel file looked at there |

The C sources include `config.h` when built with `-DSQ_CONFIG`.
`make test` boots the default build as `config-ppc64-default`.

### sparc64-sun4u

| Key | Default | Meaning |
|---|---|---|
| `CONFIG_IDENTITY_MB` | `32` | RAM mapped 1:1 for the OS in locked 4 MB TLB entries (also where `claim` allocates) |

`make test` boots the default build as `config-sparc64-default`.
