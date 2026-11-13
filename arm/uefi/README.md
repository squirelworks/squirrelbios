# AArch64 UEFI-class SquirrelBIOS (`PLATFORM=aarch64-uefi`)

Roadmap D1: the shared UEFI core (`common/efi`) on QEMU's AArch64
`virt` machine. Original code: freestanding C and assembly.

```bash
make arm-uefi
qemu-system-aarch64 -M virt -cpu cortex-a57 -nographic -bios arm/uefi/squirrel-bios.fd \
    -kernel Image -initrd initrd.img -append "console=ttyAMA0"
qemu-system-aarch64 -M virt -cpu cortex-a57 -nographic -bios arm/uefi/squirrel-bios.fd \
    -fw_cfg name=opt/org.squirrelworks/boot.efi,file=app.efi
qemu-system-aarch64 -M virt -cpu cortex-a57 -nographic -bios arm/uefi/squirrel-bios.fd \
    -drive if=none,file=disk.img,format=raw,id=d0 -device virtio-blk-device,drive=d0
```

The last boots `\EFI\BOOT\BOOTAA64.EFI` from the disk's EFI system
partition (GPT or MBR, FAT12/16/32).

```
start.S   reset at 0 (flash): copy the image to RAM at 0x40100000 and
          jump there, clear .bss, stack, VBAR for EL1 or EL2, FP/SIMD on;
          the exception vectors (report ESR/ELR/FAR and halt); the
          image_call / image_exit thunk for StartImage and Exit
arch.c    PL011 console, generic timer (CNTVCT/CNTFRQ), PL031 RTC,
          PSCI (hvc or smc, as the device tree's /psci says) for
          ResetSystem, GICv2/v3 discovery and GICv3 set-up
          (distributor with affinity routing, every SPI non-secure
          group 1 and disabled, this CPU's redistributor awake, system
          register interface on), the identity map, cache maintenance
          for loaded images
acpi.c    the ACPI tables (the SBBR set) from the device tree: FACP
          (hardware-reduced, PSCI over hvc or smc), APIC (GICC per CPU,
          GICD, GICR or GICv2m frames, ITS), GTDT, SPCR and DBG2 for the
          PL011, MCFG and IORT for the PCIe host, and a DSDT with the
          CPUs, PL011, fw_cfg, virtio-mmio transports and the PCIe host
          bridge (windows, _PRT, ECAM reserved)
arch.h    ARCH_* constants for the core
link.ld   linked at FW_BASE (0x40100000), right after the 1 MB device
          tree QEMU puts at the start of RAM
```

- **Exception level.** EL1 (QEMU's default) or EL2
  (`-M virt,virtualization=on`), where the firmware runs in the EL2
  regime and hands EL2 to the OS (Linux reports `CPU: All CPU(s)
  started at EL2`). EL3 (`secure=on`) is reported and halts.
- **Memory.** An identity map of the first 1 TB with 1 GB blocks: Device
  memory below 1 GB and from 256 GB, Normal write-back for 1-256 GB, where
  `virt` puts RAM; caches on, as the UEFI AArch64 binding requires.
  Allocations stay below 512 GB.
- **Memory map.** RAM from the device tree. The device tree itself is
  boot services data; the whole firmware image is runtime services code
  (`EFI_MEMORY_RUNTIME`) and the PL031 page runtime MMIO, so `GetTime`,
  the variables and `ResetSystem` keep working after
  `SetVirtualAddressMap`, which converts the runtime services table,
  the system table's pointers and the RTC address.
- **Images.** PE32+ for AArch64 (`0xAA64`), placed on their section
  alignment (64 KB for Linux's `Image`).
- **ACPI.** The tables go next to the device tree. Linux keeps using the
  device tree unless told `acpi=force`; then it takes CPUs, the GIC,
  timers, console, PCIe (with MSIs through the ITS) and the virtio-mmio
  devices from ACPI.
- **Not yet:** GOP, SMBIOS, PPTT, and an `_OSC` for the PCIe host.

`make linux-arm` boots Ubuntu's arm64 kernel through its EFI stub to a
shell, and `make linux-arm-acpi` does the same with `acpi=force` on a
GICv3 machine with 4 CPUs: see the top-level README.
