# SquirrelBIOS

Platform-specific firmware for Squirrel-OS/2.  
Original code. Hands off to the OS after early init.

```
x86/        64 KB ROM — VGA text console + ATA hand-off to 0x7C00 (plus SQBL extra sectors)
arm/        AArch64 QEMU virt — PL011 banner + fw_cfg kernel load, DTB in x0
riscv/      RISC-V QEMU virt — UART banner + S-mode entry to -kernel, DTB in a1
tests/      stand-in OS images and smoke tests (make test)
common/     hand-off contract
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
| `x86-no-drive` | With no disk, the firmware reports it and halts |
| `x86-mem-low` | With `-m 8`, the map has 7 MB usable at 1 MB (the CMOS `0x30`/`0x31` path) |
| `x86-mem-high` | With `-m 5120`, the map has a non-empty usable entry at 4 GB (CMOS `0x5B`–`0x5D`) |
| `x86-atapi` | With a CD-ROM as primary master, the firmware reports ATAPI and halts |
| `x86-bad-sector` | A sector without the `0xAA55` signature is reported and not jumped to |
| `x86-extra` | An SQBL header in the boot sector gets its extra sectors loaded before the jump |
| `x86-extra-map`, `x86-extra-boot`, `x86-extra-range` | An SQBL load onto the memory map, onto the boot sector, or past `0xA0000` is refused |
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
| `riscv-ipi` | `sbi_send_ipi` to the calling hart raises its software interrupt (`ipi=ok`); to the parked hart 1 it returns an error |
| `riscv-ipi-smp` | With hart 1 started, IPIs reach it by mask bit, base and base `-1` (which also reaches the caller); remote fences on both harts return, also when both harts fence each other at once |
| `riscv-fence-smp3` | With `-smp 3`, harts 0 and 1 both fence hart 2 many times at once, then each IPI reaches it |
| `riscv-harts` | With `-smp 64`, hart 0 starts the other 63, each is reported started, and one fence whose mask names all 64 succeeds (`harts=64`) |
| `riscv-rfence` | `remote_sfence_vma` and `remote_fence_i` for the calling hart succeed; a mask that also names the parked hart 1 returns an error |
| `riscv-hsm` | `sbi_hart_start` brings the parked hart 1 up in S-mode with the opaque value in `a1` (`hart1=ok a1=...`), and `hart_get_status` reports stopped, then started |
| `riscv-hsm-stop` | A started hart 1 turns on paging, calls `sbi_hart_stop`, never returns from it, is reported stopped, and starts again with `satp` and `sp` at 0 |
| `riscv-pmp` | A store, a load and a fetch in the firmware region (`0x80000000`–`0x801FFFFF`) each fault to the image's `stvec` (`pmp=ok`); its own memory and the SBI still work |
| `riscv-envcfg` | `cbo.zero`, `cbo.clean`/`flush`/`inval` and a timer set by writing `stimecmp` work from S-mode (`envcfg=ok`) |
| `riscv-dt` | The device tree the image gets reserves the firmware region (`/reserved-memory`, `no-map`) and still parses |
| `riscv-srst` | `sbi_system_reset` refuses a bad type, reason or function, then a shutdown makes QEMU exit with status 0 (`reset=ok`, never `reset=bad`) |
| `riscv-srst-reboot` | A cold reboot resets the machine, which `-no-reboot` turns into a QEMU exit with status 0 |
| `riscv-srst-fail` | A shutdown for a system failure makes QEMU exit with status 1 |
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
make linux-big                        # 64 harts, 64 GB, prints PASS linux-shell
make linux-big BIG_SMP=8 BIG_MEM=16G  # a smaller node
```

`make linux-big` is the same boot on a QEMU machine the size of one
server node, with Ubuntu 25.04's 6.14 kernel (24.04's 6.8 is built for
at most 32 harts), and also requires every hart to come online. It is
the yardstick for the large-node milestone in [ROADMAP.md](ROADMAP.md).
It also needs `zstd`, and downloads about 15 MB more. QEMU maps the
guest RAM without reserving it, so the host does not need 64 GB.
`LINUX_KERNEL=6.14` picks that kernel for `tests/linux-shell.sh` too.

## Run

```bash
# x86 (with a boot sector image); firmware messages appear on COM1
qemu-system-i386 -bios x86/squirrel-bios.rom -drive format=raw,file=boot.bin,if=ide -serial stdio

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
