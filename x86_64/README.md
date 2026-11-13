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
           image.c (PE32+ loader, LoadImage/StartImage/Exit/UnloadImage),
           event.c (events, timers, Stall; the TSC measured against the
           PIT), textcon.c (Simple Text Input/Output on COM1),
           devicepath.c, disk.c (Block I/O, GPT/MBR), fat.c (Simple File
           System), gop.c (Graphics Output Protocol on Bochs DISPI),
           runtime.c (GetTime, variables, ResetSystem, ExitBootServices),
           acpi.c (ACPI 6.x tables), aml.c (the AML writer the DSDT is
           generated with), smbios.c (SMBIOS 3.x), bootopt.c (command
           line and initrd for the booted image)
platform/  fwcfg.c (QEMU fw_cfg over I/O ports), memmap.c (etc/e820,
           CMOS as a fallback), pci.c (config space, I/O and 32-bit
           memory BAR assignment), virtio_blk.c (legacy virtio-blk),
           chipset.c (PIIX4 / ICH9 PM block, q35 ECAM, PCI interrupt
           routing)
csm/       csm.c, csm.asm: the CSM (roadmap C3), the legacy x86-pc ROM
           carried in the image and the way back to real mode
include/   sq.h, efi.h (UEFI types and tables, from the specification),
           acpi.h
link.ld    the body, linked at FW_BASE
```

Flash layout: the body from offset 0, zero padding, the stub in the
last 4 KB. The build fails if the body would reach the stub.

Boot order: after the EFI tables and console are up, discover a
virtio-blk disk (legacy I/O BAR; QEMU q35 needs
`disable-legacy=off,disable-modern=on`), publish Block I/O and a device
path, walk GPT (or MBR) partitions, mount FAT12/16/32 volumes, and start
`\EFI\BOOT\BOOTX64.EFI` when present. If nothing on disk boots, fall
back to an EFI image given as
`-fw_cfg name=opt/org.squirrelworks/boot.efi,file=...`.

Images: PE32+ for x64, EFI applications and boot or runtime drivers.
An image goes to its preferred base when that is free and below 4 GB,
anywhere else otherwise, with its base relocations applied. Applications
are unloaded when they return or call `Exit`. Protocols: install,
reinstall, uninstall, handle/open/close, locate (handle, handle buffer,
protocol), protocols per handle, and the multiple-interface calls, with
no notifications. `InstallConfigurationTable` keeps up to 32 entries.

Events: interrupts stay off, so timers are polled by `CheckEvent`,
`WaitForEvent` and `Stall`. Notify functions run when the event is
signalled (`NOTIFY_SIGNAL`) or checked (`NOTIFY_WAIT`), not queued by
TPL. Event groups (`CreateEventEx`) are signalled together.

Console: ConIn, ConOut and StdErr are COM1. Output is UTF-8 with ANSI
escape sequences for attributes, clearing and the cursor, in one 80x25
mode. Input turns terminal escape sequences into UEFI scan codes
(arrows, Home/End, Insert/Delete, Page Up/Down, F1-F12, Esc).

Disk: one transitional virtio-blk function; PCI BARs that QEMU left
floating are assigned from `0xC000`. GPT ESP partitions and MBR type
`0xEF`/`0x0C` get child Block I/O handles; a whole-disk FAT volume is
also tried. The Simple File System is read-only and matches 8.3 names
(enough for `\EFI\BOOT\BOOTX64.EFI`).

Memory: allocations come from the top of the highest free range below
4 GB (the limit of the 1:1 mapping). The UEFI memory map keeps page 0 and
the page tables (0x0000-0x6FFF), the stack (1-2 MB) and the firmware
image (from 2 MB) as boot services memory, and 0xA0000-0xFFFFF as
reserved. Services not written yet answer `EFI_UNSUPPORTED`.

GOP: QEMU std VGA (PCI `1234:1111`) through Bochs DISPI (`0x01CE`/
`0x01CF`) and the linear frame buffer in BAR0. Modes 800×600, 640×480
and 1024×768 at 32 bpp (BGRX); `QueryMode`/`SetMode`/`Blt` (fill,
buffer↔video, video↔video). Runtime: `GetTime` from the CMOS RTC,
in-RAM `Get`/`SetVariable` (no flash store), `ResetSystem` (ACPI
shutdown or keyboard-controller reset), and `ExitBootServices` (signals
exit-boot events, nulls `BootServices`, reseals the system table). The
1:1 map stays; `SetVirtualAddressMap` is a no-op after exit.

           memory BAR assignment), virtio_blk.c (legacy virtio-blk),
           chipset.c (PIIX4 / ICH9 PM block, q35 ECAM, PCI interrupt
           routing)
csm/       csm.c, csm.asm: the CSM (roadmap C3), the legacy x86-pc ROM
           carried in the image and the way back to real mode
include/   sq.h, efi.h (UEFI types and tables, from the specification),
           acpi.h
