# x86-64 SquirrelBIOS (`PLATFORM=x86_64-uefi`)

Roadmap C1: a UEFI-class firmware for QEMU's 64-bit PC machines, built
as a 1 MB flash image (`squirrel-bios.fd`, for
`qemu-system-x86_64 -bios`). Original code: freestanding C (gcc) and
NASM. The legacy 16-bit BIOS in `x86/` is separate and unchanged.

```
reset/     reset.asm: the last 4 KB of flash. Reset vector, 32-bit
           protected mode, copies the body to RAM at FW_BASE (2 MB),
           maps the first 4 GB 1:1 with 2 MB pages (tables at
           0x1000-0x6FFF), enters long mode, jumps to the body
core/      start.asm (clears .bss, calls fw_main), main.c, console.c
           (COM1, printf), lib.c, tables.c (EFI system, boot and
           runtime services tables), memory.c (UEFI memory map,
           AllocatePages/FreePages, AllocatePool/FreePool, GetMemoryMap),
           protocol.c (handles, protocols, configuration tables),
           image.c (PE32+ loader, LoadImage/StartImage/Exit/UnloadImage)
platform/  fwcfg.c (QEMU fw_cfg over I/O ports), memmap.c (etc/e820,
           CMOS as a fallback)
include/   sq.h, efi.h (UEFI types and tables, from the specification)
link.ld    the body, linked at FW_BASE
```

Flash layout: the body from offset 0, zero padding, the stub in the
last 4 KB. The build fails if the body would reach the stub.

So far it reaches long mode, prints the CPU and the memory map on COM1,
builds the EFI system table and checks its memory services. Then it
loads and starts an EFI image given to QEMU as
`-fw_cfg name=opt/org.squirrelworks/boot.efi,file=...`, and halts when
the image returns. Consoles, FAT32 and GOP come next (TODO.md section 15).

Images: PE32+ for x64, EFI applications and boot or runtime drivers.
An image goes to its preferred base when that is free and below 4 GB,
anywhere else otherwise, with its base relocations applied. Applications
are unloaded when they return or call `Exit`. Protocols: install,
reinstall, uninstall, handle/open/close, locate (handle, handle buffer,
protocol), protocols per handle, and the multiple-interface calls, with
no notifications. `InstallConfigurationTable` keeps up to 32 entries.

Memory: allocations come from the top of the highest free range below
4 GB (the limit of the 1:1 mapping). The UEFI memory map keeps page 0 and
the page tables (0x0000-0x6FFF), the stack (1-2 MB) and the firmware
image (from 2 MB) as boot services memory, and 0xA0000-0xFFFFF as
reserved. Services not written yet answer `EFI_UNSUPPORTED`.
