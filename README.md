# SquirrelBIOS

Original firmware for Squirrel-OS/2 and QEMU's PC / AArch64 `virt` /
RISC-V `virt` machines. **v1.0.0** is a comprehensive x86 PC BIOS
(POST-shaped init, IVT services, ACPI/SMBIOS, bzImage, option ROMs) on
the v0.3.0 hand-off contract. See [ROADMAP.md](ROADMAP.md),
[TODO.md](TODO.md), and [release-notes/v1.0.0.md](release-notes/v1.0.0.md).

```
x86/        64 KB ROM — PC BIOS: ATA boot, INT 10h/13h/15h/16h/1Ah, ACPI, SMBIOS
            (core/, boot/, drivers/, tables/ modules)
x86_64/     1 MB flash image — x86-64 UEFI-class firmware in progress (roadmap C1):
            reset to long mode, EFI tables, memory, PE32+ images, events, serial
            console, virtio-blk Block I/O, FAT Simple File System, ESP boot, GOP,
            runtime services, ExitBootServices
arm/        AArch64 QEMU virt — PL011 banner + fw_cfg kernel load, DTB in x0;
            arm/uefi: UEFI-class firmware on the shared core (roadmap D1)
riscv/      RISC-V QEMU virt — UART banner + S-mode entry to -kernel, DTB in a1, SBI
tests/      stand-in OS images and smoke tests (make test)
common/     hand-off contract; common/efi: the UEFI core shared by the
            device-tree machines (from x86_64/core)
configs/    build configurations, one directory per platform (make PLATFORM= CONFIG=)
tools/      build helpers (mkconfig.sh: .cfg to config.inc / config.h)
release-notes/  tagged releases (v0.2.0, v0.3.0, v1.0.0, …)
ROADMAP.md  goals, principles, milestones
TODO.md     small tasks, one per PR
```

## Build

```bash
make          # all
make x86
make x86_64
make arm
make arm-uefi # arm/uefi/squirrel-bios.fd, AArch64 UEFI-class firmware
make riscv
make clean    # remove all build and test outputs

make PLATFORM=x86-pc CONFIG=default        # build/x86-pc/default/squirrel-bios.rom
make PLATFORM=riscv64-virt CONFIG=small    # build/riscv64-virt/small/squirrel-bios.bin
make PLATFORM=x86_64-uefi CONFIG=default   # build/x86_64-uefi/default/squirrel-bios.fd
```

`PLATFORM` is `x86-pc`, `x86_64-uefi`, `aarch64-virt`, `riscv64-virt` or `aarch64-uefi`, and `CONFIG`
names a file in `configs/<platform>/` (default `default`). Plain `make`
builds the same bytes as each platform's `default.cfg`; `make test`
checks that. Keys and format: [configs/README.md](configs/README.md).

Requires: `nasm`, a host `gcc` that targets x86-64 (`CC64`), `aarch64-linux-gnu-as`/`ld`/`objcopy`, `riscv64-linux-gnu-as`/`ld`/`objcopy`, and for `aarch64-uefi` and its test app `aarch64-linux-gnu-gcc` (`AARCH64_CC`) and `python3`.
Other cross prefixes work too: `make AARCH64=aarch64-elf- RISCV64=riscv64-elf-`.

## Test

```bash
make test
```

Builds everything, checks that each platform's `configs/*/default.cfg`
builds byte-for-byte what plain `make` builds (`config-default`), then
boots each firmware under QEMU with a small stand-in OS image from
`tests/` and checks the serial output:

| Case | Passes when |
|------|-------------|
| `x86-boot` | The boot sector runs with `DL = 0x80` and `IF = DF = 0`, the memory map at `ES:DI` has the required regions, sorted and not overlapping, and the VGA is in 80x25 text mode |
| `x86-one-sector` | The same checks for a boot sector without an SQBL header |
| `x86-no-drive` | With no disk, the firmware reports it (after trying master then slave) and halts |
| `x86-slave` | With the image only on the primary slave, the firmware boots it with `DL = 0x81` |
| `x86-acpi` | Finds the RSDP in `0xE0000`–`0xFFFFF`, checks its checksum, that the RSDT names a `FACP`, and that the FADT points at a `DSDT`, with every table checksum valid |
| `x86-smbios` | Finds the `_SM_` entry point in `0xF0000`–`0xFFFFF` and checks both checksums |
| `x86-bzimage` | A minimal `HdrS` bzImage is loaded at 1 MB; the PM payload checks the entry segments and registers and boot_params (setup header, `type_of_loader`, E820 table, `alt_mem_k`, command line) |
| `x86-bzimage-big` | The same with a 300 KB protected-mode kernel; its last dword arrives |
| `x86-bzimage-toobig` | A bzImage whose `syssize` is larger than RAM is refused before it is read |
| `x86-slave-absent` | With only a master, `INT 13h` `AH=0x08` returns CF set and `AH = 0x07` for `DL = 0x81`, and `DL = 1` / BDA `0x475` = 1 for the master |
| `x86-params-slave` | With a master and a slave, `AH=0x08` succeeds on both and reports 2 disks |
| `x86-optrom` | An option ROM at `0xD0000` is checksum-checked and its init prints `optrom=ok` |
| `x86-mem-low` | With `-m 8`, the map has 7 MB usable at 1 MB (the CMOS `0x30`/`0x31` path) |
| `x86-mem-high` | With `-m 5120`, the map has a non-empty usable entry at 4 GB (CMOS `0x5B`–`0x5D`) |
| `x86-floppy` | A 1.44 MB diskette in A: boots first with `DL = 0x00`; its boot sector checks `INT 13h` `AH=0x15`/`0x08`/`0x02`/`0x03` (multi-sector reads across heads, a write read back, a sector past the track refused) and `INT 1Eh` |
| `x86-floppy-nosig` | A diskette without `0xAA55` is reported, then the hard disk boots |
| `x86-edd` | A sparse 200 GiB disk: `INT 13h` `AH=41h` reports EDD 3.0, `AH=48h` the sector count (with the device path and its checksum, and in a short buffer), `AH=42h` reads across sector 2^28 and at 143 GiB (LBA48), `AH=43h` writes there and reads back, `AH=44h` takes the last sector and refuses the one after, a 9-sector read to an offset that spans three 4 KB pages arrives in place, and an LBA past 32 bits is refused |
| `x86-ahci` | The `x86-edd` disk on q35's AHCI controller, port 4 (past q35's empty CD on port 2): the same checks over `READ/WRITE DMA EXT`, and `AH=48h` names PCI `00:1F.2`, `SATA` and port 4 |
| `x86-ahci-osload` | The 192-sector S2OK kernel load from a SATA disk on q35's AHCI port 0 |
| `x86-nvme` | The `x86-edd` disk as namespace 1 of an NVMe controller in PCI slot 5: the same checks over NVMe Read/Write and PRP lists, and `AH=48h` names PCI `00:05.0`, `NVMe` and namespace 1 |
| `x86-virtio`, `x86-virtio-osload`, `x86-virtio-int13` | The `x86-edd` disk as a virtio-blk device in PCI slot 5 (the same checks, `AH=48h` naming PCI `00:05.0`), then the S2OK kernel load on q35 and the `INT 13h` boot sector from a virtio disk |
| `x86-netboot`, `x86-netboot-q35`, `x86-netboot-big` | No disk: a virtio-net device gets an address from QEMU's DHCP server and loads the boot file over TFTP to `0x7C00` (5000 bytes; 2048 on q35, with an empty last block; 200000, past block 255), entered with `DL = FFh` |
| `x86-netboot-missing`, `x86-netboot-huge` | A boot file the server does not have, and one too large for the memory below `0x40000`: each says why and the boot halts |
| `x86-vbe`, `x86-vbe-text` | VBE 3.0: controller and mode information, mode 112h (640x480x32) set with a linear frame buffer that the test fills through unreal mode, checked on QEMU's screen; then back to 80x25 text with `4F02h` `BX=3` |
| `x86-nvme-osload` | The 192-sector S2OK kernel load from NVMe on q35, past its empty AHCI controller |
| `x86-usb` | A PIIX3 UHCI controller with a hub on root port 1 (a keyboard and a USB disk behind it) and a mouse on root port 2: each device is listed with its address, place, IDs and class, and the IDE disk then boots with the memory map intact |
| `x86-usb-ohci` | An OHCI controller: a keyboard and a second hub behind a hub on root port 1, a tablet behind the second hub, a mouse on root port 3 |
| `x86-usb-ehci` | ICH9 EHCI with three UHCI companions on q35: EHCI keeps a high-speed disk and tablet, and a full-speed keyboard and a hub with a mouse go to the companions, which list them |
| `x86-usb-ehci-alone` | A lone EHCI controller on i440fx with a high-speed disk and tablet |
| `x86-usb-xhci` | A QEMU xHCI controller: a super-speed disk on USB 3 port 2, a hub chain (mouse, second hub, keyboard) on USB 2 port 5, a tablet on port 7 |
| `x86-usb-xhci-q35` | xHCI on q35 with a USB Attached SCSI disk at super speed |
| `x86-usb-xhci-slots` | Ten devices on an xHCI with eight slots: each one that is not a hub gives its slot back once listed |
| `x86-usb-kbd`, `x86-usb-kbd-hub` | A USB keyboard on UHCI (on a root port, then behind a hub) is kept running after POST; a key typed on it reaches `INT 16h` |
| `x86-usb-kbd-ohci`, `x86-usb-kbd-ehci` | The same with a keyboard behind a hub on OHCI, and with a full-speed keyboard on an ICH9 EHCI port that its UHCI companion keeps |
| `x86-usb-kbd-xhci` | The same keyboard with an xHCI listed after it: the xHCI works from POST-only memory and the keyboard still types |
| `x86-usb-mouse`, `x86-usb-mouse-ohci`, `x86-usb-mouse-xhci` | `INT 15h` `C2xx` on a USB mouse kept beside a keyboard (UHCI, OHCI, xHCI behind a hub): refusals, reset, type, init, status, then the handler gets one motion with the left button as PS/2 packets |
| `x86-usb-kbd-on-xhci`, `x86-usb-kbd-on-xhci-hub` | A keyboard on xHCI: twenty keys (its ring goes round more than twice), then one behind two hubs |
| `x86-usbdisk`, `x86-usbdisk-osload`, `x86-usbdisk-int13` | With no other disk, a bulk-only USB disk on xHCI boots: the boot-sector test, a Squirrel-OS/2-shaped kernel load on q35, and the `INT 13h` calls its boot sector makes |
| `x86-usbdisk-edd` | The 200 GiB EDD disk as a USB disk: reads past 2^28 and at 143 GiB, a write, an unaligned read over three pages, and `AH=48h` naming PCI 00:05.0, USB and device 1 |
| `x86-usbdisk-kbd` | The disk behind a hub with a keyboard and a mouse on the same xHCI: all three are kept, it boots, and twenty keys still arrive |
| `x86-usb-kbd-after-xhci` | The same keyboard with an xHCI listed before it, working from the EBDA during POST: the keyboard still starts and types |
| `x86-usb-q35` | Two ICH9 UHCI functions on q35, a hub behind a hub on the first and a tablet on the second; addresses run on across controllers |
| `x86-pci` | On i440fx with a virtio-rng: `INT 1Ah` `B101` returns `'PCI '`, `B102`/`B103` find devices by ID and class, POST placed I/O, 32-bit and 64-bit memory BARs and enabled decoding, INTA's Interrupt Line matches the PIIX3 route, config writes read back, and `$PIR` checks out |
| `x86-pci-q35` | The same on q35, booting from a PIIX3 IDE card; Interrupt Line is `0xFF` there |
| `x86-cd` | An El Torito no-emulation ISO on `-cdrom` boots with `DL = E0h`; its image checks it was loaded whole, `AH=41h`, the `AX=4B01h` packet (secondary master, block 20, segment `07C0h`), a 2-block `AH=42h` read and `AH=48h` (2048-byte blocks, 64 of them) |
| `x86-cd-slave` | The same CD as primary slave with no disk on the master |
| `x86-atapi` | With a disc too small for a boot record as primary master, the CD boot reports the failed read, then the firmware reports the ATAPI master and halts |
| `x86-bad-sector` | A sector without the `0xAA55` signature is reported and not jumped to |
| `x86-extra` | An SQBL header in the boot sector gets its extra sectors loaded before the jump |
| `x86-extra-map`, `x86-extra-boot`, `x86-extra-range` | An SQBL load onto the memory map, onto the boot sector, or past the EBDA (`0x92000`) is refused |
| `x86-osload` | A disk signed `S2OK` (the Squirrel-OS/2 layout, no SQBL header) is loaded at `0x8000` and entered there; `INT 15h` `E820` returns the memory map, and a malformed `INT 13h` packet returns with carry set |
| `x86-int13` | A disk without that signature is entered at `0x7C00`. The boot sector reads with `INT 13h` `AH=0x42` / `0x02` / `0x08` / `0x00`, and `INT 10h` `AH=0x0E` writes the VGA cell it checks |
| `arm-boot` | The `-kernel` image runs with a valid device tree in `x0`, `x1`–`x3` = 0, MMU and D-cache off, little-endian, `DAIF` masked |
| `arm-boot-el2` | The same at EL2 (`virtualization=on`) |
| `arm-too-big` | An image that would run past the end of RAM is refused before it is loaded |
| `arm-image-fixed`, `arm-image-reloc` | A Linux `Image` header is honoured: fixed placement loads at RAM base + `text_offset`, relocatable stays at `0x40200000` |
| `arm-initrd` | With `-initrd`, the initrd is loaded after the kernel and recorded in the device tree `/chosen` |
| `arm-image-short` | An `Image` header whose `image_size` is below the file size is refused |
| `arm-no-kernel` | Without `-kernel`, the firmware reports it and halts |
| `riscv-boot` | The `-kernel` image runs in S-mode (`next_mode=1`) on hart 0 (with `-smp 2`) with a valid device tree in `a1` and `a2 = 0`, and nothing traps |
| `riscv-ecall` | An `ecall` from the image reaches the SBI and returns `SBI_ERR_NOT_SUPPORTED` for an extension that does not exist |
| `riscv-deleg` | An illegal instruction traps to the image's own `stvec` (`deleg=ok`), and its `ecall` still reaches the SBI in M-mode |
| `riscv-sbi` | The SBI base extension reports spec 1.0 and the SquirrelBIOS implementation ID, probes base, TIME, IPI, RFENCE, HSM and SRST as present, and changes no register but `a0` and `a1` |
| `riscv-timer` | `sbi_set_timer` raises the supervisor timer interrupt once (`timer=ok`, never `timer=again`) |
| `riscv-ipi` | `sbi_send_ipi` to the calling hart raises its software interrupt (`ipi=ok`); to the parked (stopped) hart 1 it succeeds and raises nothing on the caller; to hart 2, which does not exist, it returns an error |
| `riscv-ipi-smp` | With hart 1 started, IPIs reach it by mask bit, base and base `-1` (which also reaches the caller); remote fences on both harts return, also when both harts fence each other at once |
| `riscv-fence-smp3` | With `-smp 3`, harts 0 and 1 both fence hart 2 many times at once, then each IPI reaches it |
| `riscv-boot-hart` | With `fw_dynamic_info.boot_hart = 2` (a shim stands in for QEMU's reset code), hart 2 boots, HSM reports it started, and it starts hart 0 |
| `riscv-aia` | On `-M virt,aia=aplic`, an image that drives the S-level APLIC gets the UART's interrupt (`aia=ok`), because the firmware delegated the sources |
| `riscv-aia-ipi` | `riscv-ipi-smp` on `-M virt,aia=aplic-imsic`: IPIs and fences still work |
| `riscv-numa` | With `-smp 4` in two NUMA nodes (two sockets, two CLINTs), harts 2 and 3 are started and fenced through the second CLINT (`harts=ok`) |
| `riscv-harts` | With `-smp 64`, hart 0 starts the other 63, each is reported started, and one fence whose mask names all 64 succeeds (`harts=64`) |
| `riscv-rfence` | `remote_sfence_vma` and `remote_fence_i` for the calling hart succeed; so does a mask that also names the stopped hart 1; a mask that names hart 2, which does not exist, returns an error |
| `riscv-hsm` | `sbi_hart_start` brings the parked hart 1 up in S-mode with the opaque value in `a1` (`hart1=ok a1=...`), and `hart_get_status` reports stopped, then started |
| `riscv-hsm-stop` | A started hart 1 turns on paging, calls `sbi_hart_stop`, never returns from it, is reported stopped, and starts again with `satp` and `sp` at 0 |
| `riscv-pmp` | A store, a load and a fetch in the firmware region (`0x80000000`–`0x801FFFFF`) each fault to the image's `stvec` (`pmp=ok`); its own memory and the SBI still work |
| `riscv-envcfg` | `cbo.zero`, `cbo.clean`/`flush`/`inval` and a timer set by writing `stimecmp` work from S-mode (`envcfg=ok`) |
| `riscv-dt` | The device tree the image gets reserves the firmware region (`/reserved-memory`, `no-map`) and still parses |
| `riscv-srst` | `sbi_system_reset` refuses a bad type, reason or function, then a shutdown makes QEMU exit with status 0 (`reset=ok`, never `reset=bad`) |
| `riscv-srst-reboot` | A cold reboot resets the machine, which `-no-reboot` turns into a QEMU exit with status 0 |
| `riscv-srst-fail` | A shutdown for a system failure makes QEMU exit with status 1 |
| `riscv-ras` | A fault in the SBI path prints `ras: hart=... mcause=... mepc=... mtval=... fatal`, parks the other started hart, prints `boot halted`, and shuts the machine down with QEMU exit status 1 |
| `riscv-no-kernel` | Without `-kernel`, the firmware reports it and halts |
| `config-x86-minimal` | The `x86-pc/minimal` build boots the `x86-boot` image with every hand-off check |
| `config-x86-minimal-noacpi` | The same build has no RSDP (the `x86-acpi` image prints `acpi=bad`) |
| `config-riscv-small` | The `riscv64-virt/small` build (8 harts tracked) boots the `riscv-boot` image |
| `x86_64-uefi` | The x86-64 firmware on q35: from the reset vector to long mode, its body copied to RAM at 2 MB, the CPU named, and the memory map read from QEMU's fw_cfg (128 MB); then the EFI system table, the UEFI memory map with the firmware's areas carved out, and AllocatePages/FreePages/AllocatePool/FreePool/GetMemoryMap checked through the boot services table |
| `x86_64-uefi-4g` | The same with `-m 4G`: RAM below the PCI hole and above 4 GB, 4096 MB in all |
| `x86_64-uefi-pc` | The same firmware on i440fx |
| `x86_64-app`, `x86_64-app-exit` | A hand-written PE32+ EFI application given through fw_cfg (`opt/org.squirrelworks/boot.efi`) is relocated, started, and checks the system table, its loaded image protocol, the relocation and the memory services; it returns `1234h`, then calls `Exit(5678h)`. Either way it is unloaded and its pages come back |
| `x86_64-app-4g` | With 4 GB the same image loads at its preferred base, 256 MB |
| `x86_64-con` | An EFI app writes through ConOut (UTF-8 and ANSI attributes and cursor moves on COM1), checks the 80x25 mode, waits on a 10 ms timer event, stalls, then reads `a`, `b`, up, delete and `q` typed on the serial line through `WaitForKey` and `ReadKeyStroke` |
| `x86_64-app-junk` | A file that is not PE32+ is refused with `EFI_LOAD_ERROR` |
| `x86_64-fat` | A GPT disk with a FAT32 ESP on virtio-blk: Block I/O, Simple File System, and boot of `\EFI\BOOT\BOOTX64.EFI` (the PE32+ test app) |
| `x86_64-gop` | An EFI app sets GOP mode 800×600×32, Blt-fills a rectangle, GetTime and Set/GetVariable, then ExitBootServices and ResetSystem (shutdown via isa-debug-exit, QEMU status 1) |
| `aarch64-uefi` | The AArch64 UEFI-class firmware on QEMU virt: copied from flash to RAM, identity map with caches on, PL011, PL031, PSCI and GICv2 found in the device tree, the UEFI memory map built from `/memory` (firmware as runtime code, the RTC as runtime MMIO), memory services checked, the device tree installed as a configuration table |
| `aarch64-uefi-app`, `aarch64-uefi-app-exit` | The stand-in EFI app in C (`tests/efi-app.c`, wrapped as PE32+ by `tests/mkpe.py`) through fw_cfg: relocated from 256 MB; checks the tables, loaded image, relocation, pool, memory map, ConOut, a timer event, GetTime, variables and the device tree table; returns `1234h`, then calls `Exit(5678h)` |
| `aarch64-uefi-kernel` | The same app as `-kernel` with `-append` and `-initrd`: the command line arrives as LoadOptions and the initrd through LocateDevicePath and LoadFile2 on the Linux initrd media path, as Linux's EFI stub reads them |
| `aarch64-uefi-ebs` | ExitBootServices, then ResetSystem(shutdown) through the runtime services: PSCI `SYSTEM_OFF`, QEMU exits 0 |
| `aarch64-uefi-el2` | The same at EL2 (`virtualization=on`) with GICv3 and 2 GB: PSCI through `smc`, the distributor set up and this CPU's redistributor awake |
| `aarch64-uefi-wrong-arch` | An x86-64 PE32+ image is refused with `EFI_UNSUPPORTED` |

Needs `qemu-system-i386`, `qemu-system-x86_64`, `qemu-system-aarch64` and `qemu-system-riscv64`.
Serial logs are kept in `build/tests/*.log`. `SMOKE_TIMEOUT` (seconds,
default 10) sets how long each case may take. A case can also require
QEMU to exit by itself with a given status (`@exit=N` in `tests/smoke.sh`).

CI runs exactly this check: `.github/workflows/make-test.yml` installs
`nasm`, the AArch64 and RISC-V cross toolchains and the three QEMU
system emulators on Ubuntu 24.04, then runs `make test` on every push
and pull request. The serial logs are uploaded as the `smoke-logs`
artifact.

### Linux on the x86 BIOS (optional)

```bash
make linux-x86                        # prints PASS linux-x86
```

Boots Ubuntu 24.04's amd64 kernel (6.8) from an IDE disk: the firmware
reads the bzImage over ATA PIO, copies the 14 MB protected-mode kernel
to 1 MB and enters it through the 32-bit boot protocol. It passes when
the kernel's banner, the firmware's command line, the E820 map with
usable RAM at 1 MB, and the kernel's memory summary are all on COM1.
There is no initrd yet, so it stops short of a shell. The pinned package
(about 15 MB, checked against its SHA-256) is kept in `build/linux-x86/`.
Not part of `make test` or CI. `LINUX_TIMEOUT` and `LINUX_MEM` work as
below; the log is `build/linux-x86/linux-x86.log`.

### Linux to a shell through UEFI (AArch64, optional)

```bash
make linux-arm                        # prints PASS linux-arm
tests/arm-linux.sh --interactive      # the same boot, with the shell for you
LINUX_MACHINE=virt,virtualization=on,gic-version=3 LINUX_CPU=max LINUX_SMP=2 tests/arm-linux.sh
```

Boots Ubuntu 24.04's arm64 kernel (6.8) on the AArch64 UEFI-class
firmware (`arm/uefi/squirrel-bios.fd`) with a busybox initramfs. QEMU's
`-kernel`, `-initrd` and `-append` reach the firmware through fw_cfg;
the kernel's EFI stub runs as an EFI application, gets the initrd
through LoadFile2, calls ExitBootServices and SetVirtualAddressMap, and
Linux uses the runtime services afterwards. The check passes when the
shell prompt appears with `/sys/firmware/efi` present, a typed command
runs, and `poweroff` makes QEMU exit with status 0 (PSCI). Packages
(about 20 MB, pinned SHA-256) are kept in `build/linux-arm/`; needs
`curl`, `cpio` and `qemu-system-aarch64`. Not part of `make test` or CI.
`LINUX_MACHINE`, `LINUX_CPU`, `LINUX_SMP`, `LINUX_MEM` and
`LINUX_TIMEOUT` change the run; the log is `build/linux-arm/linux-arm.log`.

### Linux to a shell (RISC-V, optional)

```bash
make linux-shell                      # scripted check, prints PASS linux-shell
tests/linux-shell.sh --interactive    # the same boot, with the shell for you
```

Boots Ubuntu 24.04's riscv64 kernel (6.8) on the RISC-V firmware with a
tiny busybox initramfs, and passes when the shell prompt appears, a
command typed at it runs, and `poweroff` makes QEMU exit with status 0.
Nothing is added to the repo: the script downloads two pinned Ubuntu
packages (about 40 MB, checked against their SHA-256) into
`build/linux/` and builds the initramfs there. Needs `curl`, `cpio` and
`qemu-system-riscv64`. It is not part of `make test` or CI.
`LINUX_TIMEOUT` (default 300 seconds), `LINUX_SMP` (default 1 hart) and
`LINUX_MEM` (default `512M`) change the run; the serial log is
`build/linux/linux-shell.log`.

```bash
make linux-big                        # 64 harts, 64 GB, 2 NUMA nodes: PASS linux-shell
make linux-big BIG_SMP=8 BIG_MEM=16G  # a smaller node
make linux-big BIG_NUMA=1             # one socket
```

`make linux-big` is the same boot on a QEMU machine the size of one
server node, with Ubuntu 25.04's 6.14 kernel (24.04's 6.8 is built for
at most 32 harts), and also requires every hart to come online. It is
the yardstick for the large-node milestone in [ROADMAP.md](ROADMAP.md).
It also needs `zstd`, and downloads about 15 MB more. QEMU maps the
guest RAM without reserving it, so the host does not need 64 GB.
`LINUX_KERNEL=6.14` picks that kernel for `tests/linux-shell.sh` too.
By default the node is two NUMA nodes of 32 harts and 32 GB each (QEMU
makes them two sockets), and the check also requires Linux to bring up
both nodes (`LINUX_NUMA` in the script). The node also has a
virtio-net card and an NVMe drive, and the check requires Linux to list
both under `/sys/bus/pci/devices` (`LINUX_DEVICES` and `LINUX_EXPECT`
in the script). `LINUX_AIA=aplic make
linux-big` runs the same node with QEMU's AIA interrupt controllers.

```bash
make linux-stress                          # 1, 2, 3, 4 and 8 harts, 3 boots each
STRESS_HOTPLUG=1 make linux-stress         # with CPUs going offline and online
```

`make linux-stress` boots the same kernel many times and runs
`tests/smpstress.c` in it: two threads per hart that map and unmap
memory, rewrite and flush code, ping-pong through pipes and sleep, so the
harts keep asking each other for remote fences and IPIs through the
firmware. A boot fails if `smpstress` sees stale memory or instructions,
or the kernel logs a failed SBI call, an oops or an RCU stall. Needs
`riscv64-linux-gnu-gcc`; the knobs are at the top of
`tests/linux-stress.sh`.

## Run

```bash
# x86 (with a boot sector image); firmware messages appear on COM1
qemu-system-i386 -bios x86/squirrel-bios.rom -drive format=raw,file=boot.bin,if=ide -serial stdio

# Squirrel-OS/2 hard disk. The shell banner and "> " prompt are on the VGA.
# tests/os-boot.sh checks them; it is not part of make test.
qemu-system-i386 -bios x86/squirrel-bios.rom -drive format=raw,file=os.img,if=ide -serial stdio
tests/os-boot.sh path/to/os.img

# x86-64 UEFI-class firmware (in progress): runs an EFI application given through fw_cfg
qemu-system-x86_64 -M q35 -bios x86_64/squirrel-bios.fd -serial stdio \
    -fw_cfg name=opt/org.squirrelworks/boot.efi,file=app.efi

# AArch64 (image linked at 0x40200000)
qemu-system-aarch64 -M virt -cpu cortex-a57 -nographic -bios arm/squirrel-bios.bin -kernel image.bin

# RISC-V (image linked at 0x80200000)
qemu-system-riscv64 -M virt -nographic -bios riscv/squirrel-bios.bin -kernel image.bin
```

See `common/handoff.md` for the contract each architecture provides.

## Roadmap

[ROADMAP.md](ROADMAP.md) is where this firmware is going.
[TODO.md](TODO.md) is the list of small changes that get there.
One item in that list is one PR, branched from `main`.
