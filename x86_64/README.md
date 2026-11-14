# x86-64 SquirrelBIOS (`PLATFORM=x86_64-uefi`)

Roadmap C1: a UEFI-class firmware for QEMU's 64-bit PC machines, built
as a 1 MB flash image (`squirrel-bios.fd`, for
`qemu-system-x86_64 -bios`). Original code: freestanding C (gcc) and
NASM. The legacy 16-bit BIOS in `x86/` is separate and unchanged.

The UEFI services (memory, protocols, images, events, the text console,
tables, runtime services, Block I/O, GPT and FAT) are the shared core in
`common/efi`, built with `include/arch.h` (roadmap D4).

```
reset/     reset.asm: the last 4 KB of flash. Reset vector, 32-bit
           protected mode, copies the body to RAM at FW_BASE (2 MB),
           maps the first 4 GB 1:1 with 2 MB pages (tables at
           0x1000-0x6FFF), enters long mode, jumps to the body
core/      start.asm (clears .bss, calls fw_main), main.c, arch.c (the
           shared core's architecture interface: COM1, the TSC measured
           against the PIT, the firmware's memory, PE entry calls with
           the Microsoft x64 convention, Secure Boot's image check),
           rt.c (its runtime part: the CMOS clock, ResetSystem),
           variables.c (the variable services and their flash store),
           gop.c (Graphics Output Protocol on Bochs DISPI), acpi.c
           (ACPI 6.x tables, the DSDT through common/efi/aml.c),
           smbios.c (SMBIOS 3.x), bootopt.c (command line and initrd
           for the booted image)
platform/  fwcfg.c (QEMU fw_cfg over I/O ports), memmap.c (etc/e820,
           CMOS as a fallback), pci.c (config space, I/O and memory
           BAR assignment below 4 GB), virtio_blk.c (legacy
           virtio-blk), pcidisk.c (NVMe, AHCI, modern virtio-blk and
           xHCI USB disks through common/efi's drivers),
           chipset.c (PIIX4 / ICH9 PM block, q35 ECAM, PCI interrupt
           routing)
csm/       csm.c, csm.asm: the CSM (roadmap C3), the legacy x86-pc ROM
           carried in the image and the way back to real mode
smp/       smp.c (AP bring-up with INIT-SIPI-SIPI, AP parking, the MP
           services protocol), apic.c (local APIC in xAPIC or x2APIC
           mode), mptable.c (MPS 1.4 table), roadmap C4
smm/       smm.c (SMM in q35's TSEG, SMBASE relocation, the EINJ table
           and the SMI handler that serves it), roadmap F2
acpi/      numa.c (SRAT and SLIT from QEMU -numa), ras.c (BERT and
           HEST stubs), cxl.c (CEDT CHBS/CFMWS, ACPI0016/ACPI0017 on
           q35 with cxl=on), roadmap F1/F2; the other ACPI tables are
           in core/
secure/    secure.c (Secure Boot policy, db/dbx, the Authenticode PE
           hash), authvar.c (authenticated writes to PK, KEK, db and
           dbx; setup and user mode), sha256.c, sha1.c and sha512.c
           (SHA-384/512, for TPM PCR banks), rsa.c (RSASSA-PKCS1-v1_5
           verify), der.c (X.509 keys, Authenticode and detached
           PKCS#7), roadmap C4/C5
tpm/       tis.c (a TPM 2.0 behind the TIS/FIFO interface), tcg2.c
           (PCR extends, the event log, EFI_TCG2_PROTOCOL, the TPM2
           table), roadmap C5
mgmt/      ipmi.c (a BMC on IPMI KCS, the SPMI table), redfish.c (the
           Redfish host interface record), roadmap F3
include/   arch.h (what the shared core needs to know about x86-64, and
           the x86-only helpers), acpi.h; sq.h and efi.h are
           common/efi's
link.ld    the body, linked at FW_BASE; common/efi/runtime.c, lib.c,
           core/rt.c, core/variables.c and secure/{authvar,der,rsa,sha256}.c
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

Disk: one transitional virtio-blk function (legacy I/O BAR, assigned
from `0xC000`), then every NVMe namespace 1, AHCI SATA disk, modern
virtio-blk and USB bulk-only disk on an xHCI root port, on any bus,
through the shared drivers. GPT ESP partitions and
MBR type `0xEF`/`0x0C` get child Block I/O handles; a whole-disk FAT
volume is also tried. The Simple File System matches 8.3 and long
names, and can write (common/efi/fat.c). A file read takes whole
clusters that follow each other on disk in one request (up to 1 MB),
and the legacy virtio-blk driver waits for each request by the clock
(30 s), not a count of polls: under QEMU's TCG a request takes about
10 ms however small, so a 3 MB loader on an ESP of 512-byte clusters
once took a minute, or gave up and fell back to the CSM
(`x86_64-bigload`).

Memory: allocations come from the top of the highest free range below
4 GB (the limit of the 1:1 mapping). The UEFI memory map keeps page 0 and
the page tables (0x0000-0x6FFF), the stack (1-2 MB) and the firmware
image (from 2 MB) as boot services memory, and 0xA0000-0xFFFFF as
reserved. Services not written yet answer `EFI_UNSUPPORTED`.

GOP: QEMU std VGA (PCI `1234:1111`) through Bochs DISPI (`0x01CE`/
`0x01CF`) and the linear frame buffer in BAR0. Modes 800×600, 640×480
and 1024×768 at 32 bpp (BGRX); `QueryMode`/`SetMode`/`Blt` (fill,
buffer↔video, video↔video). Every mode set also sets the VGA
miscellaneous output register's colour bit and the attribute
controller's palette address source bit, without which the adapter
shows nothing, and clears the frame buffer. Runtime: `GetTime` from the CMOS RTC,
the variable services (below), `ResetSystem` (ACPI
shutdown or keyboard-controller reset), and `ExitBootServices` (signals
exit-boot events, nulls `BootServices`, reseals the system table). At
ExitBootServices, and before the CSM, the PC is left as a BIOS leaves
it for an OS that drives the 8259s: the PICs at vectors 08h/70h, all
masked but the cascade; the BSP's local APIC in virtual wire mode
(LINT0 ExtINT, LINT1 NMI, timer and error masked); the 8042 with its
keyboard interrupt and set 1 translation on (`x86_64-handoff`). The
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
that is given but does not parse refuses every image (fail closed).
These fixed keys cannot be changed (`EFI_WRITE_PROTECTED`), and the
CSM stays off.

Enrolled keys (C5): without a fixed db, `PK`, `KEK`, `db` and `dbx` are
variables (kept in the flash store when there is one) that change only
through time-based authenticated writes: attributes NV, BS, RT and
`TIME_BASED_AUTHENTICATED_WRITE_ACCESS` (plus `APPEND_WRITE` except for
the PK), the data led by an `EFI_VARIABLE_AUTHENTICATION_2` whose
detached PKCS#7 signs the name, vendor GUID, attributes, timestamp and
data. With no PK the machine is in setup mode (`SetupMode` 1,
`SecureBoot` 0, every image runs) and any well-formed write is taken.
With a PK it is in user mode and Secure Boot is on: a PK or KEK write
must be signed by the PK, a db or dbx write by a KEK or the PK, a
replacing write must be newer than the variable, and an empty PK signed
by the PK goes back to setup mode. The signer is matched by its
certificate's key (X.509 or RSA-2048 entries). The same rules hold at
run time, after ExitBootServices. For tests and first enrollment the
fw_cfg files `opt/org.squirrelworks/enroll/{db,dbx,KEK,PK}` (the bytes
an OS tool passes to `SetVariable`)
are written at boot in that order. `tests/mksigned.py` makes the
`auth-*.auth` test files. `tests/mksigned.py` signs test images with two RSA-2048
test keys made from fixed seeds; `osslsigncode verify` accepts its
signatures and the firmware accepts `osslsigncode sign` output.

Measured boot (C5): a TPM 2.0 on the TIS/FIFO interface at 0xFED40000
(QEMU `-device tpm-tis` with swtpm) gets TPM2_Startup(CLEAR); the
firmware then extends every active PCR bank it can hash (SHA-1,
SHA-256, SHA-384, SHA-512; an active SM3 bank is reported and left
alone) and logs each event in the TCG2 crypto-agile format: PCR 0 the
CRTM version and the 1 MB flash image, PCR 7 the SecureBoot, PK, KEK,
db and dbx variables, PCR 4 the boot action and the Authenticode hash
of every image LoadImage accepts, PCRs 0-7 a separator before the first
image, and PCR 5 ExitBootServices. `EFI_TCG2_PROTOCOL` has
GetCapability, GetEventLog (the TCG2 format only), HashLogExtendEvent
(plain data or a PE image), SubmitCommand and GetActivePcrBanks;
changing the banks is unsupported. Events after GetEventLog also go in
the final events table. The log (64 KB, ACPI NVS) is in the TPM2 table
(revision 4, start method 6), and the DSDT has an `MSFT0101` device for
the OS's TIS driver. The CRB interface is not supported. `tests/
tcglog.py` replays a log against PCRs 0-7; `LINUX_TPM=1 make
linux-uefi` checks that Linux finds the TPM and that its
binary_bios_measurements replay. With no TPM the firmware says
`tpm: none` and installs no protocol.

```
mkdir -p tpm && swtpm_setup --tpm2 --tpmstate tpm --pcr-banks sha256
swtpm socket --tpm2 --tpmstate dir=tpm --ctrl type=unixio,path=tpm/sock --daemon
qemu-system-x86_64 ... -chardev socket,id=chrtpm,path=tpm/sock \
    -tpmdev emulator,id=tpm0,chardev=chrtpm -device tpm-tis,tpmdev=tpm0
```

NUMA (F1/F2): with QEMU `-numa`, fw_cfg item 0x0D gives the node of each
APIC ID and each node's memory; the SRAT gets a processor affinity entry
per CPU the SMP code brought up (x2APIC affinity past ID 254) and memory
affinity entries laid over guest RAM in node order, split around the PCI
hole. The SLIT is 10/20 unless `opt/org.squirrelworks/numa-distance`
gives the N x N bytes; else QEMU's own SLIT (`-numa dist`), read from
the tables it offers in fw_cfg `etc/acpi/tables`. The HMAT is QEMU's
own too when it has one (`-machine hmat=on`, `-numa hmat-lb` and
`hmat-cache`: latency, bandwidth, memory-side caches); else it names the
initiator of each node with memory, its own CPUs or the nearest node
with CPUs by the SLIT. The
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
The OS owns the banks (no firmware first).

SMM and EINJ (F2, q35 only): the top 1 MB of RAM below 4 GB is TSEG
(reserved in the memory map; `T_EN` in the MCH's ESMRAMC). It holds a
GDT, one 32-bit SMI handler, a 512-byte stack per CPU and, 8 KB apart,
each CPU's SMBASE with an entry stub that enters protected mode and
jumps to the handler. At boot every CPU takes one SMI at the default
SMBASE (0x30000, saved and put back) whose stub moves its SMBASE into
TSEG; the firmware then checks that TSEG reads back closed and that the
handler answers a ping, or turns SMM off again. SMIs come only from
writes to the APM port 0xB2 (`APMC_EN`); `D_LCK` locks the SMRAM
controls before anything but the firmware runs: the first image given
to `LoadImage` (ESP, fw_cfg or network), ExitBootServices or the CSM,
whichever comes first. Before the CSM's legacy boot the SMIs are turned
off and CMOS 34h/35h are lowered so the legacy RAM size ends at TSEG. Up to 112 CPUs. The EINJ table drives a
mailbox page: `SET_ERROR_TYPE` writes the type, `EXECUTE_OPERATION`
writes `0xE1` to port 0xB2, and the handler, for a correctable memory
error (the one type `GET_ERROR_TYPE` offers), copies a CPER record
built at boot from TSEG into the error status block of a polled (500
ms) generic hardware error source in the HEST, where the OS reads and
clears it; other types fail. The handler reads nothing from outside
SMRAM but the requested type. `x86_64-einj` runs the EINJ actions as an
OS would, on the BSP and an AP; `LINUX_EINJ=1 make linux-uefi` loads the
kernel's einj module and injects through debugfs from two CPUs. pc
(i440fx) has no TSEG, so no SMM and no EINJ.

Management (F3): if a BMC answers Get Device ID on the IPMI KCS ports
(0xCA2 data, 0xCA3 command/status, as QEMU's `isa-ipmi-kcs` with
`ipmi-bmc-sim` gives), the firmware prints what it is and describes it
twice for the OS: an SPMI table (KCS, byte-wide system I/O, polled) and
SMBIOS type 38. With a BMC and a network controller on bus 0, SMBIOS
type 42 adds a Redfish host interface stub (DSP0270): a network host
interface on that PCI NIC with one Redfish over IP record, static
link-local addresses (host 169.254.0.2, service 169.254.0.1:443,
hostname `squirrel-bmc`) and the BMC's device GUID as the service UUID.
There is no Redfish service behind it in QEMU. `x86_64-ipmi` reads the
three records and talks KCS to the BMC itself; `LINUX_IPMI=1 make
linux-uefi` loads the kernel's ipmi_si, which finds the BMC through
SMBIOS type 38.

Variables (C5): the variable services keep every variable in a RAM
arena and the non-volatile ones also in a flash store on QEMU's second
pflash unit, 256 KB mapped just below the firmware (0xFFEC0000):

```
-drive if=pflash,format=raw,unit=0,readonly=on,file=x86_64/squirrel-bios.fd \
-drive if=pflash,format=raw,unit=1,file=vars.fd    # truncate -s 256K vars.fd
```

The firmware finds it with the CFI query and writes it with the Intel
command set (block erase, buffered program). The store has two banks;
each record is appended and marked valid only once written whole (a
reset mid-write loses only that record), a replaced or deleted record is marked dead,
and a full bank is compacted into the other under a higher generation.
A variable can be up to 32 KB. Without the second unit (`-bios`) the
firmware says so and variables last until reset. The store is runtime
MMIO in the memory map, so an OS keeps writing it after
ExitBootServices (`LINUX_NV=1 make linux-uefi` checks efivarfs across
two boots).

Runtime pages: only `runtime.c`, `lib.c`, `rt.c`, `variables.c`, the
Secure Boot code that checks authenticated writes, and the system,
runtime and configuration tables stay the OS's after ExitBootServices (link.ld puts
them in `.rt.*` pages); the rest of the image is boot services code.
`x86_64-rt-after-exit` overwrites every boot services page after
ExitBootServices, on its own page tables and stack, then calls GetTime,
the variable services and ResetSystem.

Extra PCI root buses (F1): QEMU's `pxb` (pc) and `pxb-pcie` (q35)
expanders add root buses that only ACPI describes. fw_cfg
`etc/extra-pci-roots` counts them, and their bus numbers are the
populated buses past 0 that no bridge leads to. Each becomes a DSDT host
bridge `PCxx` with `_BBN`, the buses up to the next root, and the first
six get a 4 KB I/O window from 0x6000 and a 64 MB memory window from
0xE0000000, cut out of PCI0's. An expander given a `numa_node` gets it as `_PXM`, read from QEMU's
own DSDT in fw_cfg `etc/acpi/tables`. On pc their `_PRT` follows QEMU's pxb
routing; on q35 devices behind an expander use MSI. All roots share ECAM
segment 0 (QEMU x86 has one). `LINUX_PXB=1 make linux-uefi` adds one
expander at bus 0x40 with a device behind it. The ACPI table area grows
with the CPU count, for the MADT and SRAT entries of up to 1024 CPUs
(SMP_MAX_CPUS); QEMU's TCG has no x2APIC, so more than 255 CPUs are not
tested under QEMU. The host-side cases `x86_64-tables-1024cpu` and
`x86_64-tables-2seg` (`tests/x86_64-tables`, `tests/acpicheck.py`) feed a
synthetic topology into the same MADT/SRAT/MCFG builders and check 1024
x2APIC entries, SRAT affinity per node, and two ECAM segments.

CXL (F2): with `-M q35,cxl=on`, a `pxb-cxl` host bridge (PCI `1b36:000b`),
`cxl-rp`, `cxl-type3` and `-M cxl-fmw.0...`, the firmware finds the bridge,
takes QEMU's CEDT from fw_cfg `etc/acpi/tables` (CHBS for the CHBCR MMIO,
CFMWS for each fixed window), republishes it, gives each CXL root
`ACPI0016` (with `_CID` PNP0A08 and the CHBCR in `_CRS`), and adds an
`ACPI0017` device so Linux's `cxl_acpi` can bind. `x86_64-cxl` checks the
CEDT ranges; without `cxl=on` there is no CEDT.
