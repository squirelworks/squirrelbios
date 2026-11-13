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
smp/       smp.c (AP bring-up with INIT-SIPI-SIPI, AP parking, the MP
           services protocol), apic.c (local APIC in xAPIC or x2APIC
           mode), mptable.c (MPS 1.4 table), roadmap C4
acpi/      numa.c (SRAT and SLIT from QEMU -numa), ras.c (BERT and
           HEST stubs), roadmap F1/F2; the other ACPI tables are in core/
secure/    secure.c (Secure Boot policy, db/dbx, the Authenticode PE
           hash), sha256.c, rsa.c (RSASSA-PKCS1-v1_5 verify), der.c
           (X.509 keys, Authenticode PKCS#7), roadmap C4
include/   sq.h, efi.h (UEFI types and tables, from the specification),
           acpi.h
link.ld    the body, linked at FW_BASE; core/runtime.c and core/lib.c
           (built apart, sections renamed .rt.*) go in page-aligned
           runtime code and data, the only part of the image the OS
           keeps after ExitBootServices, with the system, runtime
           services and configuration tables
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

CSM (roadmap C3): when no `BOOTX64.EFI` and no fw_cfg image boots, the
firmware starts the 16-bit x86-pc SquirrelBIOS (`x86/squirrel-bios.rom`,
built into the flash image by `x86_64/csm/csm.asm`). It opens shadow RAM
at E0000-FFFFF through the host bridge's PAM registers (i440FX `0x59`,
Q35 `0x90`), clears E0000-EFFFF, copies the ROM to F0000 and makes that
segment read-only, turns Bochs DISPI off so VGA text mode works again,
then copies a trampoline to 0x8000 that leaves long mode (compatibility
mode, paging and EFER.LME off, a 16-bit segment, real mode) and jumps
to F000:FFF0. The legacy BIOS then runs its whole POST and boots a
floppy, an MBR disk (IDE, AHCI, NVMe, USB, virtio), a CD or the network,
exactly as under `qemu-system-i386`. Nothing of the UEFI side survives
the jump. Squirrel-OS/2's `os.img` and `floppy.img` boot this way:

```
make os-boot-csm OS_IMG=path/to/os.img
OS_DRIVE=floppy make os-boot-csm OS_IMG=path/to/floppy.img
```

SMP (C4): after GOP, the BSP copies a trampoline into the highest free
page below 640 KB and broadcasts INIT, STARTUP, STARTUP. Each AP goes
from real mode to long mode on the BSP's page tables, takes a slot and
a 16 KB stack and parks, spinning on its mailbox. The firmware waits
until as many CPUs as fw_cfg reports (`FW_CFG_NB_CPUS`) are up, or 2 s.
Processor numbers: 0 is the BSP, the APs follow in APIC ID order;
package, core and thread come from CPUID leaf 0Bh. The MP services
protocol has `GetNumberOfProcessors`, `GetProcessorInfo`,
`StartupAllAPs` (together or one by one), `StartupThisAP`,
`EnableDisableAP` and `WhoAmI`; only blocking calls (`WaitEvent` NULL),
and an AP that runs past the timeout is INIT'ed and started again in
its slot. `SwitchBSP` answers `EFI_UNSUPPORTED`. The local APICs go to
x2APIC mode when the CPU has it and more than 255 CPUs are present, or
with `CONFIG_X2APIC=y`. The MPS 1.4 table (processors with APIC IDs up
to 254, the ISA bus, the I/O APIC, ISA IRQs and LINT0/1) is the MPS EFI
configuration table. At ExitBootServices, and before the CSM leaves
long mode, every AP gets an INIT and waits for the OS's STARTUP.

Secure Boot (C4, verify-only): the keys are ours and come from fw_cfg,
`-fw_cfg name=opt/org.squirrelworks/db,file=...` (and `.../dbx`), each a
list of EFI_SIGNATURE_LISTs with SHA-256 hashes, RSA-2048 keys or X.509
certificates. With a db, Secure Boot is on: `SecureBoot` reads 1 and
`SetupMode` 0, and `LoadImage` (fw_cfg images and `BOOTX64.EFI` alike)
answers `EFI_SECURITY_VIOLATION` unless the image's Authenticode SHA-256
is in db, or one of its PKCS#7 signatures (SHA-256 with RSA) is verified
by a db key; a hash or a signing key in dbx always refuses it. The key
must be in db itself: certificate chains are not walked. A db or dbx
that is given but does not parse refuses every image (fail closed). The databases
cannot be changed at run time (no PK/KEK authenticated writes), and the
CSM stays off. `tests/mksigned.py` signs test images with two RSA-2048
test keys made from fixed seeds; `osslsigncode verify` accepts its
signatures and the firmware accepts `osslsigncode sign` output.

NUMA (F1/F2): with QEMU `-numa`, fw_cfg item 0x0D gives the node of each
APIC ID and each node's memory; the SRAT gets a processor affinity entry
per CPU the SMP code brought up (x2APIC affinity past ID 254) and memory
affinity entries laid over guest RAM in node order, split around the PCI
hole. The SLIT is 10/20 unless `opt/org.squirrelworks/numa-distance`
gives the N x N bytes (QEMU keeps `-numa dist` to its own ACPI). The
MADT lists the same CPUs in processor-number order, with x2APIC entries
past APIC ID 254. `LINUX_SMP=4 LINUX_NUMA=2 make linux-uefi` boots
Linux on two nodes.

Machine checks (F2): at boot every CPU reads its MCA banks. A valid
MCi_STATUS is an error left from before the reset; it becomes a CPER
IA32/X64 processor error section (APIC ID, CPUID, the bank's CTL,
STATUS, ADDR and MISC) in the BERT's boot error region, which has
reserved pages of its own so the OS can map it. The banks are then
cleared and enabled and CR4.MCE is set, so a fatal machine check resets
the machine and shows up on the next boot (`x86_64-mce` injects one with
the QEMU monitor; Linux prints it as "BERT: Error records from previous
boot"). The HEST has the IA-32 machine check source and the corrected
machine check source, with every bank: CMCI when MCG_CAP has it, else
polled every second, 10 errors a minute before switching to polling.
The OS owns the banks (no firmware first) and there is no EINJ.

Runtime pages: only `runtime.c`, `lib.c` and the system, runtime and
configuration tables stay the OS's after ExitBootServices (link.ld puts
them in `.rt.*` pages); the rest of the image is boot services code.
`x86_64-rt-after-exit` overwrites every boot services page after
ExitBootServices, on its own page tables and stack, then calls GetTime,
the variable services and ResetSystem.
