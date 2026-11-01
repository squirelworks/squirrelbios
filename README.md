# SquirrelBIOS

Original firmware for Squirrel-OS/2 and QEMU's PC / AArch64 `virt` /
RISC-V `virt` machines. **v0.3.0** is the hand-off release (load image,
jump, tested contract). **v1.0.0** is a comprehensive x86 PC BIOS —
see [ROADMAP.md](ROADMAP.md) and [TODO.md](TODO.md).

```
x86/        64 KB ROM — VGA text + ATA boot + INT 13h/10h/15h E820 (growing toward a full PC BIOS)
arm/        AArch64 QEMU virt — PL011 banner + fw_cfg kernel load, DTB in x0
riscv/      RISC-V QEMU virt — UART banner + S-mode entry to -kernel, DTB in a1, SBI
tests/      stand-in OS images and smoke tests (make test)
common/     hand-off contract
release-notes/  tagged releases (v0.2.0, v0.3.0, …)
ROADMAP.md  goals, principles, milestones
TODO.md     small tasks, one per PR
```

## Build

```bash
make          # all
make x86
make arm
make riscv
make clean    # remove all build and test outputs
```

Requires: `nasm`, `aarch64-linux-gnu-as`/`ld`/`objcopy`, `riscv64-linux-gnu-as`/`ld`/`objcopy`.
Other cross prefixes work too: `make AARCH64=aarch64-elf- RISCV64=riscv64-elf-`.

## Test

```bash
make test
```

Builds everything, then boots each firmware under QEMU with a small
stand-in OS image from `tests/` and checks the serial output:

| Case | Passes when |
|------|-------------|
| `x86-boot` | The boot sector runs with `DL = 0x80` and `IF = DF = 0`, the memory map at `ES:DI` has the required regions, sorted and not overlapping, and the VGA is in 80x25 text mode |
| `x86-one-sector` | The same checks for a boot sector without an SQBL header |
| `x86-no-drive` | With no disk, the firmware reports it (after trying master then slave) and halts |
| `x86-slave` | With the image only on the primary slave, the firmware boots it with `DL = 0x81` |
| `x86-acpi` | Finds the RSDP in `0xE0000`–`0xFFFFF`, checks its checksum and that the RSDT names a `FACP` |
| `x86-smbios` | Finds the `_SM_` entry point in `0xF0000`–`0xFFFFF` and checks both checksums |
| `x86-mem-low` | With `-m 8`, the map has 7 MB usable at 1 MB (the CMOS `0x30`/`0x31` path) |
| `x86-mem-high` | With `-m 5120`, the map has a non-empty usable entry at 4 GB (CMOS `0x5B`–`0x5D`) |
| `x86-atapi` | With a CD-ROM as primary master, the firmware reports ATAPI and halts |
| `x86-bad-sector` | A sector without the `0xAA55` signature is reported and not jumped to |
| `x86-extra` | An SQBL header in the boot sector gets its extra sectors loaded before the jump |
| `x86-extra-map`, `x86-extra-boot`, `x86-extra-range` | An SQBL load onto the memory map, onto the boot sector, or past `0xA0000` is refused |
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

Needs `qemu-system-i386`, `qemu-system-aarch64` and `qemu-system-riscv64`.
Serial logs are kept in `build/tests/*.log`. `SMOKE_TIMEOUT` (seconds,
default 10) sets how long each case may take. A case can also require
QEMU to exit by itself with a given status (`@exit=N` in `tests/smoke.sh`).

CI runs exactly this check: `.github/workflows/make-test.yml` installs
`nasm`, the AArch64 and RISC-V cross toolchains and the three QEMU
system emulators on Ubuntu 24.04, then runs `make test` on every push
and pull request. The serial logs are uploaded as the `smoke-logs`
artifact.

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
