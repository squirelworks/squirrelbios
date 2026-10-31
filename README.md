# SquirrelBIOS

Platform-specific firmware for Squirrel-OS/2.  
Original code. Hands off to the OS after early init.

```
x86/        64 KB ROM — VGA banner + ATA hand-off to 0x7C00
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
| `x86-boot` | The boot sector runs with `DL = 0x80` and `IF = DF = 0`, and the memory map at `ES:DI` has the required regions, sorted and not overlapping |
| `x86-no-drive` | With no disk, the firmware reports it and halts |
| `x86-mem-low` | With `-m 8`, the map has 7 MB usable at 1 MB (the CMOS `0x30`/`0x31` path) |
| `x86-mem-high` | With `-m 5120`, the map has a non-empty usable entry at 4 GB (CMOS `0x5B`–`0x5D`) |
| `x86-atapi` | With a CD-ROM as primary master, the firmware reports ATAPI and halts |
| `arm-boot` | The `-kernel` image runs with a valid device tree in `x0` and `x1`–`x3` = 0 |
| `arm-no-kernel` | Without `-kernel`, the firmware reports it and halts |
| `riscv-boot` | The `-kernel` image runs in S-mode (`next_mode=1`) on hart 0 (with `-smp 2`) with a valid device tree in `a1` and `a2 = 0`, and nothing traps |
| `riscv-ecall` | An `ecall` from the image reaches the SBI and returns `SBI_ERR_NOT_SUPPORTED` for an extension that does not exist |
| `riscv-deleg` | An illegal instruction traps to the image's own `stvec` (`deleg=ok`), and its `ecall` still reaches the SBI in M-mode |
| `riscv-sbi` | The SBI base extension reports spec 1.0 and the SquirrelBIOS implementation ID, probes only base as present, and changes no register but `a0` and `a1` |
| `riscv-no-kernel` | Without `-kernel`, the firmware reports it and halts |

Needs `qemu-system-i386`, `qemu-system-aarch64` and `qemu-system-riscv64`.
Serial logs are kept in `build/tests/*.log`. `SMOKE_TIMEOUT` (seconds,
default 10) sets how long each case may take.

CI runs exactly this check: `.github/workflows/make-test.yml` installs
`nasm`, the AArch64 and RISC-V cross toolchains and the three QEMU
system emulators on Ubuntu 24.04, then runs `make test` on every push
and pull request. The serial logs are uploaded as the `smoke-logs`
artifact.

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
