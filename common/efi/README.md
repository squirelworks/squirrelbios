# Shared UEFI core (`common/efi`)

The UEFI-class firmware core for the device-tree machines, built once
per architecture. It started as a copy of `x86_64/core` (roadmap C1)
with the x86 parts moved behind a small architecture interface; roadmap
D4 folds `x86_64/core` onto it later. Original code.

```
include/sq.h    types, MMIO helpers, and the arch_* interface each
                architecture writes (console, counter, cache sync,
                RTC, reset, memory carve-outs, image_call/image_exit)
include/efi.h   UEFI types and tables (EFIAPI is the Microsoft ABI on
                x64 only)
main.c          efi_main: console, device tree, memory map, EFI tables,
                device tree configuration table, then boot: QEMU's
                -kernel/-initrd/-append from fw_cfg (LoadOptions, initrd
                through LoadFile2 on the Linux initrd media path), or
                opt/org.squirrelworks/boot.efi, or \EFI\BOOT\BOOT<arch>.EFI
                from a disk
virtio_mmio.c   virtio-blk on the virtio-mmio transports the device tree
                lists, version 1 (legacy) or 2 registers, polled
disk.c, fat.c   Block I/O for the disk and its GPT/MBR partitions, a
                read-only FAT12/16/32 Simple File System (8.3 and long
                names), and the ESP boot; copies of x86_64/core's, with
                the architecture's boot file name and long-name lookup
fdt.c           read-only flattened device tree walker
memmap.c        usable RAM from device_type "memory", /reserved-memory
fwcfg.c         QEMU fw_cfg, memory-mapped, with DMA for big reads
memory.c, protocol.c, image.c, event.c, textcon.c, tables.c,
runtime.c, devicepath.c, console.c, lib.c
                the services, as in x86_64/core; plus LocateDevicePath,
                PE section alignment (64 KB for arm64 Linux), runtime
                allocations marked EFI_MEMORY_RUNTIME, and
                SetVirtualAddressMap / ConvertPointer
```

An architecture provides `arch.h` (the `ARCH_*` constants listed in
`sq.h`) and the `arch_*` functions: see `arm/uefi/` (AArch64, at EL1/EL2
with an identity map) and `riscv/uefi/` (RISC-V, in S-mode on the SBI).
