# Shared UEFI core (`common/efi`)

The UEFI-class firmware core for the device-tree machines, built once
per architecture. It started as a copy of `x86_64/core` (roadmap C1)
with the x86 parts moved behind a small architecture interface; since
roadmap D4 x86-64 builds the services from here too (its `main.c`,
ACPI, SMBIOS, GOP, CSM and SMP stay in `x86_64/`). Original code.

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
virtio.c        virtqueue and virtio-blk requests for either transport
virtio_mmio.c   virtio-blk on the virtio-mmio transports the device tree
                lists, version 1 (legacy) or 2 registers, polled
pci.c           the device tree's ECAM host bridge (pci-host-ecam-generic):
                bus 0 walked, memory BARs placed in its windows, functions
                handed to the drivers below
virtio_pci.c    virtio-blk on PCI, the modern interface (capabilities)
nvme.c          NVMe namespace 1, one polled I/O queue, PRP lists
disk.c, fat.c   Block I/O for each disk and its GPT/MBR partitions, a
                read-only FAT12/16/32 Simple File System (8.3 and long
                names), and the ESP boot with the architecture's boot
                file name
fdt.c           read-only flattened device tree walker
acpi.c, aml.c   ACPI table store (headers, checksums, XSDT, RSDP as the
                ACPI 2.0 configuration table) and an AML writer (a copy
                of x86_64/core/aml.c plus string names, QWord memory,
                edge interrupts and translated I/O); the architecture
                builds the tables (arm/uefi/acpi.c)
memmap.c        usable RAM from device_type "memory", /reserved-memory
fwcfg.c         QEMU fw_cfg, memory-mapped, with DMA for big reads
memory.c, protocol.c, image.c, event.c, textcon.c, tables.c,
runtime.c, devicepath.c, console.c, lib.c
                the services: LocateDevicePath, PE section alignment
                (64 KB for arm64 Linux), runtime allocations marked
                EFI_MEMORY_RUNTIME, SetVirtualAddressMap / ConvertPointer
                (runtime.c and lib.c hold all that runs after
                ExitBootServices: x86-64 keeps only those pages)
```

An architecture provides `arch.h` (the `ARCH_*` constants listed in
`sq.h`) and the `arch_*` functions: see `arm/uefi/` (AArch64, at EL1/EL2
with an identity map), `riscv/uefi/` (RISC-V, in S-mode on the SBI) and
`x86_64/` (`include/arch.h`, `core/arch.c`, `core/rt.c`). Optional hooks
in `arch.h`: `ARCH_RT_DATA` (data the OS keeps), `arch_check_image`
(Secure Boot) and `arch_boot_options` (LoadOptions for a disk boot).
